// SPDX-License-Identifier: GPL-2.0+
/* Streaming NAND backup: GET /api/backup/mtd/<name>[?offset=&size=].
 *
 * The response body is produced from flash as TCP drains it, so a whole-chip
 * dump needs neither a RAM staging buffer nor a second transport.  Only the
 * ECC-corrected main area is emitted; a bad eraseblock is sent as 0xFF so the
 * file keeps physical offsets.  OOB and OTP are not part of this format.
 *
 * Read-only by construction: nothing here erases or writes.
 */

/* One NAND read per pump refill.  Small enough that the lwIP callback it runs
 * in stays short, large enough that a 256 MiB dump is not a per-page loop. */
#define URSUS_DL_BUF           (16 * 1024)

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

static void ursus_dl_release(struct ursus_conn *c)
{
    if (!c)
        return;
    if (c->dl_mtd) {
        put_mtd_device(c->dl_mtd);
        c->dl_mtd = NULL;
    }
    free(c->dl_buf);
    c->dl_buf = NULL;
    c->dl_bufsz = 0;
}

/* Refill the response body from flash.  Returns false only on a read error,
 * which aborts the transfer: a short or wrong backup must not look complete. */
static bool ursus_dl_refill(struct ursus_conn *c)
{
    struct mtd_info *mtd = c->dl_mtd;
    u64 block;
    size_t take;
    int bad;

    if (!mtd || c->dl_off >= c->dl_end)
        return true;

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
    c->dl_off += take;
    c->body = c->dl_buf;
    c->body_len = take;
    c->body_off = 0;
    return true;
}

static err_t ursus_dl_start(struct tcp_pcb *pcb, struct ursus_conn *c,
                            const char *path, size_t path_len)
{
    char name[URSUS_DL_NAME_MAX];
    const char *query = memchr(path, '?', path_len);
    size_t name_len = (query ? query : path + path_len) - path;
    size_t qlen = query ? path_len - name_len - 1 : 0;
    struct mtd_info *mtd;
    u64 offset, size;
    int n;

    if (query)
        query++;
    if (!ursus_dl_name_ok(path, name_len))
        return ursus_http_start_response(pcb, c, 400, "application/json",
            "{\"result\":\"REJECTED\",\"reason_class\":\"BAD_REQUEST\",\"reason\":\"invalid MTD name\"}\n");
    memcpy(name, path, name_len);
    name[name_len] = 0;

    if (ursus_ubi_migration_active() || ursus_ubi_update_active() ||
        ursus_fip_update_active())
        return ursus_http_start_response(pcb, c, 409, "application/json",
            "{\"result\":\"REJECTED\",\"reason_class\":\"OPERATION_LOCKED\",\"reason\":\"another operation is active\"}\n");

    mtd = get_mtd_device_nm(name);
    if (IS_ERR_OR_NULL(mtd))
        return ursus_http_start_response(pcb, c, 404, "application/json",
            "{\"result\":\"REJECTED\",\"reason_class\":\"NOT_FOUND\",\"reason\":\"no such MTD device\"}\n");

    offset = ursus_dl_query_u64(query, qlen, "offset", 0);
    size = ursus_dl_query_u64(query, qlen, "size", mtd->size - min_t(u64, offset, mtd->size));

    /* Eraseblock alignment keeps the bad-block substitution unambiguous. */
    /* `size > mtd->size - offset`, not `offset + size > mtd->size`: the sum
     * of two u64 values from the query string can wrap and pass. */
    if (!size || !mtd->erasesize || offset % mtd->erasesize ||
        size % mtd->erasesize || offset >= mtd->size ||
        size > mtd->size - offset) {
        put_mtd_device(mtd);
        return ursus_http_start_response(pcb, c, 400, "application/json",
            "{\"result\":\"REJECTED\",\"reason_class\":\"BAD_REQUEST\",\"reason\":\"range must be eraseblock aligned and inside the device\"}\n");
    }

    c->dl_buf = malloc(URSUS_DL_BUF);
    if (!c->dl_buf) {
        put_mtd_device(mtd);
        return ursus_http_start_response(pcb, c, 409, "application/json",
            "{\"result\":\"REJECTED\",\"reason_class\":\"OPERATION_LOCKED\",\"reason\":\"no memory for the backup buffer\"}\n");
    }
    c->dl_bufsz = URSUS_DL_BUF;
    c->dl_mtd = mtd;
    c->dl_off = offset;
    c->dl_end = offset + size;

    n = snprintf(c->hdr, sizeof(c->hdr),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                 "Content-Disposition: attachment; filename=\"%s-0x%llx-0x%llx.bin\"\r\n"
                 "Cache-Control: no-store\r\nConnection: close\r\n"
                 "Content-Length: %llu\r\n\r\n",
                 name, (unsigned long long)offset, (unsigned long long)size,
                 (unsigned long long)size);
    if (n <= 0 || n >= (int)sizeof(c->hdr)) {
        ursus_dl_release(c);
        return ERR_VAL;
    }
    c->hdr_len = n;
    c->body = NULL;
    c->body_len = 0;
    c->body_off = 0;
    c->total_len = c->hdr_len + size;
    c->response_started = true;
    ursus_logf("BACKUP: %s 0x%llx + 0x%llx (main area, no OOB)\n",
               name, (unsigned long long)offset, (unsigned long long)size);
    if (!ursus_dl_refill(c)) {
        ursus_dl_release(c);
        return ERR_VAL;
    }
    tcp_sent(pcb, ursus_http_sent);
    tcp_poll(pcb, ursus_http_poll, 1);
    return ursus_http_pump(pcb, c);
}
