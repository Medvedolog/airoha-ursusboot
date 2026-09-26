// SPDX-License-Identifier: GPL-2.0+
/* UrsusBoot canonical OpenWrt all-in-UBI support for Nokia XG-040G-MD. */

#include <command.h>
#include <asm/cache.h>
#include <env.h>
#include <mapmem.h>
#include <malloc.h>
#include <mtd.h>
#include <linux/byteorder/little_endian.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/libfdt.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <u-boot/sha256.h>
#include <ubi_uboot.h>
#include <ursus_ubi.h>
#include <ursus_version.h>

#define URSUS_NAND_SIZE          0x10000000ULL
#define URSUS_UBI_BASE           0x00020000ULL
#define URSUS_UBI_SIZE           0x0ffe0000ULL
#define URSUS_UBI_BL2_SIZE       0x00020000ULL
#define URSUS_STOCK_FIP_OFF      0x00000800ULL
#define URSUS_STOCK_ENV_OFF      0x0007c000ULL
#define URSUS_STOCK_BOSA_OFF     0x051c0000ULL
#define URSUS_STOCK_RI_OFF       0x05200000ULL
#define URSUS_BOARD_DATA_SIZE    0x00040000UL
#define URSUS_UBI_FIP_VOL_SIZE   0x00100000UL
#define URSUS_UBI_STAGED_HEADROOM_LEBS 20
#define URSUS_UBI_ROOTFS_FREE_RESERVE_LEBS 16
#define URSUS_FIP_TOC_READ       0x00001000UL
#define URSUS_FIP_MAX            (URSUS_STOCK_ENV_OFF - URSUS_STOCK_FIP_OFF)
#define URSUS_UBI_BOOT_ADDR      0x90000000UL
#define URSUS_UBI_READBACK_ADDR  0x94000000UL
#define URSUS_MIG_BOSA_ADDR      0x98000000UL
#define URSUS_MIG_RI_ADDR        0x98040000UL
#define URSUS_MIG_FIP_ADDR       0x98100000UL
#define URSUS_BL2_PRELOADER_OFF  0x00000800UL
#define URSUS_MIG_STAGE_HOLD_MS  350UL
#define URSUS_READ_CHUNK         0x00100000UL
#define URSUS_FIP_MAGIC          0xaa640001U
#define URSUS_FIP_SERIAL         0x12345678U
#define URSUS_MAX_TOC_ENTRIES    32

/* HW-proven OpenWrt/Airoha BL2 preloader used by XMODEM and UBI layout. */
static const u8 ursus_preloader_sha256[SHA256_SUM_LEN] = {
    0x6c,0x3b,0x23,0x39,0xd0,0x36,0x34,0x03,
    0x96,0x73,0x0a,0x13,0xad,0xfe,0x35,0xc0,
    0xd2,0xa4,0xdd,0xde,0xde,0xff,0xb6,0xf9,
    0x96,0x5a,0x24,0xe0,0xc7,0x90,0x88,0x08,
};

/* HW-proven BL2 presentation for this exact preloader lineage:
 * 0x800 bytes 0xff prefix, raw preloader at 0x800, 0xff padding to 128 KiB. */
static const u8 ursus_bl2_image_sha256[SHA256_SUM_LEN] = {
    0x6f, 0x9c, 0x92, 0x8b, 0xad, 0x50, 0x0d, 0xe0,
    0x33, 0x9b, 0xbf, 0xdf, 0xa3, 0x54, 0xc1, 0x7a,
    0x7a, 0xc0, 0x44, 0xf9, 0x6c, 0x91, 0x3f, 0x3a,
    0x01, 0x30, 0x19, 0x71, 0xd6, 0xcd, 0x65, 0x9d,
};

enum ursus_migration_stage {
    URSUS_MIG_IDLE = 0,
    URSUS_MIG_PRECHECK,
    URSUS_MIG_BACKUP_BOSA,
    URSUS_MIG_BACKUP_RI,
    URSUS_MIG_BACKUP_FIP,
    URSUS_MIG_FORMAT_UBI,
    URSUS_MIG_ATTACH_UBI,
    URSUS_MIG_CREATE_VOLUMES,
    URSUS_MIG_WRITE_BOSA,
    URSUS_MIG_WRITE_RI,
    URSUS_MIG_WRITE_FIP,
    URSUS_MIG_WRITE_FIT,
    URSUS_MIG_VERIFY_BOSA,
    URSUS_MIG_VERIFY_RI,
    URSUS_MIG_VERIFY_FIP,
    URSUS_MIG_VERIFY_FIT,
    URSUS_MIG_COMMIT_BL2,
    URSUS_MIG_VERIFY_BL2,
    URSUS_MIG_COMPLETE,
    URSUS_MIG_FAILED,
};

struct ursus_migration_ctx {
    enum ursus_migration_stage stage;
    enum ursus_migration_stage failed_stage;
    enum ursus_migration_stage last_success_stage;
    ulong fit_addr;
    size_t fit_len;
    ulong preloader_addr;
    size_t preloader_len;
    size_t fip_len;
    int error;
    bool active;
    bool announced;
    bool commit_started;
    bool write_started;
    char error_code[48];
    ulong announced_at;
};

static struct ursus_migration_ctx ursus_mig;

enum ursus_update_stage {
    URSUS_UPD_IDLE = 0,
    URSUS_UPD_PRECHECK,
    URSUS_UPD_STAGE_PREP,
    URSUS_UPD_STAGE_WRITE,
    URSUS_UPD_STAGE_VERIFY,
    URSUS_UPD_PROMOTE,
    URSUS_UPD_FINAL_VERIFY,
    URSUS_UPD_COMPLETE,
    URSUS_UPD_FAILED,
};

struct ursus_update_ctx {
    enum ursus_update_stage stage;
    enum ursus_update_stage failed_stage;
    enum ursus_update_stage last_success_stage;
    ulong fit_addr;
    size_t fit_len;
    int error;
    bool active;
    bool announced;
    bool fit_present;
    bool fit_old_present;
    bool direct_mode;
    bool keep_settings;
    bool write_started;
    char error_code[48];
    unsigned int fit_pebs;
    ulong announced_at;
};

static struct ursus_update_ctx ursus_upd;
static char ursus_last_boot_reason[32] = "NOT_RUN";
static void *ursus_bl2_candidate_buf;
static bool ursus_bl2_candidate_ok;

static const u8 ursus_nt_fw_uuid[16] = {
    0xd6, 0xd0, 0xee, 0xa7, 0xfc, 0xea, 0xd5, 0x4b,
    0x97, 0x82, 0x99, 0x34, 0xf2, 0x34, 0xb6, 0xe4
};

struct ursus_fip_header {
    __le32 name;
    __le32 serial_number;
    __le64 flags;
} __packed;

struct ursus_fip_entry {
    u8 uuid[16];
    __le64 offset_address;
    __le64 size;
    __le64 flags;
} __packed;

static bool ursus_ubi_mtd_ready;
static struct mtd_info *ursus_ubi_master;
static struct mtd_info *ursus_ubi_bl2;
static struct mtd_info *ursus_ubi_full;

static const struct mtd_partition ursus_ubi_parts[] = {
    { .name = "ursus-ubi-bl2", .offset = 0, .size = URSUS_UBI_BL2_SIZE },
    { .name = "ursus-ubi-full", .offset = URSUS_UBI_BASE, .size = URSUS_UBI_SIZE },
};

static bool ursus_uuid_zero(const u8 *u)
{
    unsigned int i;

    for (i = 0; i < 16; i++)
        if (u[i])
            return false;
    return true;
}

static struct mtd_info *ursus_find_master_nand(void)
{
    struct mtd_info *mtd;

    mtd_probe_devices();
    mtd_for_each_device(mtd) {
        if (!mtd_is_partition(mtd) &&
            (mtd->type == MTD_NANDFLASH || mtd->type == MTD_MLCNANDFLASH))
            return mtd;
    }
    return NULL;
}

static int ursus_read_exact(struct mtd_info *mtd, loff_t off, size_t len, void *buf)
{
    size_t retlen = 0;
    int ret = mtd_read(mtd, off, len, &retlen, buf);

    if (ret == -EUCLEAN)
        ret = 0;
    return (!ret && retlen == len) ? 0 : -EIO;
}

static int ursus_read_range(struct mtd_info *mtd, u64 off, size_t len, u8 *dst)
{
    size_t page = mtd->writesize ? mtd->writesize : 2048;
    u8 *scratch;
    int ret = 0;

    scratch = malloc(page);
    if (!scratch)
        return -ENOMEM;
    while (len) {
        u64 base = off - (off % page);
        size_t in_page = off - base;
        size_t take;

        if (!in_page && len >= page) {
            size_t direct = len - (len % page);
            if (direct > URSUS_READ_CHUNK)
                direct = URSUS_READ_CHUNK - (URSUS_READ_CHUNK % page);
            ret = ursus_read_exact(mtd, off, direct, dst);
            if (ret)
                break;
            off += direct;
            dst += direct;
            len -= direct;
            continue;
        }
        ret = ursus_read_exact(mtd, base, page, scratch);
        if (ret)
            break;
        take = min_t(size_t, len, page - in_page);
        memcpy(dst, scratch + in_page, take);
        off += take;
        dst += take;
        len -= take;
    }
    free(scratch);
    return ret;
}

static int ursus_bad_in_range(struct mtd_info *mtd, u64 start, u64 len)
{
    u64 off, end = start + len;

    for (off = start - (start % mtd->erasesize); off < end; off += mtd->erasesize) {
        int ret = mtd_block_isbad(mtd, off);
        if (ret < 0)
            return ret;
        if (ret > 0)
            return 1;
    }
    return 0;
}

static int ursus_ubi_register_mtd(void)
{
    int ret;

    if (ursus_ubi_mtd_ready)
        return 0;
    ursus_ubi_master = ursus_find_master_nand();
    if (!ursus_ubi_master)
        return -ENODEV;
    if (ursus_ubi_master->size != URSUS_NAND_SIZE ||
        ursus_ubi_master->erasesize != 0x20000 ||
        ursus_ubi_master->writesize != 0x800) {
        printf("URSUS_UBI_GEOMETRY_REJECT size=0x%llx erase=0x%x write=0x%x\n",
               (unsigned long long)ursus_ubi_master->size,
               ursus_ubi_master->erasesize, ursus_ubi_master->writesize);
        return -EINVAL;
    }
    ret = add_mtd_partitions(ursus_ubi_master, ursus_ubi_parts,
                             ARRAY_SIZE(ursus_ubi_parts));
    if (ret) {
        printf("URSUS_UBI_MTD_PARTITIONS_FAIL ret=%d\n", ret);
        return ret;
    }
    ursus_ubi_bl2 = get_mtd_device_nm("ursus-ubi-bl2");
    if (IS_ERR(ursus_ubi_bl2)) {
        ret = PTR_ERR(ursus_ubi_bl2);
        ursus_ubi_bl2 = NULL;
        return ret;
    }
    ursus_ubi_full = get_mtd_device_nm("ursus-ubi-full");
    if (IS_ERR(ursus_ubi_full)) {
        ret = PTR_ERR(ursus_ubi_full);
        ursus_ubi_full = NULL;
        return ret;
    }
    ursus_ubi_mtd_ready = true;
    printf("URSUS_UBI_MTD_READY bl2=0x%llx ubi=0x%llx\n",
           (unsigned long long)ursus_ubi_bl2->size,
           (unsigned long long)ursus_ubi_full->size);
    return 0;
}

static int ursus_mem_equal(ulong a_addr, ulong b_addr, size_t len)
{
    const void *a = map_sysmem(a_addr, len);
    const void *b = map_sysmem(b_addr, len);
    int ret;

    if (!a || !b) {
        if (a) unmap_sysmem(a);
        if (b) unmap_sysmem(b);
        return -ENOMEM;
    }
    ret = memcmp(a, b, len) ? -EBADMSG : 0;
    unmap_sysmem(a);
    unmap_sysmem(b);
    return ret;
}

int ursus_ubi_validate_preloader(ulong addr, size_t len)
{
    const u8 *buf;
    u8 digest[SHA256_SUM_LEN];
    int ret = -EINVAL;

    if (!len || len > URSUS_UBI_PRELOADER_MAX)
        return -EFBIG;
    buf = map_sysmem(addr, len);
    if (!buf)
        return -ENOMEM;
    if (len < sizeof(struct ursus_fip_header) ||
        le32_to_cpu(((const struct ursus_fip_header *)buf)->name) != URSUS_FIP_MAGIC)
        goto out;
    sha256_csum_wd(buf, len, digest, CHUNKSZ_SHA256);
    if (memcmp(digest, ursus_preloader_sha256, sizeof(digest))) {
        printf("URSUS_UBI_PRELOADER_REJECT reason=sha256 len=%u\n", (unsigned int)len);
        ret = -EBADMSG;
        goto out;
    }
    printf("URSUS_UBI_PRELOADER_VALID len=%u sha256=6c3b2339d036340396730a13adfe35c0d2a4dddedeffb6f9965a24e0c7908808\n",
           (unsigned int)len);
    ret = 0;
out:
    unmap_sysmem(buf);
    return ret;
}

static int ursus_current_fip_length(struct mtd_info *nand, size_t *out_len)
{
    u8 *toc;
    struct ursus_fip_header *hdr;
    struct ursus_fip_entry *ent;
    unsigned int i;
    int ret = -EINVAL;

    toc = malloc(URSUS_FIP_TOC_READ);
    if (!toc)
        return -ENOMEM;
    if (ursus_read_range(nand, URSUS_STOCK_FIP_OFF, URSUS_FIP_TOC_READ, toc))
        goto out;
    hdr = (struct ursus_fip_header *)toc;
    if (le32_to_cpu(hdr->name) != URSUS_FIP_MAGIC ||
        le32_to_cpu(hdr->serial_number) != URSUS_FIP_SERIAL)
        goto out;
    ent = (struct ursus_fip_entry *)(toc + sizeof(*hdr));
    for (i = 0; i < URSUS_MAX_TOC_ENTRIES; i++, ent++) {
        u64 end;
        if ((u8 *)(ent + 1) > toc + URSUS_FIP_TOC_READ)
            break;
        if (!ursus_uuid_zero(ent->uuid))
            continue;
        end = le64_to_cpu(ent->offset_address);
        if (end < 0x10000 || end > URSUS_FIP_MAX)
            break;
        *out_len = (size_t)end;
        ret = 0;
        break;
    }
out:
    free(toc);
    return ret;
}

static bool ursus_fip_buffer_valid(const void *buf, size_t len)
{
    const struct ursus_fip_header *hdr = buf;
    const struct ursus_fip_entry *ent;
    unsigned int i;
    bool nt = false, term = false;

    if (!buf || len < sizeof(*hdr) + sizeof(struct ursus_fip_entry) ||
        le32_to_cpu(hdr->name) != URSUS_FIP_MAGIC ||
        le32_to_cpu(hdr->serial_number) != URSUS_FIP_SERIAL)
        return false;
    ent = (const void *)((const u8 *)buf + sizeof(*hdr));
    for (i = 0; i < URSUS_MAX_TOC_ENTRIES; i++, ent++) {
        if ((const u8 *)(ent + 1) > (const u8 *)buf + len)
            break;
        if (ursus_uuid_zero(ent->uuid)) {
            term = true;
            break;
        }
        if (!memcmp(ent->uuid, ursus_nt_fw_uuid, sizeof(ursus_nt_fw_uuid)))
            nt = true;
    }
    return nt && term;
}

static bool ursus_fit_header_at(ulong addr)
{
    const void *fit = map_sysmem(addr, 64);
    bool ok = fit && !fdt_check_header(fit) && fdt_totalsize(fit) >= sizeof(struct fdt_header);

    if (fit)
        unmap_sysmem(fit);
    return ok;
}

static bool ursus_ubi_volume_fip_valid(const char *name)
{
    const void *buf;
    bool ok = false;

    if (run_commandf("ubi read 0x%08lx %s 0x%lx", URSUS_UBI_READBACK_ADDR,
                     name, URSUS_FIP_TOC_READ))
        return false;
    buf = map_sysmem(URSUS_UBI_READBACK_ADDR, URSUS_FIP_TOC_READ);
    if (buf) {
        ok = ursus_fip_buffer_valid(buf, URSUS_FIP_TOC_READ);
        unmap_sysmem(buf);
    }
    return ok;
}

static bool ursus_ubi_volume_fit_valid(const char *name)
{
    if (run_commandf("ubi read 0x%08lx %s 0x40", URSUS_UBI_READBACK_ADDR, name))
        return false;
    return ursus_fit_header_at(URSUS_UBI_READBACK_ADDR);
}

static void ursus_ubi_fill_volume_diag(const char *name,
                                        struct ursus_ubi_volume_diag *out,
                                        bool validate_fip)
{
    struct ubi_volume_desc *desc;

    memset(out, 0, sizeof(*out));
    out->id = -1;
    desc = ubi_open_volume_nm(0, name, UBI_READONLY);
    if (IS_ERR(desc))
        return;
    out->present = true;
    out->id = desc->vol->vol_id;
    out->type = desc->vol->vol_type;
    out->used_bytes = desc->vol->used_bytes;
    out->reserved_pebs = desc->vol->reserved_pebs;
    ubi_close_volume(desc);
    out->valid = validate_fip ? ursus_ubi_volume_fip_valid(name) :
                               ursus_ubi_volume_fit_valid(name);
}

int ursus_ubi_probe_diag(struct ursus_ubi_diag *diag)
{
    struct ubi_device *ubi;
    int i, ret;

    if (!diag)
        return -EINVAL;
    memset(diag, 0, sizeof(*diag));
    diag->fip.id = diag->fit.id = diag->fit_old.id = -1;

    ret = ursus_ubi_register_mtd();
    if (ret)
        return ret;
    run_command("ubi detach", 0);
    ret = run_command("ubi part ursus-ubi-full", 0);
    if (ret)
        return ret;
    ubi = ubi_devices[0];
    if (!ubi)
        return -ENODEV;

    diag->attached = true;
    diag->peb_count = ubi->peb_count;
    diag->good_pebs = ubi->good_peb_count;
    diag->bad_pebs = ubi->bad_peb_count;
    diag->corrupt_pebs = ubi->corr_peb_count;
    diag->free_pebs = ubi->avail_pebs;
    diag->leb_size = ubi->leb_size;
    for (i = 0; i < ubi->vtbl_slots && diag->vol_count < URSUS_UBI_DIAG_MAX_VOLS; i++) {
        struct ubi_volume *vol = ubi->volumes[i];
        struct ursus_ubi_vol_brief *b = &diag->vols[diag->vol_count];

        if (!vol)
            continue;
        snprintf(b->name, sizeof(b->name), "%s", vol->name);
        b->id = vol->vol_id;
        b->type = vol->vol_type;
        b->reserved_pebs = vol->reserved_pebs;
        b->used_bytes = vol->used_bytes;
        diag->vol_count++;
    }
    ursus_ubi_fill_volume_diag("fip", &diag->fip, true);
    ursus_ubi_fill_volume_diag("fit", &diag->fit, false);
    ursus_ubi_fill_volume_diag("fit.old", &diag->fit_old, false);
    return 0;
}

int ursus_ubi_probe(bool *fip_ok, bool *fit_ok)
{
    struct ursus_ubi_diag diag;
    int ret;

    if (fip_ok) *fip_ok = false;
    if (fit_ok) *fit_ok = false;
    ret = ursus_ubi_probe_diag(&diag);
    if (ret)
        return ret;
    if (fip_ok)
        *fip_ok = diag.fip.present && diag.fip.valid;
    if (fit_ok)
        *fit_ok = (diag.fit.present && diag.fit.valid) ||
                  (diag.fit_old.present && diag.fit_old.valid);
    printf("URSUS_UBI_PROBE fip=%s fit=%s fit_volume=%s fit_id=%d fit_old=%s fit_old_id=%d\n",
           fip_ok && *fip_ok ? "OK" : "ERROR",
           fit_ok && *fit_ok ? "OK" : "ERROR",
           diag.fit.present ? "fit" : diag.fit_old.present ? "fit.old" : "missing",
           diag.fit.id, diag.fit_old.present ? "present" : "missing", diag.fit_old.id);
    return 0;
}

const char *ursus_ubi_last_boot_reason(void)
{
    return ursus_last_boot_reason;
}

static void ursus_set_boot_reason(const char *reason)
{
    snprintf(ursus_last_boot_reason, sizeof(ursus_last_boot_reason), "%s",
             reason ? reason : "UNKNOWN");
}

int ursus_ubi_boot(void)
{
    const char *fitvol = "fit";
    ulong len;
    const void *fit;
    int ret;

    ursus_set_boot_reason("BOOT_IN_PROGRESS");
    printf("URSUS_UBI_BOOT_BEGIN\n");
    ret = ursus_ubi_register_mtd();
    if (ret) {
        ursus_set_boot_reason("MTD_REGISTER_FAILED");
        goto fail;
    }
    run_command("ubi detach", 0);
    ret = run_command("ubi part ursus-ubi-full", 0);
    if (ret) {
        ursus_set_boot_reason("UBI_ATTACH_FAILED");
        goto fail;
    }
    if (run_command("ubi check fip", 0) || !ursus_ubi_volume_fip_valid("fip")) {
        printf("URSUS_UBI_BOOT_FIP_INVALID\n");
        ursus_set_boot_reason("FIP_INVALID");
        ret = -EBADMSG;
        goto fail;
    }
    if (run_command("ubi check fit", 0)) {
        if (run_command("ubi check fit.old", 0)) {
            printf("URSUS_UBI_BOOT_FIT_MISSING\n");
            ursus_set_boot_reason("FIT_MISSING");
            ret = -ENOENT;
            goto fail;
        }
        fitvol = "fit.old";
        printf("URSUS_UBI_BOOT_FALLBACK volume=fit.old\n");
    }
    ret = run_commandf("ubi read 0x%08lx %s", URSUS_UBI_BOOT_ADDR, fitvol);
    if (ret) {
        ursus_set_boot_reason("FIT_READ_FAILED");
        goto fail;
    }
    len = env_get_hex("filesize", 0);
    if (!len || len > 0x04000000UL) {
        ursus_set_boot_reason("FIT_SIZE_INVALID");
        ret = -EFBIG;
        goto fail;
    }
    fit = map_sysmem(URSUS_UBI_BOOT_ADDR, min_t(ulong, len, 64));
    if (!fit) {
        ursus_set_boot_reason("FIT_MAP_FAILED");
        return -ENOMEM;
    }
    ret = fdt_check_header(fit);
    unmap_sysmem(fit);
    if (ret) {
        printf("URSUS_UBI_BOOT_BAD_FIT_HEADER ret=%d\n", ret);
        ursus_set_boot_reason("FIT_HEADER_INVALID");
        goto fail;
    }
    printf("URSUS_UBI_BOOT_FIT_LOADED volume=%s addr=0x%08lx bytes=%lu\n",
           fitvol, URSUS_UBI_BOOT_ADDR, len);
    ret = run_commandf("bootm 0x%08lx", URSUS_UBI_BOOT_ADDR);
    printf("URSUS_UBI_BOOT_RETURNED ret=%d\n", ret);
    ursus_set_boot_reason("BOOTM_RETURNED");
fail:
    printf("URSUS_UBI_BOOT_FAILED ret=%d reason=%s\n", ret, ursus_last_boot_reason);
    return ret ? ret : -EIO;
}

static int ursus_ubi_verify_volume_manifest(bool require_fit)
{
    static const char * const core_names[] = {
        "ubootenv", "ubootenv2", "bosa", "ri", "fip", "rootfs_data"
    };
    struct ubi_device *ubi = ubi_devices[0];
    struct ubi_volume_desc *desc;
    unsigned int i;
    unsigned long long bytes;

    if (!ubi)
        return -ENODEV;
    for (i = 0; i < ARRAY_SIZE(core_names); i++) {
        desc = ubi_open_volume_nm(0, core_names[i], UBI_READONLY);
        if (IS_ERR(desc)) {
            printf("URSUS_UBI_VOLUME_MANIFEST_FAIL missing=%s\n", core_names[i]);
            return PTR_ERR(desc);
        }
        ubi_close_volume(desc);
    }
    if (require_fit) {
        desc = ubi_open_volume_nm(0, "fit", UBI_READONLY);
        if (IS_ERR(desc)) {
            printf("URSUS_UBI_VOLUME_MANIFEST_FAIL missing=fit\n");
            return PTR_ERR(desc);
        }
        ubi_close_volume(desc);
    }

    desc = ubi_open_volume_nm(0, "rootfs_data", UBI_READONLY);
    if (IS_ERR(desc))
        return PTR_ERR(desc);
    bytes = (unsigned long long)desc->vol->reserved_pebs * desc->vol->usable_leb_size;
    if (desc->vol->vol_type != UBI_DYNAMIC_VOLUME || !desc->vol->reserved_pebs || !bytes) {
        printf("URSUS_UBI_ROOTFS_DATA_FAIL lebs=%d bytes=%llu type=%d\n",
               desc->vol->reserved_pebs, bytes, desc->vol->vol_type);
        ubi_close_volume(desc);
        return -EINVAL;
    }
    printf("URSUS_UBI_ROOTFS_DATA_OK lebs=%d bytes=%llu free_lebs=%d\n",
           desc->vol->reserved_pebs, bytes, ubi->avail_pebs);
    ubi_close_volume(desc);

    printf("URSUS_UBI_VOLUME_MANIFEST_OK core=6 fit=%s free_lebs=%d\n",
           require_fit ? "required" : "optional", ubi->avail_pebs);
    return 0;
}

static int ursus_ubi_store_rootfs_data_max(unsigned long bytes)
{
    ulong old = env_get_ulong("rootfs_data_max", 10, 0);
    int ret;

    if (old == bytes) {
        printf("URSUS_ROOTFS_DATA_MAX_ENV_OK bytes=%lu unchanged=1\n", bytes);
        return 0;
    }
    if (env_set_ulong("rootfs_data_max", bytes))
        return -EIO;
    ret = env_save();
    if (ret) {
        printf("URSUS_ROOTFS_DATA_MAX_ENV_WARN bytes=%lu saveenv_ret=%d\n", bytes, ret);
        return -EIO;
    }
    printf("URSUS_ROOTFS_DATA_MAX_ENV_OK bytes=%lu persisted=1\n", bytes);
    return 0;
}

static int ursus_ubi_current_rootfs_data(unsigned long *bytes_out, unsigned int *lebs_out)
{
    struct ubi_volume_desc *desc;
    unsigned long long bytes;

    desc = ubi_open_volume_nm(0, "rootfs_data", UBI_READONLY);
    if (IS_ERR(desc))
        return PTR_ERR(desc);
    bytes = (unsigned long long)desc->vol->reserved_pebs * desc->vol->usable_leb_size;
    if (bytes > ULONG_MAX) {
        ubi_close_volume(desc);
        return -EOVERFLOW;
    }
    if (bytes_out)
        *bytes_out = (unsigned long)bytes;
    if (lebs_out)
        *lebs_out = desc->vol->reserved_pebs;
    ubi_close_volume(desc);
    return 0;
}

static int ursus_ubi_create_clean_rootfs_data(bool persist_env, unsigned long *bytes_out)
{
    struct ubi_device *ubi = ubi_devices[0];
    unsigned int target_lebs;
    unsigned long target_bytes;
    int ret;

    if (!ubi || !ubi->leb_size)
        return -ENODEV;
    if (ubi->avail_pebs <= URSUS_UBI_ROOTFS_FREE_RESERVE_LEBS)
        return -ENOSPC;
    target_lebs = ubi->avail_pebs - URSUS_UBI_ROOTFS_FREE_RESERVE_LEBS;
    target_bytes = (unsigned long)target_lebs * ubi->leb_size;
    ret = run_commandf("ubi create rootfs_data 0x%lx dynamic 6", target_bytes);
    if (ret)
        return ret;
    printf("URSUS_ROOTFS_DATA_CREATED lebs=%u bytes=%lu reserve_free_lebs=%u\n",
           target_lebs, target_bytes, URSUS_UBI_ROOTFS_FREE_RESERVE_LEBS);
    if (bytes_out)
        *bytes_out = target_bytes;
    if (persist_env) {
        ret = ursus_ubi_store_rootfs_data_max(target_bytes);
        if (ret)
            printf("URSUS_ROOTFS_DATA_ENV_PERSIST_WARNING ret=%d\n", ret);
    }
    return 0;
}

int ursus_ubi_reset_settings(void)
{
    unsigned long target_bytes = 0;
    int ret;

    ret = ursus_ubi_register_mtd();
    if (ret)
        return ret;
    run_command("ubi detach", 0);
    ret = run_command("ubi part ursus-ubi-full", 0);
    if (ret)
        return ret;
    if (!run_command("ubi check rootfs_data", 0)) {
        ret = run_command("ubi remove rootfs_data", 0);
        if (ret)
            return ret;
    }
    ret = ursus_ubi_create_clean_rootfs_data(true, &target_bytes);
    if (ret)
        return ret;
    ret = run_command("ubi check rootfs_data", 0);
    if (ret)
        return ret;
    printf("URSUS_OPENWRT_SETTINGS_RESET_OK layout=OPENWRT_UBI rootfs_data_bytes=%lu\n", target_bytes);
    return 0;
}

static const char *ursus_upd_stage_name(enum ursus_update_stage stage)
{
    switch (stage) {
    case URSUS_UPD_PRECHECK: return "PRECHECK";
    case URSUS_UPD_STAGE_PREP: return "STAGE_PREP";
    case URSUS_UPD_STAGE_WRITE: return "STAGE_WRITE";
    case URSUS_UPD_STAGE_VERIFY: return "STAGE_VERIFY";
    case URSUS_UPD_PROMOTE: return "PROMOTE";
    case URSUS_UPD_FINAL_VERIFY: return "FINAL_VERIFY";
    case URSUS_UPD_COMPLETE: return "COMPLETE";
    case URSUS_UPD_FAILED: return "FAILED";
    default: return "IDLE";
    }
}

static const char *ursus_upd_detail(enum ursus_update_stage stage)
{
    switch (stage) {
    case URSUS_UPD_PRECHECK: return "Validate UBI, UrsusBoot FIP and OpenWrt image";
    case URSUS_UPD_STAGE_PREP: return "Prepare safe update target volume";
    case URSUS_UPD_STAGE_WRITE: return "Write OpenWrt candidate";
    case URSUS_UPD_STAGE_VERIFY: return "Read back and verify OpenWrt candidate";
    case URSUS_UPD_PROMOTE: return "Activate staged candidate and preserve previous fit where space allows";
    case URSUS_UPD_FINAL_VERIFY: return "Verify active fit and apply OpenWrt settings policy";
    case URSUS_UPD_COMPLETE: return "OpenWrt image verified; manual reboot available";
    case URSUS_UPD_FAILED: return "Update stopped; UrsusBoot Recovery remains available; staged mode preserves fit fallback";
    default: return "Idle";
    }
}

static unsigned int ursus_upd_percent(enum ursus_update_stage stage)
{
    switch (stage) {
    case URSUS_UPD_PRECHECK: return 5;
    case URSUS_UPD_STAGE_PREP: return 15;
    case URSUS_UPD_STAGE_WRITE: return 35;
    case URSUS_UPD_STAGE_VERIFY: return 60;
    case URSUS_UPD_PROMOTE: return 75;
    case URSUS_UPD_FINAL_VERIFY: return 90;
    case URSUS_UPD_COMPLETE: return 100;
    case URSUS_UPD_FAILED: return 100;
    default: return 0;
    }
}

static int ursus_upd_fail_reason(int ret, const char *code)
{
    enum ursus_update_stage failed = ursus_upd.stage;

    ursus_upd.error = ret ? ret : -EIO;
    ursus_upd.failed_stage = failed;
    if (!ursus_upd.error_code[0])
        snprintf(ursus_upd.error_code, sizeof(ursus_upd.error_code), "%s", code ? code : "UPDATE_FAILED");
    ursus_upd.stage = URSUS_UPD_FAILED;
    ursus_upd.active = false;
    ursus_upd.announced = false;
    printf("URSUS_UBI_UPDATE_FAILED failed_stage=%s ret=%d code=%s transaction=%s mode=%s recovery=available staged_fallback=%u\n",
           ursus_upd_stage_name(failed), ursus_upd.error, ursus_upd.error_code,
           ursus_upd.write_started ? "WRITE_STATE_UNKNOWN" : "NOT_STARTED",
           ursus_upd.direct_mode ? "DIRECT_RECOVERY_SAFE" : "STAGED",
           ursus_upd.direct_mode ? 0 : 1);
    return ursus_upd.error;
}

static int ursus_upd_fail(int ret)
{
    char code[48];
    snprintf(code, sizeof(code), "%s_ERR_%d", ursus_upd_stage_name(ursus_upd.stage), ret ? -ret : EIO);
    return ursus_upd_fail_reason(ret, code);
}

int ursus_ubi_update_start(ulong fit_addr, size_t fit_len, bool keep_settings)
{
    if (ursus_upd.active || ursus_mig.active)
        return -EBUSY;
    if (!fit_len || fit_len > 0x04000000UL)
        return -EFBIG;
    if (fdt_check_header((const void *)(uintptr_t)fit_addr))
        return -ENOEXEC;
    memset(&ursus_upd, 0, sizeof(ursus_upd));
    ursus_upd.fit_addr = fit_addr;
    ursus_upd.fit_len = fit_len;
    ursus_upd.keep_settings = keep_settings;
    ursus_upd.stage = URSUS_UPD_PRECHECK;
    ursus_upd.active = true;
    printf("URSUS_UBI_UPDATE_ARMED bytes=%u keep_settings=%u transaction=auto-staged-or-direct-readback\n",
           (unsigned int)fit_len, keep_settings ? 1 : 0);
    return 0;
}

bool ursus_ubi_update_active(void) { return ursus_upd.active; }
bool ursus_ubi_update_complete(void) { return ursus_upd.stage == URSUS_UPD_COMPLETE; }
bool ursus_ubi_update_failed(void) { return ursus_upd.stage == URSUS_UPD_FAILED; }
const char *ursus_ubi_update_stage(void) { return ursus_upd_stage_name(ursus_upd.stage); }
const char *ursus_ubi_update_detail(void) { return ursus_upd_detail(ursus_upd.stage); }
unsigned int ursus_ubi_update_percent(void) { return ursus_upd_percent(ursus_upd.stage); }
int ursus_ubi_update_error(void) { return ursus_upd.error; }
const char *ursus_ubi_update_failed_stage(void) { return ursus_upd.failed_stage ? ursus_upd_stage_name(ursus_upd.failed_stage) : "NONE"; }
const char *ursus_ubi_update_last_success_stage(void) { return ursus_upd.last_success_stage ? ursus_upd_stage_name(ursus_upd.last_success_stage) : "IDLE"; }
const char *ursus_ubi_update_error_code(void) { return ursus_upd.error_code[0] ? ursus_upd.error_code : "NONE"; }
const char *ursus_ubi_update_transaction_state(void)
{
    if (ursus_upd.stage == URSUS_UPD_COMPLETE) return "COMPLETION_PROVEN";
    if (ursus_upd.stage == URSUS_UPD_FAILED) return ursus_upd.write_started ? "WRITE_STATE_UNKNOWN" : "NOT_STARTED";
    if (ursus_upd.active && ursus_upd.write_started) return "IN_PROGRESS";
    return "NOT_STARTED";
}

int ursus_ubi_update_step(void)
{
    struct ubi_device *ubi;
    struct ubi_volume_desc *desc;
    unsigned int reclaim = 0, required_pebs;
    int ret = 0;

    if (!ursus_upd.active)
        return ursus_upd.stage == URSUS_UPD_COMPLETE ? 1 :
               ursus_upd.stage == URSUS_UPD_FAILED ? ursus_upd.error : 0;

    if (!ursus_upd.announced) {
        ursus_upd.announced = true;
        ursus_upd.announced_at = get_timer(0);
        printf("URSUS_UBI_UPDATE_STAGE name=%s percent=%u detail=%s\n",
               ursus_upd_stage_name(ursus_upd.stage),
               ursus_upd_percent(ursus_upd.stage), ursus_upd_detail(ursus_upd.stage));
        return 0;
    }
    if (get_timer(ursus_upd.announced_at) < URSUS_MIG_STAGE_HOLD_MS)
        return 0;
    ursus_upd.announced = false;

    switch (ursus_upd.stage) {
    case URSUS_UPD_PRECHECK:
        ret = ursus_ubi_register_mtd();
        if (ret) return ursus_upd_fail_reason(ret, "PRECHECK_MTD_REGISTER_FAILED");
        run_command("ubi detach", 0);
        ret = run_command("ubi part ursus-ubi-full", 0);
        if (ret) return ursus_upd_fail_reason(ret, "PRECHECK_UBI_ATTACH_FAILED");
        if (run_command("ubi check fip", 0) || !ursus_ubi_volume_fip_valid("fip")) {
            printf("URSUS_UBI_UPDATE_REJECT reason=fip-invalid\n");
            return ursus_upd_fail_reason(-EBADMSG, "PRECHECK_FIP_INVALID");
        }
        ret = ursus_ubi_verify_volume_manifest(false);
        if (ret) return ursus_upd_fail_reason(ret, "PRECHECK_VOLUME_MANIFEST_INVALID");
        ubi = ubi_devices[0];
        if (!ubi || !ubi->leb_size)
            return ursus_upd_fail_reason(-ENODEV, "PRECHECK_UBI_DEVICE_UNAVAILABLE");
        required_pebs = DIV_ROUND_UP(ursus_upd.fit_len, ubi->leb_size);
        desc = ubi_open_volume_nm(0, "fit.old", UBI_READONLY);
        if (!IS_ERR(desc)) {
            ursus_upd.fit_old_present = true;
            reclaim += desc->vol->reserved_pebs;
            ubi_close_volume(desc);
        }
        desc = ubi_open_volume_nm(0, "fit.new", UBI_READONLY);
        if (!IS_ERR(desc)) {
            reclaim += desc->vol->reserved_pebs;
            ubi_close_volume(desc);
        }
        desc = ubi_open_volume_nm(0, "fit", UBI_READONLY);
        if (!IS_ERR(desc)) {
            ursus_upd.fit_present = true;
            ursus_upd.fit_pebs = desc->vol->reserved_pebs;
            ubi_close_volume(desc);
        }
        if ((unsigned int)ubi->avail_pebs + reclaim >=
            required_pebs + URSUS_UBI_STAGED_HEADROOM_LEBS) {
            ursus_upd.direct_mode = false;
        } else {
            unsigned int current_headroom = (unsigned int)ubi->avail_pebs + reclaim;
            unsigned int preserve_headroom = min_t(unsigned int, current_headroom,
                                                    URSUS_UBI_ROOTFS_FREE_RESERVE_LEBS);
            unsigned int available_after_reclaim = current_headroom + ursus_upd.fit_pebs;
            unsigned int projected_free = available_after_reclaim >= required_pebs ?
                                          available_after_reclaim - required_pebs : 0;

            if (available_after_reclaim >= required_pebs + preserve_headroom) {
                ursus_upd.direct_mode = true;
            } else {
                printf("URSUS_UBI_UPDATE_REJECT reason=headroom free=%d reclaim=%u current_fit=%u need=%u preserve=%u projected=%u\n",
                       ubi->avail_pebs, reclaim, ursus_upd.fit_pebs, required_pebs,
                       preserve_headroom, projected_free);
                return ursus_upd_fail_reason(-ENOSPC, "PRECHECK_INSUFFICIENT_UBI_HEADROOM");
            }
        }
        printf("URSUS_UBI_UPDATE_PRECHECK_OK mode=%s fit_present=%u fit_old_present=%u free_lebs=%d reclaim_lebs=%u candidate_lebs=%u projected_free=%u\n",
               ursus_upd.direct_mode ? "DIRECT_RECOVERY_SAFE" : "STAGED",
               ursus_upd.fit_present, ursus_upd.fit_old_present,
               ubi->avail_pebs, reclaim, required_pebs,
               (unsigned int)ubi->avail_pebs + reclaim + ursus_upd.fit_pebs >= required_pebs ?
               (unsigned int)ubi->avail_pebs + reclaim + ursus_upd.fit_pebs - required_pebs : 0);
        ursus_upd.last_success_stage = URSUS_UPD_PRECHECK;
        ursus_upd.stage = URSUS_UPD_STAGE_PREP;
        break;
    case URSUS_UPD_STAGE_PREP:
        ursus_upd.write_started = true;
        if (!run_command("ubi check fit.new", 0) && run_command("ubi remove fit.new", 0))
            return ursus_upd_fail(-EIO);
        if (!run_command("ubi check fit.old", 0) && run_command("ubi remove fit.old", 0))
            return ursus_upd_fail(-EIO);
        if (ursus_upd.direct_mode && !run_command("ubi check fit", 0) && run_command("ubi remove fit", 0))
            return ursus_upd_fail(-EIO);
        ret = run_commandf(ursus_upd.direct_mode ?
                           "ubi create fit 0x%lx dynamic" : "ubi create fit.new 0x%lx dynamic",
                           (ulong)ursus_upd.fit_len);
        if (ret) return ursus_upd_fail(ret);
        printf("URSUS_UBI_UPDATE_STAGE_READY volume=%s bytes=%u\n",
               ursus_upd.direct_mode ? "fit" : "fit.new", (unsigned int)ursus_upd.fit_len);
        ursus_upd.last_success_stage = URSUS_UPD_STAGE_PREP;
        ursus_upd.stage = URSUS_UPD_STAGE_WRITE;
        break;
    case URSUS_UPD_STAGE_WRITE:
        ret = run_commandf(ursus_upd.direct_mode ?
                           "ubi write 0x%08lx fit 0x%lx" : "ubi write 0x%08lx fit.new 0x%lx",
                           ursus_upd.fit_addr, (ulong)ursus_upd.fit_len);
        if (ret) return ursus_upd_fail(ret);
        ursus_upd.last_success_stage = URSUS_UPD_STAGE_WRITE;
        ursus_upd.stage = URSUS_UPD_STAGE_VERIFY;
        break;
    case URSUS_UPD_STAGE_VERIFY:
        ret = run_commandf(ursus_upd.direct_mode ?
                           "ubi read 0x%08lx fit 0x%lx" : "ubi read 0x%08lx fit.new 0x%lx",
                           URSUS_UBI_READBACK_ADDR, (ulong)ursus_upd.fit_len);
        if (ret || ursus_mem_equal(ursus_upd.fit_addr, URSUS_UBI_READBACK_ADDR,
                                   ursus_upd.fit_len))
            return ursus_upd_fail(ret ? ret : -EBADMSG);
        if (fdt_check_header((const void *)(uintptr_t)URSUS_UBI_READBACK_ADDR))
            return ursus_upd_fail(-ENOEXEC);
        printf("URSUS_UBI_UPDATE_STAGE_VERIFIED volume=%s bytes=%u\n",
               ursus_upd.direct_mode ? "fit" : "fit.new", (unsigned int)ursus_upd.fit_len);
        ursus_upd.last_success_stage = URSUS_UPD_STAGE_VERIFY;
        ursus_upd.stage = ursus_upd.direct_mode ? URSUS_UPD_FINAL_VERIFY : URSUS_UPD_PROMOTE;
        break;
    case URSUS_UPD_PROMOTE:
        if (!run_command("ubi check fit", 0)) {
            ret = run_command("ubi rename fit fit.old", 0);
            if (ret) return ursus_upd_fail(ret);
            printf("URSUS_UBI_UPDATE_FALLBACK_READY volume=fit.old\n");
        }
        ret = run_command("ubi rename fit.new fit", 0);
        if (ret) return ursus_upd_fail(ret);
        printf("URSUS_UBI_UPDATE_PROMOTED volume=fit\n");
        ursus_upd.last_success_stage = URSUS_UPD_PROMOTE;
        ursus_upd.stage = URSUS_UPD_FINAL_VERIFY;
        break;
    case URSUS_UPD_FINAL_VERIFY:
        ret = run_commandf("ubi read 0x%08lx fit 0x%lx", URSUS_UBI_READBACK_ADDR,
                           (ulong)ursus_upd.fit_len);
        if (ret || ursus_mem_equal(ursus_upd.fit_addr, URSUS_UBI_READBACK_ADDR,
                                   ursus_upd.fit_len))
            return ursus_upd_fail(ret ? ret : -EBADMSG);
        if (fdt_check_header((const void *)(uintptr_t)URSUS_UBI_READBACK_ADDR))
            return ursus_upd_fail(-ENOEXEC);
        if (!ursus_upd.keep_settings) {
            ret = ursus_ubi_reset_settings();
            if (ret) return ursus_upd_fail(ret);
        } else {
            unsigned long rootfs_bytes = 0;
            ret = ursus_ubi_current_rootfs_data(&rootfs_bytes, NULL);
            if (ret) return ursus_upd_fail(ret);
            if (ursus_ubi_store_rootfs_data_max(rootfs_bytes))
                printf("URSUS_ROOTFS_DATA_ENV_PERSIST_WARNING keep_settings=1 bytes=%lu\n", rootfs_bytes);
        }
        ret = ursus_ubi_verify_volume_manifest(true);
        if (ret) return ursus_upd_fail(ret);
        ursus_upd.last_success_stage = URSUS_UPD_FINAL_VERIFY;
        ursus_upd.stage = URSUS_UPD_COMPLETE;
        ursus_upd.active = false;
        ursus_upd.error = 0;
        printf("URSUS_UBI_UPDATE_VERIFIED bytes=%u\nURSUS_UBI_UPDATE_COMPLETE reboot=MANUAL\n",
               (unsigned int)ursus_upd.fit_len);
        return 1;
    default:
        return ursus_upd_fail(-EINVAL);
    }
    return 0;
}

int ursus_ubi_update_fit(ulong fit_addr, size_t fit_len)
{
    int ret = ursus_ubi_update_start(fit_addr, fit_len, true);

    if (ret)
        return ret;
    while (ursus_ubi_update_active()) {
        ret = ursus_ubi_update_step();
        if (ret < 0)
            return ret;
        mdelay(10);
    }
    return ursus_ubi_update_complete() ? 0 : ursus_ubi_update_error();
}

static const char *ursus_mig_stage_name(enum ursus_migration_stage stage)
{
    switch (stage) {
    case URSUS_MIG_PRECHECK: return "PRECHECK";
    case URSUS_MIG_BACKUP_BOSA: return "BACKUP_BOSA";
    case URSUS_MIG_BACKUP_RI: return "BACKUP_RI";
    case URSUS_MIG_BACKUP_FIP: return "BACKUP_FIP";
    case URSUS_MIG_FORMAT_UBI: return "FORMAT_UBI";
    case URSUS_MIG_ATTACH_UBI: return "ATTACH_UBI";
    case URSUS_MIG_CREATE_VOLUMES: return "CREATE_VOLUMES";
    case URSUS_MIG_WRITE_BOSA: return "WRITE_BOSA";
    case URSUS_MIG_WRITE_RI: return "WRITE_RI";
    case URSUS_MIG_WRITE_FIP: return "WRITE_FIP";
    case URSUS_MIG_WRITE_FIT: return "WRITE_FIT";
    case URSUS_MIG_VERIFY_BOSA: return "VERIFY_BOSA";
    case URSUS_MIG_VERIFY_RI: return "VERIFY_RI";
    case URSUS_MIG_VERIFY_FIP: return "VERIFY_FIP";
    case URSUS_MIG_VERIFY_FIT: return "VERIFY_FIT";
    case URSUS_MIG_COMMIT_BL2: return "COMMIT_BL2";
    case URSUS_MIG_VERIFY_BL2: return "VERIFY_BL2";
    case URSUS_MIG_COMPLETE: return "COMPLETE";
    case URSUS_MIG_FAILED: return "FAILED";
    default: return "IDLE";
    }
}

static const char *ursus_mig_detail(enum ursus_migration_stage stage)
{
    switch (stage) {
    case URSUS_MIG_PRECHECK: return "Validate NAND and BL2 candidate";
    case URSUS_MIG_BACKUP_BOSA: return "Read bosa into RAM";
    case URSUS_MIG_BACKUP_RI: return "Read ri into RAM";
    case URSUS_MIG_BACKUP_FIP: return "Preserve current UrsusBoot FIP in RAM";
    case URSUS_MIG_FORMAT_UBI: return "Format NAND region for UBI";
    case URSUS_MIG_ATTACH_UBI: return "Attach UBI";
    case URSUS_MIG_CREATE_VOLUMES: return "Create seven required UBI volumes";
    case URSUS_MIG_WRITE_BOSA: return "Write bosa";
    case URSUS_MIG_WRITE_RI: return "Write ri";
    case URSUS_MIG_WRITE_FIP: return "Write UrsusBoot FIP";
    case URSUS_MIG_WRITE_FIT: return "Write OpenWrt FIT";
    case URSUS_MIG_VERIFY_BOSA: return "Read back bosa";
    case URSUS_MIG_VERIFY_RI: return "Read back ri";
    case URSUS_MIG_VERIFY_FIP: return "Read back UrsusBoot FIP";
    case URSUS_MIG_VERIFY_FIT: return "Read back OpenWrt FIT and verify volume manifest";
    case URSUS_MIG_COMMIT_BL2: return "Write complete 128 KiB BL2 last";
    case URSUS_MIG_VERIFY_BL2: return "Read back complete BL2";
    case URSUS_MIG_COMPLETE: return "OpenWrt UBI migration complete";
    case URSUS_MIG_FAILED: return "Migration stopped by error";
    default: return "Idle";
    }
}

static unsigned int ursus_mig_percent(enum ursus_migration_stage stage)
{
    switch (stage) {
    case URSUS_MIG_PRECHECK: return 3;
    case URSUS_MIG_BACKUP_BOSA: return 8;
    case URSUS_MIG_BACKUP_RI: return 12;
    case URSUS_MIG_BACKUP_FIP: return 18;
    case URSUS_MIG_FORMAT_UBI: return 25;
    case URSUS_MIG_ATTACH_UBI: return 35;
    case URSUS_MIG_CREATE_VOLUMES: return 42;
    case URSUS_MIG_WRITE_BOSA: return 48;
    case URSUS_MIG_WRITE_RI: return 53;
    case URSUS_MIG_WRITE_FIP: return 59;
    case URSUS_MIG_WRITE_FIT: return 67;
    case URSUS_MIG_VERIFY_BOSA: return 76;
    case URSUS_MIG_VERIFY_RI: return 80;
    case URSUS_MIG_VERIFY_FIP: return 84;
    case URSUS_MIG_VERIFY_FIT: return 88;
    case URSUS_MIG_COMMIT_BL2: return 93;
    case URSUS_MIG_VERIFY_BL2: return 97;
    case URSUS_MIG_COMPLETE: return 100;
    case URSUS_MIG_FAILED: return 0;
    default: return 0;
    }
}

static void ursus_print_sha256(const u8 *digest)
{
    unsigned int i;

    for (i = 0; i < SHA256_SUM_LEN; i++)
        printf("%02x", digest[i]);
}

int ursus_ubi_prepare_bl2_candidate(ulong preloader_addr, size_t preloader_len)
{
    u8 digest[SHA256_SUM_LEN];
    u8 *dst;
    const u8 *src;
    size_t i;

    ursus_bl2_candidate_ok = false;
    if (!preloader_len ||
        preloader_len > URSUS_UBI_BL2_SIZE - URSUS_BL2_PRELOADER_OFF)
        return -EFBIG;

    if (!ursus_bl2_candidate_buf) {
        ursus_bl2_candidate_buf = memalign(ARCH_DMA_MINALIGN, URSUS_UBI_BL2_SIZE);
        if (!ursus_bl2_candidate_buf)
            return -ENOMEM;
    }

    dst = ursus_bl2_candidate_buf;
    src = map_sysmem(preloader_addr, preloader_len);
    if (!src)
        return -ENOMEM;

    memset(dst, 0xff, URSUS_UBI_BL2_SIZE);
    memcpy(dst + URSUS_BL2_PRELOADER_OFF, src, preloader_len);

    /* Structural check is deliberately independent from the digest. */
    for (i = 0; i < URSUS_BL2_PRELOADER_OFF; i++) {
        if (dst[i] != 0xff) {
            unmap_sysmem(src);
            printf("URSUS_UBI_BL2_CANDIDATE_REJECT reason=prefix offset=0x%zx\n", i);
            return -EBADMSG;
        }
    }
    if (memcmp(dst + URSUS_BL2_PRELOADER_OFF, src, preloader_len)) {
        unmap_sysmem(src);
        printf("URSUS_UBI_BL2_CANDIDATE_REJECT reason=preloader-copy\n");
        return -EBADMSG;
    }
    unmap_sysmem(src);
    for (i = URSUS_BL2_PRELOADER_OFF + preloader_len;
         i < URSUS_UBI_BL2_SIZE; i++) {
        if (dst[i] != 0xff) {
            printf("URSUS_UBI_BL2_CANDIDATE_REJECT reason=padding offset=0x%zx\n", i);
            return -EBADMSG;
        }
    }

    sha256_csum_wd(dst, URSUS_UBI_BL2_SIZE, digest, CHUNKSZ_SHA256);
    if (memcmp(digest, ursus_bl2_image_sha256, sizeof(digest))) {
        printf("URSUS_UBI_BL2_CANDIDATE_REJECT reason=sha256 actual=");
        ursus_print_sha256(digest);
        printf(" expected=");
        ursus_print_sha256(ursus_bl2_image_sha256);
        printf(" addr=0x%08lx\n", (ulong)(uintptr_t)dst);
        return -EBADMSG;
    }

    ursus_bl2_candidate_ok = true;
    printf("URSUS_UBI_BL2_CANDIDATE_OK size=0x%x prefix=0x%x preloader_offset=0x%x addr=0x%08lx sha256=",
           (unsigned int)URSUS_UBI_BL2_SIZE, (unsigned int)URSUS_BL2_PRELOADER_OFF,
           (unsigned int)URSUS_BL2_PRELOADER_OFF, (ulong)(uintptr_t)dst);
    ursus_print_sha256(ursus_bl2_image_sha256);
    printf("\n");
    return 0;
}

bool ursus_ubi_bl2_candidate_valid(void)
{
    return ursus_bl2_candidate_ok;
}

ulong ursus_ubi_bl2_candidate_addr(void)
{
    return ursus_bl2_candidate_ok ? (ulong)(uintptr_t)ursus_bl2_candidate_buf : 0;
}

static int ursus_verify_bl2_candidate(void)
{
    u8 digest[SHA256_SUM_LEN];

    if (!ursus_bl2_candidate_ok || !ursus_bl2_candidate_buf)
        return -EINVAL;
    sha256_csum_wd(ursus_bl2_candidate_buf, URSUS_UBI_BL2_SIZE, digest, CHUNKSZ_SHA256);
    if (memcmp(digest, ursus_bl2_image_sha256, sizeof(digest))) {
        ursus_bl2_candidate_ok = false;
        printf("URSUS_UBI_BL2_CANDIDATE_REJECT reason=rehash actual=");
        ursus_print_sha256(digest);
        printf("\n");
        return -EBADMSG;
    }
    return 0;
}

/* The fast-scan BL2 currently installed at NAND 0..128 KiB must be exactly the
 * pinned BL2 image this runtime migrates with. The Vanilla FIP replacement uses
 * this so a Vanilla BL31/U-Boot is committed only on top of the proven BL2. */
int ursus_ubi_installed_bl2_matches_pin(void)
{
    struct mtd_info *nand = ursus_find_master_nand();
    u8 digest[SHA256_SUM_LEN];
    u8 *buf;
    int ret;

    if (!nand)
        return -ENODEV;
    buf = malloc(URSUS_UBI_BL2_SIZE);
    if (!buf)
        return -ENOMEM;
    ret = ursus_read_exact(nand, 0, URSUS_UBI_BL2_SIZE, buf);
    if (!ret) {
        sha256_csum_wd(buf, URSUS_UBI_BL2_SIZE, digest, CHUNKSZ_SHA256);
        ret = memcmp(digest, ursus_bl2_image_sha256, sizeof(digest)) ? -EBADMSG : 0;
    }
    free(buf);
    printf("URSUS_UBI_INSTALLED_BL2 %s actual=", ret ? "MISMATCH" : "OK");
    if (ret == 0 || ret == -EBADMSG)
        ursus_print_sha256(digest);
    printf(" pinned=");
    ursus_print_sha256(ursus_bl2_image_sha256);
    printf("\n");
    return ret;
}

static int ursus_mig_fail(int ret)
{
    enum ursus_migration_stage failed = ursus_mig.stage;
    ursus_mig.error = ret ? ret : -EIO;
    ursus_mig.failed_stage = failed;
    snprintf(ursus_mig.error_code, sizeof(ursus_mig.error_code), "%s_ERR_%d",
             ursus_mig_stage_name(failed), ursus_mig.error ? -ursus_mig.error : EIO);
    ursus_mig.stage = URSUS_MIG_FAILED;
    ursus_mig.active = false;
    ursus_mig.announced = false;
    printf("URSUS_UBI_MIGRATION_FAILED failed_stage=%s ret=%d code=%s transaction=%s commit_may_be_incomplete=%u\n",
           ursus_mig_stage_name(failed), ursus_mig.error, ursus_mig.error_code,
           ursus_mig.write_started ? "WRITE_STATE_UNKNOWN" : "NOT_STARTED",
           ursus_mig.commit_started ? 1 : 0);
    return ursus_mig.error;
}

int ursus_ubi_migration_start(ulong fit_addr, size_t fit_len,
                              ulong preloader_addr, size_t preloader_len)
{
    if (ursus_mig.active)
        return -EBUSY;
    if (!fit_len || fit_len > 0x04000000UL)
        return -EFBIG;
    memset(&ursus_mig, 0, sizeof(ursus_mig));
    ursus_mig.fit_addr = fit_addr;
    ursus_mig.fit_len = fit_len;
    ursus_mig.preloader_addr = preloader_addr;
    ursus_mig.preloader_len = preloader_len;
    ursus_mig.stage = URSUS_MIG_PRECHECK;
    ursus_mig.active = true;
    printf("URSUS_UBI_MIGRATION_ARMED fit=%u preloader=%u commit=BL2_LAST bl2_layout=FF800+PRELOADER@800+FFPAD\n",
           (unsigned int)fit_len, (unsigned int)preloader_len);
    return 0;
}

bool ursus_ubi_migration_active(void) { return ursus_mig.active; }
bool ursus_ubi_migration_complete(void) { return ursus_mig.stage == URSUS_MIG_COMPLETE; }
bool ursus_ubi_migration_failed(void) { return ursus_mig.stage == URSUS_MIG_FAILED; }
const char *ursus_ubi_migration_stage(void) { return ursus_mig_stage_name(ursus_mig.stage); }
const char *ursus_ubi_migration_detail(void) { return ursus_mig_detail(ursus_mig.stage); }
unsigned int ursus_ubi_migration_percent(void) { return ursus_mig_percent(ursus_mig.stage); }
int ursus_ubi_migration_error(void) { return ursus_mig.error; }
const char *ursus_ubi_migration_failed_stage(void) { return ursus_mig.failed_stage ? ursus_mig_stage_name(ursus_mig.failed_stage) : "NONE"; }
const char *ursus_ubi_migration_last_success_stage(void) { return ursus_mig.last_success_stage ? ursus_mig_stage_name(ursus_mig.last_success_stage) : "IDLE"; }
const char *ursus_ubi_migration_error_code(void) { return ursus_mig.error_code[0] ? ursus_mig.error_code : "NONE"; }
const char *ursus_ubi_migration_transaction_state(void)
{
    if (ursus_mig.stage == URSUS_MIG_COMPLETE) return "COMPLETION_PROVEN";
    if (ursus_mig.stage == URSUS_MIG_FAILED) return ursus_mig.write_started ? "WRITE_STATE_UNKNOWN" : "NOT_STARTED";
    if (ursus_mig.active && ursus_mig.write_started) return "IN_PROGRESS";
    return "NOT_STARTED";
}

int ursus_ubi_migration_step(void)
{
    struct mtd_info *nand;
    int ret = 0;

    if (!ursus_mig.active)
        return ursus_mig.stage == URSUS_MIG_COMPLETE ? 1 :
               ursus_mig.stage == URSUS_MIG_FAILED ? ursus_mig.error : 0;

    /* Hold every newly announced stage briefly while the WebFailsafe loop
     * remains alive, so /api/status and /api/log can expose it before a
     * potentially blocking NAND command starts. */
    if (!ursus_mig.announced) {
        ursus_mig.announced = true;
        ursus_mig.announced_at = get_timer(0);
        printf("URSUS_UBI_MIGRATION_STAGE name=%s percent=%u detail=%s\n",
               ursus_mig_stage_name(ursus_mig.stage),
               ursus_mig_percent(ursus_mig.stage), ursus_mig_detail(ursus_mig.stage));
        return 0;
    }
    if (get_timer(ursus_mig.announced_at) < URSUS_MIG_STAGE_HOLD_MS)
        return 0;
    ursus_mig.announced = false;
    nand = ursus_ubi_master;

    switch (ursus_mig.stage) {
    case URSUS_MIG_PRECHECK:
        ret = ursus_ubi_validate_preloader(ursus_mig.preloader_addr, ursus_mig.preloader_len);
        if (ret) return ursus_mig_fail(ret);
        ret = ursus_ubi_register_mtd();
        if (ret) return ursus_mig_fail(ret);
        nand = ursus_ubi_master;
        ret = ursus_bad_in_range(nand, 0, 0x80000);
        if (ret) return ursus_mig_fail(ret < 0 ? ret : -EIO);
        ret = ursus_bad_in_range(nand, URSUS_STOCK_BOSA_OFF, URSUS_BOARD_DATA_SIZE);
        if (ret) return ursus_mig_fail(ret < 0 ? ret : -EIO);
        ret = ursus_bad_in_range(nand, URSUS_STOCK_RI_OFF, URSUS_BOARD_DATA_SIZE);
        if (ret) return ursus_mig_fail(ret < 0 ? ret : -EIO);
        ret = ursus_verify_bl2_candidate();
        if (ret) return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=NAND_GEOMETRY_BADBLOCKS_BL2_CANDIDATE\n");
        ursus_mig.last_success_stage = URSUS_MIG_PRECHECK;
        ursus_mig.stage = URSUS_MIG_BACKUP_BOSA;
        break;
    case URSUS_MIG_BACKUP_BOSA:
        ret = ursus_read_range(nand, URSUS_STOCK_BOSA_OFF, URSUS_BOARD_DATA_SIZE,
                               (u8 *)(uintptr_t)URSUS_MIG_BOSA_ADDR);
        if (ret) return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=BOSA_BACKUP bytes=0x%lx\n", URSUS_BOARD_DATA_SIZE);
        ursus_mig.last_success_stage = URSUS_MIG_BACKUP_BOSA;
        ursus_mig.stage = URSUS_MIG_BACKUP_RI;
        break;
    case URSUS_MIG_BACKUP_RI:
        ret = ursus_read_range(nand, URSUS_STOCK_RI_OFF, URSUS_BOARD_DATA_SIZE,
                               (u8 *)(uintptr_t)URSUS_MIG_RI_ADDR);
        if (ret) return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=RI_BACKUP bytes=0x%lx\n", URSUS_BOARD_DATA_SIZE);
        ursus_mig.last_success_stage = URSUS_MIG_BACKUP_RI;
        ursus_mig.stage = URSUS_MIG_BACKUP_FIP;
        break;
    case URSUS_MIG_BACKUP_FIP:
        ret = ursus_current_fip_length(nand, &ursus_mig.fip_len);
        if (ret) return ursus_mig_fail(ret);
        if (ursus_mig.fip_len > URSUS_UBI_FIP_VOL_SIZE)
            return ursus_mig_fail(-EFBIG);
        ret = ursus_read_range(nand, URSUS_STOCK_FIP_OFF, ursus_mig.fip_len,
                               (u8 *)(uintptr_t)URSUS_MIG_FIP_ADDR);
        if (ret) return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=FIP_BACKUP bytes=%u\n",
               (unsigned int)ursus_mig.fip_len);
        ursus_mig.last_success_stage = URSUS_MIG_BACKUP_FIP;
        ursus_mig.stage = URSUS_MIG_FORMAT_UBI;
        break;
    case URSUS_MIG_FORMAT_UBI:
        ursus_mig.write_started = true;
        run_command("ubi detach", 0);
        printf("URSUS_UBI_MIGRATION_WRITE_CRITICAL=1\n");
        ret = run_command("mtd erase ursus-ubi-full 0 0xffe0000", 0);
        if (ret) return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=UBI_REGION_ERASED\n");
        ursus_mig.last_success_stage = URSUS_MIG_FORMAT_UBI;
        ursus_mig.stage = URSUS_MIG_ATTACH_UBI;
        break;
    case URSUS_MIG_ATTACH_UBI:
        ret = run_command("ubi part ursus-ubi-full", 0);
        if (ret) return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=UBI_ATTACHED\n");
        ursus_mig.last_success_stage = URSUS_MIG_ATTACH_UBI;
        ursus_mig.stage = URSUS_MIG_CREATE_VOLUMES;
        break;
    case URSUS_MIG_CREATE_VOLUMES:
        if ((ret = run_command("ubi create ubootenv 0x1f000 dynamic 0", 0)) ||
            (ret = run_command("ubi create ubootenv2 0x1f000 dynamic 1", 0)) ||
            (ret = run_command("ubi create bosa 0x40000 dynamic 2", 0)) ||
            (ret = run_command("ubi create ri 0x40000 dynamic 3", 0)) ||
            (ret = run_command("ubi create fip 0x100000 static 4", 0)) ||
            (ret = run_commandf("ubi create fit 0x%lx dynamic 5", (ulong)ursus_mig.fit_len)))
            return ursus_mig_fail(ret);
        ret = ursus_ubi_create_clean_rootfs_data(false, NULL);
        if (ret)
            return ursus_mig_fail(ret);
        if ((ret = run_command("ubi check ubootenv", 0)) ||
            (ret = run_command("ubi check ubootenv2", 0)) ||
            (ret = run_command("ubi check bosa", 0)) ||
            (ret = run_command("ubi check ri", 0)) ||
            (ret = run_command("ubi check fip", 0)) ||
            (ret = run_command("ubi check fit", 0)) ||
            (ret = run_command("ubi check rootfs_data", 0)))
            return ursus_mig_fail(ret);
        ret = ursus_ubi_verify_volume_manifest(true);
        if (ret)
            return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=VOLUMES_CREATED count=7 rootfs_data_policy=MAX_MINUS_16_PEBS\n");
        ursus_mig.last_success_stage = URSUS_MIG_CREATE_VOLUMES;
        ursus_mig.stage = URSUS_MIG_WRITE_BOSA;
        break;
    case URSUS_MIG_WRITE_BOSA:
        ret = run_commandf("ubi write 0x%08lx bosa 0x%lx", URSUS_MIG_BOSA_ADDR, URSUS_BOARD_DATA_SIZE);
        if (ret) return ursus_mig_fail(ret);
        ursus_mig.last_success_stage = URSUS_MIG_WRITE_BOSA;
        ursus_mig.stage = URSUS_MIG_WRITE_RI;
        break;
    case URSUS_MIG_WRITE_RI:
        ret = run_commandf("ubi write 0x%08lx ri 0x%lx", URSUS_MIG_RI_ADDR, URSUS_BOARD_DATA_SIZE);
        if (ret) return ursus_mig_fail(ret);
        ursus_mig.last_success_stage = URSUS_MIG_WRITE_RI;
        ursus_mig.stage = URSUS_MIG_WRITE_FIP;
        break;
    case URSUS_MIG_WRITE_FIP:
        ret = run_commandf("ubi write 0x%08lx fip 0x%lx", URSUS_MIG_FIP_ADDR, (ulong)ursus_mig.fip_len);
        if (ret) return ursus_mig_fail(ret);
        ursus_mig.last_success_stage = URSUS_MIG_WRITE_FIP;
        ursus_mig.stage = URSUS_MIG_WRITE_FIT;
        break;
    case URSUS_MIG_WRITE_FIT:
        ret = run_commandf("ubi write 0x%08lx fit 0x%lx", ursus_mig.fit_addr, (ulong)ursus_mig.fit_len);
        if (ret) return ursus_mig_fail(ret);
        ursus_mig.last_success_stage = URSUS_MIG_WRITE_FIT;
        ursus_mig.stage = URSUS_MIG_VERIFY_BOSA;
        break;
    case URSUS_MIG_VERIFY_BOSA:
        ret = run_commandf("ubi read 0x%08lx bosa 0x%lx", URSUS_UBI_READBACK_ADDR, URSUS_BOARD_DATA_SIZE);
        if (ret || ursus_mem_equal(URSUS_MIG_BOSA_ADDR, URSUS_UBI_READBACK_ADDR, URSUS_BOARD_DATA_SIZE))
            return ursus_mig_fail(ret ? ret : -EBADMSG);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=BOSA_READBACK\n");
        ursus_mig.last_success_stage = URSUS_MIG_VERIFY_BOSA;
        ursus_mig.stage = URSUS_MIG_VERIFY_RI;
        break;
    case URSUS_MIG_VERIFY_RI:
        ret = run_commandf("ubi read 0x%08lx ri 0x%lx", URSUS_UBI_READBACK_ADDR, URSUS_BOARD_DATA_SIZE);
        if (ret || ursus_mem_equal(URSUS_MIG_RI_ADDR, URSUS_UBI_READBACK_ADDR, URSUS_BOARD_DATA_SIZE))
            return ursus_mig_fail(ret ? ret : -EBADMSG);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=RI_READBACK\n");
        ursus_mig.last_success_stage = URSUS_MIG_VERIFY_RI;
        ursus_mig.stage = URSUS_MIG_VERIFY_FIP;
        break;
    case URSUS_MIG_VERIFY_FIP:
        ret = run_commandf("ubi read 0x%08lx fip 0x%lx", URSUS_UBI_READBACK_ADDR, (ulong)ursus_mig.fip_len);
        if (ret || ursus_mem_equal(URSUS_MIG_FIP_ADDR, URSUS_UBI_READBACK_ADDR, ursus_mig.fip_len))
            return ursus_mig_fail(ret ? ret : -EBADMSG);
        printf("URSUS_UBI_MIGRATION_CHECK_OK name=FIP_READBACK\n");
        ursus_mig.last_success_stage = URSUS_MIG_VERIFY_FIP;
        ursus_mig.stage = URSUS_MIG_VERIFY_FIT;
        break;
    case URSUS_MIG_VERIFY_FIT:
        ret = run_commandf("ubi read 0x%08lx fit 0x%lx", URSUS_UBI_READBACK_ADDR, (ulong)ursus_mig.fit_len);
        if (ret || ursus_mem_equal(ursus_mig.fit_addr, URSUS_UBI_READBACK_ADDR, ursus_mig.fit_len))
            return ursus_mig_fail(ret ? ret : -EBADMSG);
        ret = run_command("ubi check rootfs_data", 0);
        if (ret)
            return ursus_mig_fail(ret);
        {
            unsigned long rootfs_bytes = 0;
            ret = ursus_ubi_current_rootfs_data(&rootfs_bytes, NULL);
            if (ret)
                return ursus_mig_fail(ret);
            printf("URSUS_UBI_MIGRATION_CHECK_OK name=ROOTFS_DATA_PRESENT bytes=%lu policy=MAX_MINUS_16_PEBS\n", rootfs_bytes);
            if (ursus_ubi_store_rootfs_data_max(rootfs_bytes))
                printf("URSUS_ROOTFS_DATA_ENV_PERSIST_WARNING migration=1 bytes=%lu\n", rootfs_bytes);
        }
        ret = ursus_ubi_verify_volume_manifest(true);
        if (ret)
            return ursus_mig_fail(ret);
        printf("URSUS_UBI_MIGRATION_VOLUMES_VERIFIED count=7 ubootenv=OK ubootenv2=OK bosa=OK ri=OK fip=OK fit=OK rootfs_data=OK\n");
        ursus_mig.last_success_stage = URSUS_MIG_VERIFY_FIT;
        ursus_mig.stage = URSUS_MIG_COMMIT_BL2;
        break;
    case URSUS_MIG_COMMIT_BL2:
        printf("URSUS_UBI_MIGRATION_COMMIT_BEGIN target=BL2 size=0x%x prefix=0x%x preloader_offset=0x%x\n",
               (unsigned int)URSUS_UBI_BL2_SIZE, (unsigned int)URSUS_BL2_PRELOADER_OFF,
               (unsigned int)URSUS_BL2_PRELOADER_OFF);
        ursus_mig.commit_started = true;
        run_command("ubi detach", 0);
        ret = run_command("mtd erase ursus-ubi-bl2 0 0x20000", 0);
        if (ret) return ursus_mig_fail(ret);
        ret = run_commandf("mtd write ursus-ubi-bl2 0x%08lx 0 0x20000",
                           (ulong)(uintptr_t)ursus_bl2_candidate_buf);
        if (ret) return ursus_mig_fail(ret);
        ursus_mig.last_success_stage = URSUS_MIG_COMMIT_BL2;
        ursus_mig.stage = URSUS_MIG_VERIFY_BL2;
        break;
    case URSUS_MIG_VERIFY_BL2:
        ret = run_commandf("mtd read ursus-ubi-bl2 0x%08lx 0 0x20000", URSUS_UBI_READBACK_ADDR);
        if (ret || ursus_mem_equal((ulong)(uintptr_t)ursus_bl2_candidate_buf,
                                   URSUS_UBI_READBACK_ADDR, URSUS_UBI_BL2_SIZE))
            return ursus_mig_fail(ret ? ret : -EBADMSG);
        printf("URSUS_UBI_MIGRATION_BL2_VERIFIED bytes=0x%x sha256=6f9c928bad500de0339bbfdfa354c17a7ac044f96c913f3a01301971d6cd659d\n",
               (unsigned int)URSUS_UBI_BL2_SIZE);
        ursus_mig.last_success_stage = URSUS_MIG_VERIFY_BL2;
        ursus_mig.stage = URSUS_MIG_COMPLETE;
        ursus_mig.active = false;
        ursus_mig.error = 0;
        printf("URSUS_UBI_MIGRATION_COMPLETE reboot=MANUAL\n");
        return 1;
    default:
        return ursus_mig_fail(-EINVAL);
    }
    return 0;
}

int ursus_ubi_migrate_from_stock(ulong fit_addr, size_t fit_len,
                                 ulong preloader_addr, size_t preloader_len)
{
    int ret = ursus_ubi_migration_start(fit_addr, fit_len, preloader_addr, preloader_len);
    if (ret)
        return ret;
    while (ursus_ubi_migration_active()) {
        ret = ursus_ubi_migration_step();
        if (ret < 0)
            return ret;
        mdelay(10);
    }
    return ursus_ubi_migration_complete() ? 0 : ursus_ubi_migration_error();
}

static int do_ursusubiboot(struct cmd_tbl *cmdtp, int flag, int argc,
                           char *const argv[])
{
    return ursus_ubi_boot() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(ursusubiboot, 1, 0, do_ursusubiboot,
           URSUS_PRODUCT_VERSION " boot OpenWrt from UBI fit volume", "");
