// SPDX-License-Identifier: GPL-2.0+
/* Streaming NAND backup and the read catalog:
 *
 *   GET /api/backup/catalog                    live MTD devices + UBI volumes
 *   GET /api/backup/mtd/<name>[?offset=&size=] a live MTD device or partition
 *   GET /api/backup/ubi/<volume>               one UBI volume, whole
 *
 * The response body is produced from flash as TCP drains it, so a whole-chip
 * dump needs neither a RAM staging buffer nor a second transport.  Only the
 * ECC-corrected main area of an MTD range is emitted; a bad eraseblock is sent
 * as 0xFF so the file keeps physical offsets.  OOB and OTP are not part of
 * this format.
 *
 * Read-only by construction: nothing here erases, writes, maps or unmaps.
 *
 * WHERE THE READ RUNS.  Not in an lwIP callback.  board_schedule_poll() (t74)
 * pumps lwIP from schedule(), which the NAND drivers call between pages, so
 * any callback that starts a flash read can be re-entered by its own read: a
 * nested tcp_sent would start a second refill on the same buffer and offset,
 * and a nested tcp_err/close would free the connection under the read.  The
 * body is therefore produced by ursus_dl_service(), called from the main
 * loop, and the callbacks only queue and acknowledge.  A teardown that lands
 * while dl_busy is set only marks the connection dead; the read finishes it.
 */

/* One flash read per service pass: small enough to stay short, large enough
 * that a 256 MiB dump is not a per-page loop. */
#define URSUS_DL_BUF           (16 * 1024)
/* Do not read flash unless at least one write chunk of it can be queued. */
#define URSUS_DL_MIN_SND       1024
#define URSUS_DL_CATALOG_MAX   8192
/* A peer that stops reading (cable pulled, tab frozen) must not hold the
 * flash and block every state-changing request for as long as TCP takes to
 * give up.  No new span published for this long aborts the stream. */
#define URSUS_DL_STALL_MS      60000UL

#define URSUS_DL_MTD           1
#define URSUS_DL_UBI           2

static struct ursus_conn *ursus_dl_conn;
static struct tcp_pcb *ursus_dl_pcb;
static char ursus_dl_catalog[URSUS_DL_CATALOG_MAX];

/* True only while flash is still being read for a stream.  Once the last span
 * is read the flash is free, so a peer slow to acknowledge the tail must not
 * keep state-changing requests locked out. */
static bool ursus_dl_active(void)
{
    return ursus_dl_conn && URSUS_DL_MORE(ursus_dl_conn);
}

static bool ursus_dl_name_ok(const char *s, size_t len)
{
    size_t i;

    if (!len || len >= URSUS_DL_NAME_MAX)
        return false;
    for (i = 0; i < len; i++) {
        char ch = s[i];

        if (!isalnum((int)ch) && ch != '_' && ch != '-' && ch != '.')
            return false;
    }
    return true;
}

/* Value of one decimal or 0x-prefixed query parameter, or `fallback`. */
static u64 ursus_dl_query_u64(const char *query, size_t qlen,
                              const char *key, u64 fallback)
{
    size_t klen = strlen(key);
    const char *p = query;

    if (!query || !qlen)
        return fallback;
    while (p < query + qlen) {
        const char *amp = memchr(p, '&', query + qlen - p);
        size_t plen = (amp ? amp : query + qlen) - p;

        if (plen > klen + 1 && !strncmp(p, key, klen) && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t vlen = plen - klen - 1;
            char buf[24];

            if (vlen >= sizeof(buf))
                return fallback;
            memcpy(buf, v, vlen);
            buf[vlen] = 0;
            return simple_strtoull(buf, NULL, 0);
        }
        if (!amp)
            break;
        p = amp + 1;
    }
    return fallback;
}

/* Release the MTD reference and the buffer.  Safe on any connection, and the
 * one place the active-stream pointer is cleared. */
static void ursus_dl_release(struct ursus_conn *c)
{
    if (!c)
        return;
    if (ursus_dl_conn == c)
        ursus_dl_conn = NULL;
    if (c->dl_mtd) {
        put_mtd_device(c->dl_mtd);
        c->dl_mtd = NULL;
    }
    free(c->dl_buf);
    c->dl_buf = NULL;
    c->dl_bufsz = 0;
    c->dl_kind = 0;
    c->dl_ubi = NULL;
}

/* Web server stopping (UART Ctrl-C): give the device back.  The connection
 * itself dies with the netif, as every other connection does. */
static void ursus_dl_shutdown(void)
{
    if (ursus_dl_conn && !ursus_dl_conn->dl_busy)
        ursus_dl_release(ursus_dl_conn);
    ursus_dl_conn = NULL;
    ursus_dl_pcb = NULL;
}

/* Point the connection body at the bytes just read. */
static void ursus_dl_publish(struct ursus_conn *c, size_t take)
{
    c->dl_off += take;
    c->body = c->dl_buf;
    c->body_len = take;
    c->body_off = 0;
    c->dl_progress = get_timer(0);
}

/* Refill from an MTD range.  Returns false only on a read error, which aborts
 * the transfer: a short or wrong backup must not look complete. */
static bool ursus_dl_refill_mtd(struct ursus_conn *c)
{
    struct mtd_info *mtd = c->dl_mtd;
    u64 block;
    size_t take;
    int bad;

    take = (size_t)min_t(u64, (u64)c->dl_bufsz, c->dl_end - c->dl_off);
    /* Never span an eraseblock: bad-block state is decided per block. */
    block = c->dl_off - (c->dl_off % mtd->erasesize);
    take = (size_t)min_t(u64, (u64)take, block + mtd->erasesize - c->dl_off);

    bad = mtd_block_isbad(mtd, block);
    if (bad < 0) {
        ursus_logf("BACKUP: bad-block query failed at 0x%llx (%d)\n",
                   (unsigned long long)block, bad);
        return false;
    }
    if (bad > 0) {
        /* Keep physical offsets: a bad eraseblock occupies its own span. */
        memset(c->dl_buf, 0xff, take);
    } else {
        size_t got = 0;
        int ret = mtd_read(mtd, c->dl_off, take, &got, c->dl_buf);

        if (ret == -EUCLEAN)       /* corrected bitflips: data is good */
            ret = 0;
        if (ret || got != take) {
            ursus_logf("BACKUP: read failed at 0x%llx (%d, %u/%u)\n",
                       (unsigned long long)c->dl_off, ret,
                       (unsigned int)got, (unsigned int)take);
            return false;
        }
    }
    ursus_dl_publish(c, take);
    return true;
}

/* Refill from a UBI volume.  The volume is opened and closed around each read
 * and never held between passes: `ubi detach` from the expert console is not
 * refused while a volume is open, so a descriptor kept across passes could
 * dangle.  Re-checking the device and the volume's size each time turns any
 * change under the stream into an abort instead of a wrong file. */
static bool ursus_dl_refill_ubi(struct ursus_conn *c)
{
    struct ubi_volume_desc *desc;
    struct ubi_volume_info vi;
    u64 in_leb;
    size_t take;
    int ret;

    if (!ubi_devices[0] || (void *)ubi_devices[0] != c->dl_ubi) {
        ursus_logf("BACKUP: UBI changed under the stream of %s\n", c->dl_name);
        return false;
    }
    desc = ubi_open_volume_nm(0, c->dl_name, UBI_READONLY);
    if (IS_ERR(desc)) {
        ursus_logf("BACKUP: UBI volume %s vanished (%ld)\n", c->dl_name, PTR_ERR(desc));
        return false;
    }
    ubi_get_volume_info(desc, &vi);
    if (vi.upd_marker || vi.usable_leb_size <= 0 ||
        (u64)vi.used_bytes != c->dl_end) {
        ubi_close_volume(desc);
        ursus_logf("BACKUP: UBI volume %s changed under the stream\n", c->dl_name);
        return false;
    }
    in_leb = c->dl_off % (u64)vi.usable_leb_size;
    take = (size_t)min_t(u64, (u64)c->dl_bufsz, c->dl_end - c->dl_off);
    take = (size_t)min_t(u64, (u64)take, (u64)vi.usable_leb_size - in_leb);
    ret = ubi_read(desc, (int)(c->dl_off / (u64)vi.usable_leb_size),
                   c->dl_buf, (int)in_leb, (int)take);
    ubi_close_volume(desc);
    if (ret < 0) {
        ursus_logf("BACKUP: UBI read failed in %s at 0x%llx (%d)\n", c->dl_name,
                   (unsigned long long)c->dl_off, ret);
        return false;
    }
    ursus_dl_publish(c, take);
    return true;
}

static bool ursus_dl_refill(struct ursus_conn *c)
{
    if (!URSUS_DL_MORE(c))
        return true;
    return c->dl_kind == URSUS_DL_UBI ? ursus_dl_refill_ubi(c)
                                      : ursus_dl_refill_mtd(c);
}

/* Main-loop service point: the ONLY caller of ursus_dl_refill().  See the
 * header comment for why it must not run from a callback. */
static void ursus_dl_service(void)
{
    struct ursus_conn *c = ursus_dl_conn;
    struct tcp_pcb *pcb = ursus_dl_pcb;
    bool ok;
    err_t err;

    if (!c || c->dl_busy || !URSUS_DL_MORE(c))
        return;
    if (get_timer(c->dl_progress) >= URSUS_DL_STALL_MS) {
        ursus_logf("BACKUP: no progress for %lu s, aborting the stream of %s\n",
                   URSUS_DL_STALL_MS / 1000, c->dl_name);
        ursus_conn_release(pcb, c, true);
        return;
    }
    if (c->hdr_off < c->hdr_len || c->body_off < c->body_len)
        return;                          /* still draining the last span */
    if (tcp_sndbuf(pcb) < URSUS_DL_MIN_SND)
        return;                          /* nothing could be queued yet */

    c->dl_busy = true;
    ok = ursus_dl_refill(c);
    c->dl_busy = false;

    if (c->dl_dead) {
        /* The peer went away during the read; the teardown was deferred to
         * here.  The pcb is already closed or gone: do not touch it. */
        ursus_dl_release(c);
        free(c);
        return;
    }
    if (!ok) {
        ursus_conn_release(pcb, c, true);
        return;
    }
    err = ursus_http_pump(pcb, c);
    if (err != ERR_OK)
        ursus_conn_release(pcb, c, true);
}

static err_t ursus_dl_reject(struct tcp_pcb *pcb, struct ursus_conn *c,
                             int status, const char *klass, const char *why)
{
    snprintf(ursus_reply_body, sizeof(ursus_reply_body),
             "{\"result\":\"REJECTED\",\"reason_class\":\"%s\",\"reason\":\"%s\"}\n",
             klass, why);
    return ursus_http_start_response(pcb, c, status, "application/json",
                                     ursus_reply_body);
}

/* Common tail of a stream start: build the header, queue it, and hand the body
 * to ursus_dl_service().  Takes ownership of the MTD reference and the buffer
 * already stored in `c`; releases them itself on failure. */
static err_t ursus_dl_begin(struct tcp_pcb *pcb, struct ursus_conn *c,
                            const char *label, u64 offset, u64 size)
{
    int n;

    n = snprintf(c->hdr, sizeof(c->hdr),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                 "Content-Disposition: attachment; filename=\"%s-0x%llx-0x%llx.bin\"\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n"
                 "Content-Length: %llu\r\n\r\n",
                 label, (unsigned long long)offset, (unsigned long long)size,
                 (unsigned long long)size);
    if (n <= 0 || n >= (int)sizeof(c->hdr)) {
        ursus_dl_release(c);
        return ERR_VAL;
    }
    c->hdr_len = n;
    c->body = NULL;
    c->body_len = 0;
    c->body_off = 0;
    c->dl_off = offset;
    c->dl_end = offset + size;
    c->total_len = c->hdr_len + size;
    c->response_started = true;
    c->dl_progress = get_timer(0);
    ursus_dl_conn = c;
    ursus_dl_pcb = pcb;
    tcp_sent(pcb, ursus_http_sent);
    tcp_poll(pcb, ursus_http_poll, 1);
    return ursus_http_pump(pcb, c);
}

static bool ursus_dl_flash_busy(void)
{
    return ursus_ubi_migration_active() || ursus_ubi_update_active() ||
           ursus_fip_update_active();
}

static err_t ursus_dl_start_mtd(struct tcp_pcb *pcb, struct ursus_conn *c,
                                const char *path, size_t path_len)
{
    const char *query = memchr(path, '?', path_len);
    size_t name_len = (query ? query : path + path_len) - path;
    size_t qlen = query ? path_len - name_len - 1 : 0;
    struct mtd_info *mtd;
    u64 offset, size;

    if (query)
        query++;
    if (!ursus_dl_name_ok(path, name_len))
        return ursus_dl_reject(pcb, c, 400, "BAD_REQUEST", "invalid MTD name");
    memcpy(c->dl_name, path, name_len);
    c->dl_name[name_len] = 0;

    mtd = get_mtd_device_nm(c->dl_name);
    if (IS_ERR_OR_NULL(mtd))
        return ursus_dl_reject(pcb, c, 404, "NOT_FOUND", "no such MTD device");

    offset = ursus_dl_query_u64(query, qlen, "offset", 0);
    size = ursus_dl_query_u64(query, qlen, "size",
                              mtd->size - min_t(u64, offset, mtd->size));

    /* `size > mtd->size - offset`, not `offset + size > mtd->size`: the sum
     * of two u64 values from the query string can wrap and pass. */
    if (!size || !mtd->erasesize || offset % mtd->erasesize ||
        size % mtd->erasesize || offset >= mtd->size ||
        size > mtd->size - offset) {
        put_mtd_device(mtd);
        return ursus_dl_reject(pcb, c, 400, "BAD_REQUEST",
                               "range must be eraseblock aligned and inside the device");
    }
    c->dl_buf = malloc(URSUS_DL_BUF);
    if (!c->dl_buf) {
        put_mtd_device(mtd);
        return ursus_dl_reject(pcb, c, 409, "OPERATION_LOCKED",
                               "no memory for the backup buffer");
    }
    c->dl_bufsz = URSUS_DL_BUF;
    c->dl_mtd = mtd;
    c->dl_kind = URSUS_DL_MTD;
    ursus_logf("BACKUP: mtd %s 0x%llx + 0x%llx (main area, no OOB)\n", c->dl_name,
               (unsigned long long)offset, (unsigned long long)size);
    return ursus_dl_begin(pcb, c, c->dl_name, offset, size);
}

static err_t ursus_dl_start_ubi(struct tcp_pcb *pcb, struct ursus_conn *c,
                                const char *path, size_t path_len)
{
    struct ubi_volume_desc *desc;
    struct ubi_volume_info vi;

    if (memchr(path, '?', path_len))
        return ursus_dl_reject(pcb, c, 400, "BAD_REQUEST",
                               "a UBI volume is read whole; no range parameters");
    if (!ursus_dl_name_ok(path, path_len))
        return ursus_dl_reject(pcb, c, 400, "BAD_REQUEST", "invalid volume name");
    memcpy(c->dl_name, path, path_len);
    c->dl_name[path_len] = 0;

    if (!ubi_devices[0])
        return ursus_dl_reject(pcb, c, 409, "OPERATION_LOCKED",
                               "UBI is not attached; there are no volumes to read");
    desc = ubi_open_volume_nm(0, c->dl_name, UBI_READONLY);
    if (IS_ERR(desc))
        return ursus_dl_reject(pcb, c, 404, "NOT_FOUND", "no such UBI volume");
    ubi_get_volume_info(desc, &vi);
    ubi_close_volume(desc);
    if (vi.upd_marker)
        return ursus_dl_reject(pcb, c, 409, "OPERATION_LOCKED",
                               "the volume has an interrupted update marker");
    if (vi.used_bytes < 0 || vi.usable_leb_size <= 0)
        return ursus_dl_reject(pcb, c, 409, "OPERATION_LOCKED",
                               "the volume geometry is not usable");

    c->dl_buf = malloc(URSUS_DL_BUF);
    if (!c->dl_buf)
        return ursus_dl_reject(pcb, c, 409, "OPERATION_LOCKED",
                               "no memory for the backup buffer");
    c->dl_bufsz = URSUS_DL_BUF;
    c->dl_ubi = ubi_devices[0];
    c->dl_kind = URSUS_DL_UBI;
    ursus_logf("BACKUP: ubi volume %s, %llu bytes\n", c->dl_name,
               (unsigned long long)vi.used_bytes);
    return ursus_dl_begin(pcb, c, c->dl_name, 0, (u64)vi.used_bytes);
}

/* Append to the catalog buffer; the first overflow poisons it. */
static void ursus_dl_cat_add(size_t *len, bool *overflow, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*overflow)
        return;
    va_start(ap, fmt);
    n = vsnprintf(ursus_dl_catalog + *len, sizeof(ursus_dl_catalog) - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(ursus_dl_catalog) - *len) {
        *overflow = true;
        return;
    }
    *len += n;
}

/* What can be read right now, taken from the live MTD and UBI lists rather
 * than from a hard-coded map.  Reading a wrong range is harmless, so unlike a
 * write path this needs no proven per-model partition table.  On a Nokia stock
 * layout the partitions listed are the DTS ones (bl2/ubi); the named stock
 * partitions do not exist as MTD devices here, but any range of the whole
 * device can still be read with offset/size. */
static err_t ursus_dl_catalog_send(struct tcp_pcb *pcb, struct ursus_conn *c)
{
    struct mtd_info *mtd;
    struct ubi_device *ubi = ubi_devices[0];
    size_t len = 0;
    bool overflow = false, first = true;
    int i;

    ursus_dl_cat_add(&len, &overflow, "{\"schema\":1,\"mtd\":[");
    mtd_for_each_device(mtd) {
        struct mtd_info *root = mtd;
        u64 abs = 0;

        if (!mtd->name || !ursus_dl_name_ok(mtd->name, strlen(mtd->name)))
            continue;
        if (mtd->type != MTD_NANDFLASH && mtd->type != MTD_MLCNANDFLASH)
            continue;
        /* `offset` is relative to the parent; the client needs it absolute in
         * the root device, because an archive records physical offsets. */
        while (root->parent) {
            abs += root->offset;
            root = root->parent;
        }
        if (!root->name || !ursus_dl_name_ok(root->name, strlen(root->name)))
            continue;
        ursus_dl_cat_add(&len, &overflow,
            "%s{\"name\":\"%s\",\"root\":\"%s\",\"offset\":%llu,\"size\":%llu,"
            "\"erase\":%u,\"page\":%u,\"whole\":%s}",
            first ? "" : ",", mtd->name, root->name, (unsigned long long)abs,
            (unsigned long long)mtd->size, (unsigned int)mtd->erasesize,
            (unsigned int)mtd->writesize, mtd_is_partition(mtd) ? "false" : "true");
        first = false;
    }
    ursus_dl_cat_add(&len, &overflow, "],\"ubi\":{\"attached\":%s",
                     ubi ? "true" : "false");
    if (ubi) {
        ursus_dl_cat_add(&len, &overflow, ",\"leb\":%d,\"volumes\":[", ubi->leb_size);
        first = true;
        for (i = 0; i < ubi->vtbl_slots; i++) {
            struct ubi_volume *vol = ubi->volumes[i];

            if (!vol || !ursus_dl_name_ok(vol->name, strlen(vol->name)))
                continue;
            ursus_dl_cat_add(&len, &overflow,
                "%s{\"name\":\"%s\",\"id\":%d,\"type\":\"%s\",\"size\":%llu}",
                first ? "" : ",", vol->name, vol->vol_id,
                vol->vol_type == UBI_DYNAMIC_VOLUME ? "dynamic" : "static",
                (unsigned long long)vol->used_bytes);
            first = false;
        }
        ursus_dl_cat_add(&len, &overflow, "]");
    }
    ursus_dl_cat_add(&len, &overflow, "}}\n");

    if (overflow)
        return ursus_dl_reject(pcb, c, 500, "INTERNAL", "catalog exceeds its buffer");
    return ursus_http_start_response(pcb, c, 200, "application/json", ursus_dl_catalog);
}

/* Dispatcher for every GET /api/backup/... request. */
static err_t ursus_dl_route(struct tcp_pcb *pcb, struct ursus_conn *c)
{
    static const char mtd_prefix[] = "GET /api/backup/mtd/";
    static const char ubi_prefix[] = "GET /api/backup/ubi/";
    const char *path, *end;

    if (URSUS_REQ_MATCH(c->reqhdr, "GET /api/backup/catalog "))
        return ursus_dl_catalog_send(pcb, c);

    if (URSUS_REQ_MATCH(c->reqhdr, "GET /api/backup/mtd/"))
        path = c->reqhdr + sizeof(mtd_prefix) - 1;
    else if (URSUS_REQ_MATCH(c->reqhdr, "GET /api/backup/ubi/"))
        path = c->reqhdr + sizeof(ubi_prefix) - 1;
    else
        return ursus_dl_reject(pcb, c, 404, "NOT_FOUND", "unknown backup resource");

    end = strchr(path, ' ');
    if (!end)
        return ursus_dl_reject(pcb, c, 400, "BAD_REQUEST", "malformed request line");
    if (ursus_dl_active())
        return ursus_dl_reject(pcb, c, 409, "OPERATION_LOCKED",
                               "another backup stream is already running");
    if (ursus_dl_flash_busy())
        return ursus_dl_reject(pcb, c, 409, "OPERATION_LOCKED",
                               "another operation is active");
    if (URSUS_REQ_MATCH(c->reqhdr, "GET /api/backup/mtd/"))
        return ursus_dl_start_mtd(pcb, c, path, end - path);
    return ursus_dl_start_ubi(pcb, c, path, end - path);
}
