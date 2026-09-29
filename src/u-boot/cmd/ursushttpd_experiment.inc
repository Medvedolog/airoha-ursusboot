/* SPDX-License-Identifier: GPL-2.0+
 *
 * UrsusBoot upstream lwIP HTTPD transport A/B experiment.
 *
 * Purpose: keep the Ursus upload/session/validation model intact while
 * replacing only the raw HTTP/TCP framing layer. This file deliberately
 * exposes status + upload begin/chunk endpoints only.
 */

#include <lwip/apps/httpd.h>
#include <lwip/apps/fs.h>

#define URSUS_HTTPD_REPLY_URI "/ursus-reply.json"

enum ursus_httpd_post_mode {
    URSUS_HTTPD_POST_NONE = 0,
    URSUS_HTTPD_POST_BEGIN,
    URSUS_HTTPD_POST_CHUNK,
};

struct ursus_httpd_post_state {
    void *connection;
    enum ursus_httpd_post_mode mode;
    enum ursus_upload_kind kind;
    size_t content_len;
    size_t received;
    size_t offset;
    size_t total;
    char generation[URSUS_GENERATION_MAX];
    bool failed;
};

struct ursus_httpd_file_owner {
    char *allocation;
};

static struct ursus_httpd_post_state ursus_httpd_post;
static int ursus_httpd_reply_status = 200;

static const char *ursus_httpd_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 501: return "Not Implemented";
    default: return "Error";
    }
}

static int ursus_httpd_open_mem(struct fs_file *file, int status,
                                const char *ctype, const char *body)
{
    struct ursus_httpd_file_owner *owner;
    size_t body_len = strlen(body);
    size_t cap = body_len + 224;
    char *wire;
    int head;

    owner = calloc(1, sizeof(*owner));
    if (!owner)
        return 0;
    wire = malloc(cap);
    if (!wire) {
        free(owner);
        return 0;
    }

    head = snprintf(wire, cap,
                    "HTTP/1.1 %d %s\r\n"
                    "Content-Type: %s\r\n"
                    "Cache-Control: no-store\r\n"
                    "Connection: close\r\n"
                    "Content-Length: %u\r\n\r\n",
                    status, ursus_httpd_reason(status), ctype,
                    (unsigned int)body_len);
    if (head <= 0 || (size_t)head + body_len >= cap) {
        free(wire);
        free(owner);
        return 0;
    }
    memcpy(wire + head, body, body_len);

    file->data = wire;
    file->len = head + body_len;
    file->index = file->len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
    file->pextension = owner;
    owner->allocation = wire;
    return 1;
}

int fs_open_custom(struct fs_file *file, const char *name)
{
    if (!strcmp(name, "/") || !strcmp(name, "/index.html"))
        return ursus_httpd_open_mem(file, 200, "text/html; charset=utf-8",
                                    ursus_root_body);
    if (!strcmp(name, "/logo.svg"))
        return ursus_httpd_open_mem(file, 200, "image/svg+xml",
                                    ursus_logo_body);
    if (!strcmp(name, "/favicon.svg"))
        return ursus_httpd_open_mem(file, 200, "image/svg+xml",
                                    ursus_favicon_body);
    if (!strcmp(name, "/api/status")) {
        ursus_build_status();
        return ursus_httpd_open_mem(file, 200, "application/json",
                                    ursus_status_body);
    }
    if (!strcmp(name, "/api/log") || !strcmp(name, "/api/operation-log"))
        return ursus_httpd_open_mem(file, 200, "text/plain; charset=utf-8",
                                    ursus_web_log);
    if (!strcmp(name, URSUS_HTTPD_REPLY_URI))
        return ursus_httpd_open_mem(file, ursus_httpd_reply_status,
                                    "application/json", ursus_reply_body);
    return 0;
}

void fs_close_custom(struct fs_file *file)
{
    struct ursus_httpd_file_owner *owner = file ? file->pextension : NULL;

    if (!owner)
        return;
    free(owner->allocation);
    free(owner);
    file->pextension = NULL;
}

static enum ursus_upload_kind ursus_httpd_uri_kind(const char *uri, bool *begin)
{
    *begin = false;
    if (!strcmp(uri, "/api/firmware-begin")) {
        *begin = true; return URSUS_UPLOAD_FIRMWARE;
    }
    if (!strcmp(uri, "/api/firmware-chunk"))
        return URSUS_UPLOAD_FIRMWARE;
    if (!strcmp(uri, "/api/initramfs-begin")) {
        *begin = true; return URSUS_UPLOAD_INITRAMFS;
    }
    if (!strcmp(uri, "/api/initramfs-chunk"))
        return URSUS_UPLOAD_INITRAMFS;
    if (!strcmp(uri, "/api/ubi-preloader-begin")) {
        *begin = true; return URSUS_UPLOAD_UBI_PRELOADER;
    }
    if (!strcmp(uri, "/api/ubi-preloader-chunk"))
        return URSUS_UPLOAD_UBI_PRELOADER;
    if (!strcmp(uri, "/api/ursus-fip-begin")) {
        *begin = true; return URSUS_UPLOAD_URSUS_FIP;
    }
    if (!strcmp(uri, "/api/ursus-fip-chunk"))
        return URSUS_UPLOAD_URSUS_FIP;
    if (!strcmp(uri, "/api/vanilla-fip-begin")) {
        *begin = true; return URSUS_UPLOAD_VANILLA_FIP;
    }
    if (!strcmp(uri, "/api/vanilla-fip-chunk"))
        return URSUS_UPLOAD_VANILLA_FIP;
    return URSUS_UPLOAD_NONE;
}

static int ursus_httpd_reject(int status, const char *reason_class,
                              const char *reason, size_t expected, size_t got)
{
    ursus_httpd_reply_status = status;
    snprintf(ursus_reply_body, sizeof(ursus_reply_body),
             "{\"result\":\"REJECTED\",\"reason_class\":\"%s\","
             "\"reason\":\"%s\",\"expected\":%u,\"got\":%u}\n",
             reason_class, reason,
             (unsigned int)expected, (unsigned int)got);
    ursus_logf("HTTPD UPLOAD ERROR: class=%s reason=%s expected=%u got=%u\n",
               reason_class, reason,
               (unsigned int)expected, (unsigned int)got);
    return status;
}

static int ursus_httpd_begin_upload(enum ursus_upload_kind kind,
                                    const char *headers)
{
    struct ursus_upload_session *u = ursus_upload_session(kind);
    size_t total = 0;
    size_t max_len = (kind == URSUS_UPLOAD_FIRMWARE || kind == URSUS_UPLOAD_INITRAMFS) ?
                     URSUS_STAGE_MAX :
                     kind == URSUS_UPLOAD_UBI_PRELOADER ?
                     URSUS_UBI_PRELOADER_MAX : URSUS_FIP_STAGE_MAX;
    ulong addr = (kind == URSUS_UPLOAD_FIRMWARE || kind == URSUS_UPLOAD_INITRAMFS) ?
                 URSUS_STAGE_ADDR :
                 kind == URSUS_UPLOAD_UBI_PRELOADER ?
                 URSUS_UBI_PRELOADER_ADDR : URSUS_FIP_STAGE_ADDR;
    char generation[URSUS_GENERATION_MAX];
    char encoded[URSUS_FILENAME_MAX * 3];
    char filename[URSUS_FILENAME_MAX];

    if (!ursus_parse_header_ulong(headers, "X-Ursus-Total", &total) || !total)
        return ursus_httpd_reject(400, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "missing or invalid declared size", 1, total);
    if (total > max_len)
        return ursus_httpd_reject(413, URSUS_RC_IMAGE_TOO_LARGE,
                                  "declared image exceeds staging capacity",
                                  max_len, total);
    if (!ursus_parse_header_value(headers, "X-Ursus-Generation",
                                  generation, sizeof(generation)) ||
        !ursus_generation_valid(generation))
        return ursus_httpd_reject(400, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "missing or invalid upload generation", 0, 0);
    if (!ursus_parse_header_value(headers, "X-Ursus-Filename",
                                  encoded, sizeof(encoded)) ||
        !ursus_decode_filename(encoded, filename, sizeof(filename)))
        return ursus_httpd_reject(400, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "missing or invalid filename", 0, 0);

    memset(u, 0, sizeof(*u));
    u->kind = kind;
    u->addr = addr;
    u->max_len = max_len;
    u->total = total;
    u->active = true;
    snprintf(u->generation, sizeof(u->generation), "%s", generation);
    snprintf(u->filename, sizeof(u->filename), "%s", filename);

    if (kind == URSUS_UPLOAD_FIRMWARE) {
        ursus_reset_image_state();
        ursus_reset_expert_state();
        memset(&ursus_init_upload, 0, sizeof(ursus_init_upload));
        ursus_log_reset();
    } else if (kind == URSUS_UPLOAD_INITRAMFS) {
        ursus_reset_expert_state();
        ursus_reset_image_state();
        memset(&ursus_fw_upload, 0, sizeof(ursus_fw_upload));
    } else if (kind == URSUS_UPLOAD_UBI_PRELOADER) {
        ursus_preloader_valid = false;
    } else {
        ursus_fip_upload_valid = false;
        ursus_vanilla_upload_valid = false;
        memset(kind == URSUS_UPLOAD_VANILLA_FIP ?
               &ursus_fip_upload : &ursus_vanilla_upload,
               0, sizeof(struct ursus_upload_session));
    }

    ursus_logf("HTTPD UPLOAD BEGIN: kind=%s generation=%s filename=%s declared=%u\n",
               ursus_upload_kind_name(kind), u->generation, u->filename,
               (unsigned int)u->total);
    ursus_httpd_reply_status = 200;
    snprintf(ursus_reply_body, sizeof(ursus_reply_body),
             "{\"result\":\"UPLOAD_READY\",\"reason_class\":\"%s\","
             "\"generation\":\"%s\",\"filename\":\"%s\","
             "\"declared_size\":%u,\"received\":0}\n",
             URSUS_RC_OK, u->generation, u->filename,
             (unsigned int)u->total);
    return 200;
}

static int ursus_httpd_prepare_chunk(enum ursus_upload_kind kind,
                                     const char *headers, size_t content_len)
{
    struct ursus_upload_session *u = ursus_upload_session(kind);
    size_t off = 0, total = 0;
    char generation[URSUS_GENERATION_MAX];

    if (!content_len || content_len > URSUS_UPLOAD_CHUNK_MAX)
        return ursus_httpd_reject(413, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "chunk must be 1..65536 bytes",
                                  URSUS_UPLOAD_CHUNK_MAX, content_len);
    if (!ursus_parse_header_ulong(headers, "X-Ursus-Offset", &off) ||
        !ursus_parse_header_ulong(headers, "X-Ursus-Total", &total) ||
        !ursus_parse_header_value(headers, "X-Ursus-Generation",
                                  generation, sizeof(generation)))
        return ursus_httpd_reject(400, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "chunk headers are incomplete",
                                  u ? u->received : 0, off);
    if (!u || !u->active)
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "no active upload generation; begin upload first",
                                  0, off);
    if (strcmp(generation, u->generation))
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "stale or wrong upload generation",
                                  u->received, off);
    if (total != u->total)
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "declared size changed during upload",
                                  u->total, total);
    if (off != u->received)
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "unexpected chunk offset",
                                  u->received, off);
    if (off > total || content_len > total - off)
        return ursus_httpd_reject(413, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "chunk exceeds declared image size",
                                  total, off + content_len);
    if (off + content_len > u->max_len)
        return ursus_httpd_reject(413, URSUS_RC_IMAGE_TOO_LARGE,
                                  "chunk exceeds staging capacity",
                                  u->max_len, off + content_len);

    ursus_httpd_post.offset = off;
    ursus_httpd_post.total = total;
    ursus_httpd_post.content_len = content_len;
    snprintf(ursus_httpd_post.generation,
             sizeof(ursus_httpd_post.generation), "%s", generation);
    return 200;
}

static int ursus_httpd_complete_upload(void)
{
    struct ursus_upload_session *u =
        ursus_upload_session(ursus_httpd_post.kind);
    size_t done;
    int vret = 0;

    if (!u || !u->active)
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "upload session disappeared while receiving chunk",
                                  0, ursus_httpd_post.offset);
    if (strcmp(ursus_httpd_post.generation, u->generation))
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "upload generation changed while receiving chunk",
                                  u->received, ursus_httpd_post.offset);
    if (ursus_httpd_post.received != ursus_httpd_post.content_len)
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "chunk connection ended before Content-Length",
                                  ursus_httpd_post.content_len,
                                  ursus_httpd_post.received);
    if (u->received != ursus_httpd_post.offset)
        return ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                                  "upload offset changed while receiving chunk",
                                  u->received, ursus_httpd_post.offset);

    done = ursus_httpd_post.offset + ursus_httpd_post.content_len;
    u->received = done;

    if (done != u->total) {
        ursus_httpd_reply_status = 200;
        snprintf(ursus_reply_body, sizeof(ursus_reply_body),
                 "{\"result\":\"CHUNK_OK\",\"reason_class\":\"%s\","
                 "\"complete\":false,\"generation\":\"%s\","
                 "\"filename\":\"%s\",\"received\":%u,"
                 "\"declared_size\":%u}\n",
                 URSUS_RC_OK, u->generation, u->filename,
                 (unsigned int)u->received, (unsigned int)u->total);
        return 200;
    }

    u->active = false;
    ursus_logf("HTTPD UPLOAD COMPLETE: kind=%s generation=%s filename=%s declared=%u received=%u\n",
               ursus_upload_kind_name(ursus_httpd_post.kind),
               u->generation, u->filename,
               (unsigned int)u->total, (unsigned int)u->received);

    if (ursus_httpd_post.kind == URSUS_UPLOAD_FIRMWARE) {
        vret = ursus_validate_staged(u->addr, u->total, true);
        ursus_build_plan_reply(vret);
    } else if (ursus_httpd_post.kind == URSUS_UPLOAD_INITRAMFS) {
        vret = ursus_validate_expert_fit(u->addr, u->total);
        ursus_build_expert_reply(vret);
    } else if (ursus_httpd_post.kind == URSUS_UPLOAD_UBI_PRELOADER) {
        vret = ursus_ubi_validate_preloader(u->addr, u->total);
        if (!vret)
            vret = ursus_ubi_prepare_bl2_candidate(u->addr, u->total);
        ursus_preloader_valid = !vret && ursus_ubi_bl2_candidate_valid();
        snprintf(ursus_reply_body, sizeof(ursus_reply_body),
                 "{\"result\":\"%s\",\"complete\":true,"
                 "\"generation\":\"%s\",\"filename\":\"%s\","
                 "\"declared_size\":%u,\"received\":%u,"
                 "\"ubi_bl2_candidate_valid\":%s,"
                 "\"reason_class\":\"%s\",\"reason\":\"%s\"}\n",
                 vret ? "REJECTED" : "VALID",
                 u->generation, u->filename,
                 (unsigned int)u->total, (unsigned int)u->received,
                 ursus_ubi_bl2_candidate_valid() ? "true" : "false",
                 vret ? URSUS_RC_BAD_CONTAINER : URSUS_RC_OK,
                 vret ? "UBI preloader or full 128 KiB BL2 candidate validation failed" :
                        "UBI preloader and full 128 KiB BL2 candidate validated");
    } else if (ursus_httpd_post.kind == URSUS_UPLOAD_VANILLA_FIP) {
        bool stock_limit = strcmp(ursus_current_layout, "OPENWRT_UBI") != 0;
        vret = ursus_vanilla_fip_validate(u->addr, u->total, stock_limit);
        if (!vret)
            vret = ursus_ubi_installed_bl2_matches_pin() ? -ENOEXEC : 0;
        ursus_vanilla_upload_valid = !vret;
        snprintf(ursus_reply_body, sizeof(ursus_reply_body),
                 "{\"result\":\"%s\",\"complete\":true,"
                 "\"generation\":\"%s\",\"filename\":\"%s\","
                 "\"declared_size\":%u,\"received\":%u,"
                 "\"layout\":\"%s\",\"vanilla_fip_pinned\":%s,"
                 "\"reason_class\":\"%s\",\"reason\":\"%s\"}\n",
                 vret ? "REJECTED" : "VALID",
                 u->generation, u->filename,
                 (unsigned int)u->total, (unsigned int)u->received,
                 stock_limit ? "STOCK" : "UBI",
                 ursus_vanilla_fip_pinned() ? "true" : "false",
                 vret ? (vret == -ENODEV ?
                         URSUS_RC_DEVICE_MISMATCH : URSUS_RC_BAD_CONTAINER) :
                        URSUS_RC_OK,
                 !vret ? "Pinned Vanilla U-Boot FIP validated for this board; installed BL2 is the pinned fast BL2" :
                 vret == -ENODEV ? "Not a Vanilla OpenWrt U-Boot FIP for this board" :
                 vret == -ENOEXEC ? "Installed BL2 is not the pinned fast BL2 of this UrsusBoot" :
                 stock_limit ? "Vanilla U-Boot requires the OpenWrt UBI layout" :
                 "Not the Vanilla FIP pinned into this UrsusBoot");
    } else {
        bool stock_limit = strcmp(ursus_current_layout, "OPENWRT_UBI") != 0;
        vret = ursus_fip_validate(u->addr, u->total, stock_limit);
        ursus_fip_upload_valid = !vret;
        snprintf(ursus_reply_body, sizeof(ursus_reply_body),
                 "{\"result\":\"%s\",\"complete\":true,"
                 "\"generation\":\"%s\",\"filename\":\"%s\","
                 "\"declared_size\":%u,\"received\":%u,"
                 "\"layout\":\"%s\",\"reason_class\":\"%s\","
                 "\"reason\":\"%s\"}\n",
                 vret ? "REJECTED" : "VALID",
                 u->generation, u->filename,
                 (unsigned int)u->total, (unsigned int)u->received,
                 stock_limit ? "STOCK" : "UBI",
                 vret ? (vret == -ENODEV ?
                         URSUS_RC_DEVICE_MISMATCH : URSUS_RC_BAD_CONTAINER) :
                        URSUS_RC_OK,
                 vret ? (vret == -ENODEV ?
                         "UrsusBoot FIP is for another device" :
                         "Expected a complete UrsusBoot .fip container; raw u-boot.bin or u-boot.lzma is not an update FIP") :
                        "UrsusBoot FIP validated for this board/layout");
    }

    ursus_httpd_reply_status = vret ? 400 : 200;
    return ursus_httpd_reply_status;
}

err_t httpd_post_begin(void *connection, const char *uri,
                       const char *http_request, u16_t http_request_len,
                       int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd)
{
    bool begin;
    enum ursus_upload_kind kind;
    int status;

    (void)http_request_len;

    if (ursus_httpd_post.connection && ursus_httpd_post.connection != connection) {
        ursus_httpd_reject(409, URSUS_RC_OPERATION_LOCKED,
                           "another upload POST is still active", 0, 0);
        strlcpy(response_uri, URSUS_HTTPD_REPLY_URI, response_uri_len);
        return ERR_USE;
    }

    kind = ursus_httpd_uri_kind(uri, &begin);
    if (kind == URSUS_UPLOAD_NONE) {
        ursus_httpd_reply_status = 501;
        snprintf(ursus_reply_body, sizeof(ursus_reply_body),
                 "{\"result\":\"NOT_IMPLEMENTED\","
                 "\"reason\":\"HTTPD A/B experiment exposes status and upload transport only\"}\n");
        strlcpy(response_uri, URSUS_HTTPD_REPLY_URI, response_uri_len);
        return ERR_VAL;
    }

    memset(&ursus_httpd_post, 0, sizeof(ursus_httpd_post));
    ursus_httpd_post.connection = connection;
    ursus_httpd_post.kind = kind;
    ursus_httpd_post.mode = begin ?
        URSUS_HTTPD_POST_BEGIN : URSUS_HTTPD_POST_CHUNK;
    ursus_httpd_post.content_len = content_len;

    if (begin) {
        if (content_len != 0) {
            status = ursus_httpd_reject(400, URSUS_RC_UPLOAD_INCOMPLETE,
                                        "begin request must have empty body",
                                        0, content_len);
        } else {
            status = ursus_httpd_begin_upload(kind, http_request);
        }
    } else {
        status = ursus_httpd_prepare_chunk(kind, http_request, content_len);
    }

    if (status != 200) {
        memset(&ursus_httpd_post, 0, sizeof(ursus_httpd_post));
        strlcpy(response_uri, URSUS_HTTPD_REPLY_URI, response_uri_len);
        return ERR_VAL;
    }

    *post_auto_wnd = 0;
    return ERR_OK;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p)
{
    struct ursus_upload_session *u;
    struct pbuf *q;
    size_t copied = 0;

    if (!p)
        return ERR_OK;

    if (connection != ursus_httpd_post.connection ||
        ursus_httpd_post.mode != URSUS_HTTPD_POST_CHUNK) {
        pbuf_free(p);
        return ERR_VAL;
    }

    u = ursus_upload_session(ursus_httpd_post.kind);
    if (!u || !u->active ||
        strcmp(ursus_httpd_post.generation, u->generation)) {
        ursus_httpd_post.failed = true;
    } else {
        for (q = p; q; q = q->next) {
            size_t remain = ursus_httpd_post.content_len -
                            ursus_httpd_post.received;
            size_t take = min_t(size_t, q->len, remain);

            if (take) {
                void *dst = map_sysmem(u->addr + ursus_httpd_post.offset +
                                      ursus_httpd_post.received, take);
                memcpy(dst, q->payload, take);
                unmap_sysmem(dst);
                ursus_httpd_post.received += take;
                copied += take;
            }
            if (take != q->len) {
                ursus_httpd_post.failed = true;
                break;
            }
        }
    }

    httpd_post_data_recved(connection, p->tot_len);
    pbuf_free(p);

    if (ursus_httpd_post.failed)
        return ERR_VAL;
    (void)copied;
    return ERR_OK;
}

void httpd_post_finished(void *connection, char *response_uri,
                         u16_t response_uri_len)
{
    if (connection != ursus_httpd_post.connection) {
        ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                           "POST connection state mismatch", 0, 0);
        strlcpy(response_uri, URSUS_HTTPD_REPLY_URI, response_uri_len);
        return;
    }

    if (ursus_httpd_post.mode == URSUS_HTTPD_POST_CHUNK) {
        if (ursus_httpd_post.failed)
            ursus_httpd_reject(409, URSUS_RC_UPLOAD_INCOMPLETE,
                               "chunk receive failed",
                               ursus_httpd_post.content_len,
                               ursus_httpd_post.received);
        else
            ursus_httpd_complete_upload();
    }

    strlcpy(response_uri, URSUS_HTTPD_REPLY_URI, response_uri_len);
    memset(&ursus_httpd_post, 0, sizeof(ursus_httpd_post));
}

static int ursus_httpd_experiment_run(void)
{
    struct netif *netif = NULL;
    int ret;
    bool own_eth = false, own_netif = false;

    if (ursus_web_running) {
        printf("URSUS_WEB_ALREADY_RUNNING owner=active action=noop\n");
        return CMD_RET_SUCCESS;
    }

    ursus_web_running = true;
    ursus_network_failed = false;
    ursus_last_rx_error = 0;
    ursus_rx_error_repeats = 0;
    printf("URSUS_HTTPD_EXPERIMENT=1 transport=lwip-httpd scope=status+protected-upload\n");

    env_set("ipaddr", "192.168.1.1");
    env_set("netmask", "255.255.255.0");
    env_set("serverip", "192.168.1.254");
    ursus_reset_image_state();
    ursus_reset_expert_state();
    memset(&ursus_fw_upload, 0, sizeof(ursus_fw_upload));
    memset(&ursus_init_upload, 0, sizeof(ursus_init_upload));
    memset(&ursus_preloader_upload, 0, sizeof(ursus_preloader_upload));
    memset(&ursus_fip_upload, 0, sizeof(ursus_fip_upload));
    memset(&ursus_vanilla_upload, 0, sizeof(ursus_vanilla_upload));
    memset(&ursus_httpd_post, 0, sizeof(ursus_httpd_post));
    ursus_preloader_valid = false;
    ursus_fip_upload_valid = false;
    ursus_vanilla_upload_valid = false;
    ursus_log_reset();

    if (ursus_header_parser_selftest())
        goto fail_plain;

    ret = net_lwip_eth_start();
    if (ret)
        goto fail_plain;
    own_eth = true;
    ursus_web_udev = eth_get_dev();
    if (!ursus_web_udev)
        goto fail_owned;
    netif = net_lwip_new_netif(ursus_web_udev);
    if (!netif)
        goto fail_owned;
    own_netif = true;
    ursus_web_netif = netif;

    ursus_lan_led_enable();
    ursus_probe_device(ursus_web_udev);
    ursus_build_status();

    httpd_init();
    printf("URSUS_HTTPD_LISTEN_OK port=80\n"
           "URSUS_WEBFAILSAFE_READY\n"
           "HTTPD EXPERIMENT: http://192.168.1.1/\n");

    ursus_uart_line_len = 0;
    ursus_uart_last_cr = false;
    printf("URSUS_UART_SHELL_READY=1\n");
    ursus_uart_shell_prompt();

    ursus_stop = false;
    while (!ursus_stop) {
        ursus_uart_shell_poll();
        if (ursus_stop)
            break;
        ret = net_lwip_rx(ursus_web_udev, netif);
        ursus_rx_report(ret);
        sys_check_timeouts();
        ursus_led_poll();
        udelay(500);
    }

    ursus_web_netif = NULL;
    if (own_netif && netif)
        net_lwip_remove_netif(netif);
    if (own_eth)
        net_lwip_eth_stop();
    ursus_web_udev = NULL;
    ursus_web_running = false;

    printf("URSUS_HTTPD_EXPERIMENT_STOPPED restart=unsupported-in-same-session\n");
    return ursus_network_failed ? CMD_RET_FAILURE : CMD_RET_SUCCESS;

fail_owned:
    ursus_web_netif = NULL;
    if (own_netif && netif)
        net_lwip_remove_netif(netif);
    if (own_eth)
        net_lwip_eth_stop();
    ursus_web_udev = NULL;
fail_plain:
    ursus_web_running = false;
    return CMD_RET_FAILURE;
}
