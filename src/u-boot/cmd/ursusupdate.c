// SPDX-License-Identifier: GPL-2.0+
/* UrsusBoot FIP self-update backend for Nokia XG-040G-MD / Airoha AN7581.
 *
 * Transport-independent by design. TFTP, wget, X/YMODEM and WebFailsafe only
 * place a candidate FIP in RAM. This module validates and commits it.
 *
 * The same UBI transaction (fip.new -> readback -> atomic fip/fip.old swap ->
 * verify -> rollback) also performs the one-way replacement of UrsusBoot by
 * the pinned Vanilla OpenWrt U-Boot FIP. That is a separate, explicitly
 * requested candidate kind with its own validator: an UrsusBoot self-update
 * never accepts a Vanilla FIP and the Vanilla path never accepts UrsusBoot.
 * Board strings come from ursus_board_policy.h only (no literals here), so
 * the MF identity rewrite of this file cannot alter the Vanilla checks.
 */

#include <command.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/byteorder/little_endian.h>
#include <lzma/LzmaTools.h>
#include <mapmem.h>
#include <malloc.h>
#include <mtd.h>
#include <u-boot/sha256.h>
#include <ursus_board_policy.h>
#include <ursus_ubi.h>
#include <ursus_update.h>
#include <ursus_version.h>

/* UBI multiple-volume rename is the commit primitive for native-UBI FIP.
 * U-Boot does not expose it through the public command API, but the core UBI
 * API is linked in this build and supports atomic multi-volume rename. */
#include "../drivers/mtd/ubi/ubi.h"

#define URSUS_NAND_SIZE            0x10000000ULL
#define URSUS_STOCK_BOOT_SIZE      0x00080000UL
#define URSUS_STOCK_FIP_OFF        0x00000800UL
#define URSUS_STOCK_ENV_OFF        0x0007c000UL
#define URSUS_STOCK_FIP_MAX        (URSUS_STOCK_ENV_OFF - URSUS_STOCK_FIP_OFF)
#define URSUS_UBI_BASE             0x00020000ULL
#define URSUS_UBI_SIZE             0x0ffe0000ULL
#define URSUS_UBI_READBACK_ADDR    0x94000000UL
#define URSUS_UBI_FIP_VOL_SIZE     0x00100000UL
#define URSUS_FIP_MAGIC            0xaa640001U
#define URSUS_FIP_SERIAL           0x12345678U
#define URSUS_FIP_MAX_ENTRIES      32
#define URSUS_FIP_TOC_MIN          0x00000400UL
#define URSUS_FIP_RAW_MAX          0x00200000UL
#define URSUS_UPDATE_STAGE_HOLD_MS 250UL

/* SHA256 of the only Vanilla FIP this runtime may install. All zero = none
 * pinned, replacement refused. scripts/pin_vanilla_fip.py rewrites it. */
static const u8 ursus_vanilla_fip_sha256[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

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

enum ursus_update_stage {
    URSUS_UP_IDLE = 0,
    URSUS_UP_PRECHECK,
    URSUS_UP_STOCK_BACKUP,
    URSUS_UP_STOCK_ERASE,
    URSUS_UP_STOCK_WRITE,
    URSUS_UP_STOCK_VERIFY,
    URSUS_UP_UBI_ATTACH,
    URSUS_UP_UBI_STAGE_PREP,
    URSUS_UP_UBI_STAGE_WRITE,
    URSUS_UP_UBI_STAGE_VERIFY,
    URSUS_UP_UBI_PROMOTE,
    URSUS_UP_UBI_VERIFY,
    URSUS_UP_UBI_ROLLBACK,
    URSUS_UP_COMPLETE,
    URSUS_UP_FAILED,
};

struct ursus_update_ctx {
    enum ursus_fip_kind kind;
    enum ursus_update_stage stage;
    enum ursus_update_stage failed_stage;
    enum ursus_update_stage last_success_stage;
    ulong addr;
    size_t len;
    int error;
    bool active;
    bool announced;
    ulong announced_at;
    bool ubi_layout;
    bool ubi_repair_create;
    bool commit_started;
    bool write_started;
    char error_code[48];
    u8 digest[SHA256_SUM_LEN];
    u8 *stock_candidate;
    u8 *stock_readback;
};

static struct ursus_update_ctx ursus_up;
static struct mtd_info *ursus_update_nand;
static struct mtd_info *ursus_update_ubi_part;
static const char *ursus_update_ubi_part_name;
static bool ursus_update_parts_ready;
static const struct mtd_partition ursus_update_parts[] = {
    { .name = "ursus-update-ubi", .offset = URSUS_UBI_BASE, .size = URSUS_UBI_SIZE },
};

static bool ursus_uuid_zero(const u8 *u)
{
    unsigned int i;
    for (i = 0; i < 16; i++)
        if (u[i])
            return false;
    return true;
}

static bool ursus_mem_has(const u8 *buf, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!n || n > len)
        return false;
    for (i = 0; i + n <= len; i++)
        if (!memcmp(buf + i, needle, n))
            return true;
    return false;
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

static int ursus_update_register_ubi_part(void)
{
    int ret;

    if (ursus_update_parts_ready)
        return 0;
    ursus_update_nand = ursus_find_master_nand();
    if (!ursus_update_nand)
        return -ENODEV;
    if (ursus_update_nand->size != URSUS_NAND_SIZE ||
        ursus_update_nand->erasesize != 0x20000 ||
        ursus_update_nand->writesize != 0x800) {
        printf("URSUS_UPDATE_GEOMETRY_REJECT size=0x%llx erase=0x%x write=0x%x\n",
               (unsigned long long)ursus_update_nand->size,
               ursus_update_nand->erasesize, ursus_update_nand->writesize);
        return -EINVAL;
    }

    /* WebFailsafe/migration may already have registered the canonical full
     * UBI partition. Re-use it rather than creating a second overlapping MTD. */
    ursus_update_ubi_part = get_mtd_device_nm("ursus-ubi-full");
    if (!IS_ERR(ursus_update_ubi_part)) {
        ursus_update_ubi_part_name = "ursus-ubi-full";
        ursus_update_parts_ready = true;
        printf("URSUS_UPDATE_MTD_REUSE name=%s\n", ursus_update_ubi_part_name);
        return 0;
    }
    ursus_update_ubi_part = NULL;

    ret = add_mtd_partitions(ursus_update_nand, ursus_update_parts,
                             ARRAY_SIZE(ursus_update_parts));
    if (ret)
        return ret;
    ursus_update_ubi_part = get_mtd_device_nm("ursus-update-ubi");
    if (IS_ERR(ursus_update_ubi_part)) {
        ret = PTR_ERR(ursus_update_ubi_part);
        ursus_update_ubi_part = NULL;
        return ret;
    }
    ursus_update_ubi_part_name = "ursus-update-ubi";
    ursus_update_parts_ready = true;
    printf("URSUS_UPDATE_MTD_READY name=%s\n", ursus_update_ubi_part_name);
    return 0;
}

static int ursus_update_ensure_ubi_attachment(void)
{
    struct ubi_device *ubi;
    int ret;

    ret = ursus_update_register_ubi_part();
    if (ret)
        return ret;

    /* Reuse the live UBI device when Recovery already attached the canonical
     * UrsusBoot UBI partition. Never detach a healthy live UBI merely to
     * attach the same bytes again: that can fail while MTD users are active. */
    ubi = ubi_get_device(0);
    if (ubi) {
        if (ubi->mtd != ursus_update_ubi_part ||
            ubi->mtd->size != URSUS_UBI_SIZE ||
            ubi->mtd->erasesize != 0x20000 ||
            ubi->mtd->writesize != 0x800) {
            printf("URSUS_UPDATE_UBI_ATTACHMENT_MISMATCH attached=%s expected=%s size=0x%llx\n",
                   ubi->mtd && ubi->mtd->name ? ubi->mtd->name : "unknown",
                   ursus_update_ubi_part_name ? ursus_update_ubi_part_name : "unknown",
                   ubi->mtd ? (unsigned long long)ubi->mtd->size : 0ULL);
            ubi_put_device(ubi);
            return -EXDEV;
        }
        printf("URSUS_UPDATE_UBI_REUSE attached=%s action=NO_DETACH\n",
               ubi->mtd->name ? ubi->mtd->name : ursus_update_ubi_part_name);
        ubi_put_device(ubi);
    } else {
        /* No UBI device is currently attached. A single attach is allowed;
         * this path still never detaches another UBI device. */
        ret = run_commandf("ubi part %s", ursus_update_ubi_part_name);
        if (ret)
            return ret;
        ubi = ubi_get_device(0);
        if (!ubi)
            return -ENODEV;
        if (ubi->mtd != ursus_update_ubi_part) {
            printf("URSUS_UPDATE_UBI_ATTACHMENT_MISMATCH attached=%s expected=%s\n",
                   ubi->mtd && ubi->mtd->name ? ubi->mtd->name : "unknown",
                   ursus_update_ubi_part_name ? ursus_update_ubi_part_name : "unknown");
            ubi_put_device(ubi);
            return -EXDEV;
        }
        printf("URSUS_UPDATE_UBI_ATTACH_OK attached=%s action=ATTACH_ONCE\n",
               ubi->mtd->name ? ubi->mtd->name : ursus_update_ubi_part_name);
        ubi_put_device(ubi);
    }

    return 0;
}

static int ursus_fip_parse(const u8 *buf, size_t len, size_t *nt_off,
                           size_t *nt_size, size_t *declared_end)
{
    const struct ursus_fip_header *hdr;
    const struct ursus_fip_entry *ent;
    unsigned int i;
    bool nt = false, term = false;
    size_t noff = 0, nsize = 0, dend = 0;

    if (!buf || len < sizeof(*hdr) + 2 * sizeof(*ent))
        return -EINVAL;
    hdr = (const struct ursus_fip_header *)buf;
    if (le32_to_cpu(hdr->name) != URSUS_FIP_MAGIC ||
        le32_to_cpu(hdr->serial_number) != URSUS_FIP_SERIAL)
        return -EINVAL;
    ent = (const void *)(buf + sizeof(*hdr));
    for (i = 0; i < URSUS_FIP_MAX_ENTRIES; i++, ent++) {
        u64 off, size, end;
        if ((const u8 *)(ent + 1) > buf + len)
            return -EINVAL;
        off = le64_to_cpu(ent->offset_address);
        size = le64_to_cpu(ent->size);
        if (ursus_uuid_zero(ent->uuid)) {
            if (size || off < URSUS_FIP_TOC_MIN || off > len)
                return -EINVAL;
            term = true;
            dend = (size_t)off;
            break;
        }
        if (off < URSUS_FIP_TOC_MIN || size == 0 || off > len || size > len - off)
            return -ERANGE;
        end = off + size;
        if (end > len)
            return -ERANGE;
        if (!memcmp(ent->uuid, ursus_nt_fw_uuid, sizeof(ursus_nt_fw_uuid))) {
            nt = true;
            noff = (size_t)off;
            nsize = (size_t)size;
        }
    }
    if (!nt || !term)
        return -EINVAL;
    if (nt_off) *nt_off = noff;
    if (nt_size) *nt_size = nsize;
    if (declared_end) *declared_end = dend;
    return 0;
}


/* Unpack the LZMA NT_FW (BL33) of a FIP whose TOC ends exactly at len.
 * Shared by the UrsusBoot and Vanilla validators. Caller frees *raw. */
static int ursus_fip_bl33_unpack(const u8 *buf, size_t len, u8 **raw_out, SizeT *raw_len)
{
    size_t nt_off, nt_size, declared_end;
    const u8 *nt;
    u64 raw64 = 0;
    SizeT raw_cap, compressed_len;
    u8 *raw;
    int ret;
    unsigned int i;

    if (!len || len > URSUS_UBI_FIP_VOL_SIZE)
        return -EFBIG;
    ret = ursus_fip_parse(buf, len, &nt_off, &nt_size, &declared_end);
    if (ret)
        return ret;
    if (declared_end != len)
        return -EINVAL;
    if (nt_size < 13 || nt_off + nt_size > len)
        return -EINVAL;
    nt = buf + nt_off;
    for (i = 0; i < 8; i++)
        raw64 |= (u64)nt[5 + i] << (i * 8);
    if (raw64 < 0x10000 || raw64 > URSUS_FIP_RAW_MAX)
        return -EFBIG;
    raw_cap = (SizeT)raw64;
    raw = malloc(raw_cap);
    if (!raw)
        return -ENOMEM;
    compressed_len = nt_size;
    ret = lzmaBuffToBuffDecompress(raw, &raw_cap, nt, compressed_len);
    if (ret != SZ_OK || raw_cap != raw64) {
        printf("URSUS_UPDATE_REJECT reason=nt-fw-lzma ret=%d expected=%llu got=%lu\n",
               ret, (unsigned long long)raw64, (ulong)raw_cap);
        free(raw);
        return -EBADMSG;
    }
    *raw_out = raw;
    *raw_len = raw_cap;
    return 0;
}

static int ursus_fip_validate_current(const u8 *buf, size_t available_len, size_t *declared_end)
{
    size_t nt_off, nt_size, end;
    const u8 *nt;
    u64 raw64 = 0;
    SizeT raw_cap, compressed_len;
    u8 *raw;
    int ret;
    unsigned int i;
    bool ursus_id, stock_id;

    ret = ursus_fip_parse(buf, available_len, &nt_off, &nt_size, &end);
    if (ret)
        return ret;
    if (!end || end > available_len || nt_size < 13 || nt_off + nt_size > end)
        return -EINVAL;
    nt = buf + nt_off;
    for (i = 0; i < 8; i++)
        raw64 |= (u64)nt[5 + i] << (i * 8);
    if (raw64 < 0x10000 || raw64 > URSUS_FIP_RAW_MAX)
        return -EFBIG;
    raw_cap = (SizeT)raw64;
    raw = malloc(raw_cap);
    if (!raw)
        return -ENOMEM;
    compressed_len = nt_size;
    ret = lzmaBuffToBuffDecompress(raw, &raw_cap, nt, compressed_len);
    if (ret != SZ_OK || raw_cap != raw64) {
        free(raw);
        return -EBADMSG;
    }
    ursus_id = ursus_mem_has(raw, raw_cap, "U-Boot 2026.07-UrsusBoot-") &&
               ursus_mem_has(raw, raw_cap, "nokia,xg-040g-md") &&
               ursus_mem_has(raw, raw_cap, "airoha,an7581");
    stock_id = ursus_mem_has(raw, raw_cap, "XG040GMC2P5G") &&
               ursus_mem_has(raw, raw_cap, "AN7581");
    free(raw);
    if (!ursus_id && !stock_id) {
        printf("URSUS_UPDATE_CURRENT_FIP_REJECT reason=device-mismatch\n");
        return -ENODEV;
    }
    if (declared_end)
        *declared_end = end;
    printf("URSUS_UPDATE_CURRENT_FIP_OK identity=%s bytes=0x%x\n",
           ursus_id ? "URSUSBOOT_MD" : "NOKIA_XG040GMD_STOCK", (unsigned int)end);
    return 0;
}

static int ursus_fip_validate_buf(const u8 *buf, size_t len, bool stock_limit,
                                  u8 digest[SHA256_SUM_LEN])
{
    SizeT raw_cap;
    u8 *raw;
    int ret;

    if (stock_limit && len >= URSUS_STOCK_FIP_MAX)
        return -EFBIG;
    ret = ursus_fip_bl33_unpack(buf, len, &raw, &raw_cap);
    if (ret)
        return ret;
    if (!ursus_mem_has(raw, raw_cap, "U-Boot 2026.07-UrsusBoot-") ||
        !ursus_mem_has(raw, raw_cap, "nokia,xg-040g-md") ||
        !ursus_mem_has(raw, raw_cap, "airoha,an7581")) {
        printf("URSUS_UPDATE_REJECT reason=device-mismatch ursus=%u board=%u soc=%u\n",
               ursus_mem_has(raw, raw_cap, "U-Boot 2026.07-UrsusBoot-"),
               ursus_mem_has(raw, raw_cap, "nokia,xg-040g-md"),
               ursus_mem_has(raw, raw_cap, "airoha,an7581"));
        free(raw);
        return -ENODEV;
    }
    free(raw);
    sha256_csum_wd(buf, len, digest, CHUNKSZ_SHA256);
    return 0;
}

int ursus_fip_validate(ulong addr, size_t len, bool stock_limit)
{
    const u8 *buf = map_sysmem(addr, len);
    u8 digest[SHA256_SUM_LEN];
    int ret;
    unsigned int i;

    if (!buf)
        return -ENOMEM;
    ret = ursus_fip_validate_buf(buf, len, stock_limit, digest);
    unmap_sysmem(buf);
    if (ret) {
        printf("URSUS_UPDATE_PRECHECK_REJECT ret=%d len=%u stock_limit=%u\n",
               ret, (unsigned int)len, stock_limit);
        return ret;
    }
    printf("URSUS_UPDATE_PRECHECK_OK bytes=%u sha256=", (unsigned int)len);
    for (i = 0; i < SHA256_SUM_LEN; i++)
        printf("%02x", digest[i]);
    printf(" board=nokia_xg-040g-md soc=an7581\n");
    return 0;
}

static const char *ursus_fip_kind_name(enum ursus_fip_kind kind)
{
    return kind == URSUS_FIP_KIND_VANILLA ? "VANILLA" : "URSUSBOOT";
}

bool ursus_vanilla_fip_pinned(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(ursus_vanilla_fip_sha256); i++)
        if (ursus_vanilla_fip_sha256[i])
            return true;
    return false;
}

/* Vanilla OpenWrt U-Boot for exactly this board: this board's compatible, not
 * the sibling's, U-Boot 2026.07 without UrsusBoot lineage, and the pinned FIP
 * SHA256 (the decisive check; the identity checks explain a mismatch). */
static int ursus_vanilla_fip_validate_buf(const u8 *buf, size_t len, u8 digest[SHA256_SUM_LEN])
{
    bool board, other, uboot, ursus;
    SizeT raw_len;
    u8 *raw;
    int ret;

    ret = ursus_fip_bl33_unpack(buf, len, &raw, &raw_len);
    if (ret)
        return ret;
    board = ursus_mem_has(raw, raw_len, URSUS_BOARD_COMPATIBLE);
    other = ursus_mem_has(raw, raw_len, URSUS_BOARD_OTHER_COMPATIBLE);
    uboot = ursus_mem_has(raw, raw_len, "U-Boot 2026.07");
    ursus = ursus_mem_has(raw, raw_len, "U-Boot 2026.07-UrsusBoot-");
    free(raw);
    if (!board || other || !uboot || ursus) {
        printf("URSUS_UPDATE_REJECT reason=device-mismatch kind=VANILLA board=%u other_board=%u uboot=%u ursusboot=%u\n",
               board, other, uboot, ursus);
        return -ENODEV;
    }
    sha256_csum_wd(buf, len, digest, CHUNKSZ_SHA256);
    if (!ursus_vanilla_fip_pinned()) {
        printf("URSUS_UPDATE_REJECT reason=vanilla-not-pinned\n");
        return -EPERM;
    }
    if (memcmp(digest, ursus_vanilla_fip_sha256, SHA256_SUM_LEN)) {
        printf("URSUS_UPDATE_REJECT reason=vanilla-sha256-not-pinned\n");
        return -EPERM;
    }
    return 0;
}

int ursus_vanilla_fip_validate(ulong addr, size_t len, bool stock_limit)
{
    const u8 *buf;
    u8 digest[SHA256_SUM_LEN];
    int ret;
    unsigned int i;

    if (stock_limit) {
        /* Vanilla boots only from the native UBI layout with the fast BL2. */
        printf("URSUS_UPDATE_PRECHECK_REJECT kind=VANILLA reason=requires-ubi-layout\n");
        return -EPERM;
    }
    buf = map_sysmem(addr, len);
    if (!buf)
        return -ENOMEM;
    ret = ursus_vanilla_fip_validate_buf(buf, len, digest);
    unmap_sysmem(buf);
    if (ret) {
        printf("URSUS_UPDATE_PRECHECK_REJECT kind=VANILLA ret=%d len=%u\n", ret, (unsigned int)len);
        return ret;
    }
    printf("URSUS_UPDATE_PRECHECK_OK kind=VANILLA bytes=%u sha256=", (unsigned int)len);
    for (i = 0; i < SHA256_SUM_LEN; i++)
        printf("%02x", digest[i]);
    printf(" board=%s\n", URSUS_BOARD_COMPATIBLE);
    return 0;
}

static int ursus_fip_validate_kind(ulong addr, size_t len, bool stock_limit, enum ursus_fip_kind kind)
{
    return kind == URSUS_FIP_KIND_VANILLA ? ursus_vanilla_fip_validate(addr, len, stock_limit) :
                                            ursus_fip_validate(addr, len, stock_limit);
}

/*
 * The UBI environment volumes still hold UrsusBoot's environment (the STOCK->UBI
 * migration saves it). Vanilla U-Boot would load it and stop at its prompt
 * (no bootmenu_N, bootcmd=ursusdispatch). Invalidate both copies the way
 * OpenWrt's own reset_factory does after a FIP write, so Vanilla starts from its
 * default environment and runs its first-boot setup (MAC from ri, own env).
 */
#define URSUS_VANILLA_ENV_RESET_LEN 0x800

static int ursus_vanilla_reset_env(void)
{
    static const char *const vols[] = { "ubootenv", "ubootenv2" };
    size_t i, j;
    int ret;

    for (i = 0; i < ARRAY_SIZE(vols); i++) {
        u8 *buf;

        if (run_commandf("ubi check %s", vols[i])) {
            printf("URSUS_VANILLA_ENV_RESET_FAIL volume=%s reason=missing\n", vols[i]);
            return -ENOENT;
        }
        buf = map_sysmem(URSUS_UBI_READBACK_ADDR, URSUS_VANILLA_ENV_RESET_LEN);
        memset(buf, 0, URSUS_VANILLA_ENV_RESET_LEN);
        unmap_sysmem(buf);
        ret = run_commandf("ubi write 0x%08lx %s 0x%x", URSUS_UBI_READBACK_ADDR, vols[i],
                           URSUS_VANILLA_ENV_RESET_LEN);
        if (ret) {
            printf("URSUS_VANILLA_ENV_RESET_FAIL volume=%s reason=write ret=%d\n", vols[i], ret);
            return -EIO;
        }
        buf = map_sysmem(URSUS_UBI_READBACK_ADDR, URSUS_VANILLA_ENV_RESET_LEN);
        memset(buf, 0xa5, URSUS_VANILLA_ENV_RESET_LEN);
        unmap_sysmem(buf);
        ret = run_commandf("ubi read 0x%08lx %s 0x%x", URSUS_UBI_READBACK_ADDR, vols[i],
                           URSUS_VANILLA_ENV_RESET_LEN);
        buf = map_sysmem(URSUS_UBI_READBACK_ADDR, URSUS_VANILLA_ENV_RESET_LEN);
        for (j = 0; !ret && j < URSUS_VANILLA_ENV_RESET_LEN; j++)
            if (buf[j])
                ret = -EBADMSG;
        unmap_sysmem(buf);
        if (ret) {
            printf("URSUS_VANILLA_ENV_RESET_FAIL volume=%s reason=readback ret=%d\n", vols[i], ret);
            return -EIO;
        }
    }
    printf("URSUS_VANILLA_ENV_RESET_OK volumes=ubootenv,ubootenv2 next_boot=VANILLA_DEFAULT_ENV\n");
    return 0;
}

static int ursus_detect_ubi_layout(bool *is_ubi)
{
    u8 hdr[4];
    int ret;
    ursus_update_nand = ursus_find_master_nand();
    if (!ursus_update_nand)
        return -ENODEV;
    if (ursus_update_nand->size != URSUS_NAND_SIZE ||
        ursus_update_nand->erasesize != 0x20000 ||
        ursus_update_nand->writesize != 0x800)
        return -EINVAL;
    ret = ursus_read_exact(ursus_update_nand, URSUS_UBI_BASE, sizeof(hdr), hdr);
    if (ret)
        return ret;
    *is_ubi = !memcmp(hdr, "UBI#", 4);
    return 0;
}

static int ursus_ubi_atomic_switch(const char *current, const char *candidate,
                                   const char *backup)
{
    struct ubi_volume_desc *old_desc = NULL, *new_desc = NULL;
    struct ubi_device *ubi;
    struct ubi_rename_entry old_re, new_re;
    struct list_head list;
    int ret = 0;

    ubi = ubi_get_device(0);
    if (!ubi)
        return -ENODEV;
    old_desc = ubi_open_volume_nm(0, current, UBI_READWRITE);
    if (IS_ERR(old_desc)) {
        ret = PTR_ERR(old_desc);
        old_desc = NULL;
        goto out_put;
    }
    new_desc = ubi_open_volume_nm(0, candidate, UBI_READWRITE);
    if (IS_ERR(new_desc)) {
        ret = PTR_ERR(new_desc);
        new_desc = NULL;
        goto out;
    }
    memset(&old_re, 0, sizeof(old_re));
    memset(&new_re, 0, sizeof(new_re));
    old_re.new_name_len = strlen(backup);
    snprintf(old_re.new_name, sizeof(old_re.new_name), "%s", backup);
    old_re.desc = old_desc;
    new_re.new_name_len = strlen(current);
    snprintf(new_re.new_name, sizeof(new_re.new_name), "%s", current);
    new_re.desc = new_desc;
    INIT_LIST_HEAD(&old_re.list);
    INIT_LIST_HEAD(&new_re.list);
    INIT_LIST_HEAD(&list);
    list_add_tail(&old_re.list, &list);
    list_add_tail(&new_re.list, &list);
    ret = ubi_rename_volumes(ubi, &list);
out:
    if (new_desc)
        ubi_close_volume(new_desc);
    if (old_desc)
        ubi_close_volume(old_desc);
out_put:
    ubi_put_device(ubi);
    return ret;
}

static const char *ursus_update_stage_name(enum ursus_update_stage s);
static const char *ursus_update_detail_text(enum ursus_update_stage s);

static int ursus_update_fail_reason(int ret, const char *code)
{
    enum ursus_update_stage failed = ursus_up.stage;
    ursus_up.error = ret ? ret : -EIO;
    ursus_up.failed_stage = failed;
    if (!ursus_up.error_code[0])
        snprintf(ursus_up.error_code, sizeof(ursus_up.error_code), "%s", code ? code : "URSUSBOOT_UPDATE_FAILED");
    ursus_up.stage = URSUS_UP_FAILED;
    ursus_up.active = false;
    printf("URSUS_UPDATE_FAILED failed_stage=%s ret=%d code=%s layout=%s transaction=%s commit_started=%u\n",
           ursus_update_stage_name(failed), ursus_up.error, ursus_up.error_code,
           ursus_up.ubi_layout ? "UBI" : "STOCK",
           ursus_up.write_started ? "WRITE_STATE_UNKNOWN" : "NOT_STARTED", ursus_up.commit_started);
    return ursus_up.error;
}

static int ursus_update_fail(int ret)
{
    char code[48];
    snprintf(code, sizeof(code), "%s_ERR_%d", ursus_update_stage_name(ursus_up.stage), ret ? -ret : EIO);
    return ursus_update_fail_reason(ret, code);
}

static const char *ursus_update_stage_name(enum ursus_update_stage s)
{
    switch (s) {
    case URSUS_UP_IDLE: return "IDLE";
    case URSUS_UP_PRECHECK: return "PRECHECK";
    case URSUS_UP_STOCK_BACKUP: return "STOCK_BACKUP";
    case URSUS_UP_STOCK_ERASE: return "STOCK_ERASE";
    case URSUS_UP_STOCK_WRITE: return "STOCK_WRITE";
    case URSUS_UP_STOCK_VERIFY: return "STOCK_VERIFY";
    case URSUS_UP_UBI_ATTACH: return "UBI_ATTACH";
    case URSUS_UP_UBI_STAGE_PREP: return "UBI_STAGE_PREP";
    case URSUS_UP_UBI_STAGE_WRITE: return "UBI_STAGE_WRITE";
    case URSUS_UP_UBI_STAGE_VERIFY: return "UBI_STAGE_VERIFY";
    case URSUS_UP_UBI_PROMOTE: return "UBI_PROMOTE";
    case URSUS_UP_UBI_VERIFY: return "UBI_VERIFY";
    case URSUS_UP_UBI_ROLLBACK: return "UBI_ROLLBACK";
    case URSUS_UP_COMPLETE: return "COMPLETE";
    case URSUS_UP_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

static const char *ursus_update_detail_text(enum ursus_update_stage s)
{
    switch (s) {
    case URSUS_UP_PRECHECK: return "Validate FIP, board, SoC, NAND geometry and layout";
    case URSUS_UP_STOCK_BACKUP: return "Build 512 KiB stock bootloader candidate preserving prefix and tcboot env";
    case URSUS_UP_STOCK_ERASE: return "Erase stock bootloader mtd0 span";
    case URSUS_UP_STOCK_WRITE: return "Write preserved stock bootloader candidate with new UrsusBoot FIP";
    case URSUS_UP_STOCK_VERIFY: return "Read back and compare complete 512 KiB stock bootloader span";
    case URSUS_UP_UBI_ATTACH: return "Attach native UBI";
    case URSUS_UP_UBI_STAGE_PREP: return "Create fip.new while current fip remains bootable";
    case URSUS_UP_UBI_STAGE_WRITE: return "Write fip.new";
    case URSUS_UP_UBI_STAGE_VERIFY: return "Read back and validate fip.new";
    case URSUS_UP_UBI_PROMOTE: return "Atomically rename fip to fip.old and fip.new to fip";
    case URSUS_UP_UBI_VERIFY: return "Verify promoted fip";
    case URSUS_UP_UBI_ROLLBACK: return "Rollback promoted fip to previous fip.old";
    case URSUS_UP_COMPLETE: return ursus_up.kind == URSUS_FIP_KIND_VANILLA ?
        "Vanilla U-Boot is in fip (UrsusBoot kept as fip.old); reboot is operator-controlled" :
        "UrsusBoot update complete; reboot is operator-controlled";
    case URSUS_UP_FAILED: return ursus_up.kind == URSUS_FIP_KIND_VANILLA ?
        "Vanilla U-Boot replacement failed" : "UrsusBoot update failed";
    default: return "Idle";
    }
}

static unsigned int ursus_update_percent_value(enum ursus_update_stage s)
{
    switch (s) {
    case URSUS_UP_PRECHECK: return 5;
    case URSUS_UP_STOCK_BACKUP: return 20;
    case URSUS_UP_STOCK_ERASE: return 45;
    case URSUS_UP_STOCK_WRITE: return 70;
    case URSUS_UP_STOCK_VERIFY: return 92;
    case URSUS_UP_UBI_ATTACH: return 18;
    case URSUS_UP_UBI_STAGE_PREP: return 30;
    case URSUS_UP_UBI_STAGE_WRITE: return 50;
    case URSUS_UP_UBI_STAGE_VERIFY: return 70;
    case URSUS_UP_UBI_PROMOTE: return 85;
    case URSUS_UP_UBI_VERIFY: return 95;
    case URSUS_UP_UBI_ROLLBACK: return 90;
    case URSUS_UP_COMPLETE: return 100;
    case URSUS_UP_FAILED: return 100;
    default: return 0;
    }
}

bool ursus_fip_update_active(void) { return ursus_up.active; }
bool ursus_fip_update_complete(void) { return ursus_up.stage == URSUS_UP_COMPLETE; }
bool ursus_fip_update_failed(void) { return ursus_up.stage == URSUS_UP_FAILED; }
const char *ursus_fip_update_stage(void) { return ursus_update_stage_name(ursus_up.stage); }
const char *ursus_fip_update_detail(void) { return ursus_update_detail_text(ursus_up.stage); }
const char *ursus_fip_update_layout(void) { return ursus_up.ubi_layout ? "UBI" : "STOCK"; }
const char *ursus_fip_update_kind(void) { return ursus_fip_kind_name(ursus_up.kind); }
unsigned int ursus_fip_update_percent(void) { return ursus_update_percent_value(ursus_up.stage); }
int ursus_fip_update_error(void) { return ursus_up.error; }
const char *ursus_fip_update_failed_stage(void) { return ursus_up.failed_stage ? ursus_update_stage_name(ursus_up.failed_stage) : "NONE"; }
const char *ursus_fip_update_last_success_stage(void) { return ursus_up.last_success_stage ? ursus_update_stage_name(ursus_up.last_success_stage) : "IDLE"; }
const char *ursus_fip_update_error_code(void) { return ursus_up.error_code[0] ? ursus_up.error_code : "NONE"; }
const char *ursus_fip_update_transaction_state(void)
{
    if (ursus_up.stage == URSUS_UP_COMPLETE) return "COMPLETION_PROVEN";
    if (ursus_up.stage == URSUS_UP_FAILED) return ursus_up.write_started ? "WRITE_STATE_UNKNOWN" : "NOT_STARTED";
    if (ursus_up.active && ursus_up.write_started) return "IN_PROGRESS";
    return "NOT_STARTED";
}

static int ursus_fip_update_start_kind(ulong addr, size_t len, enum ursus_fip_kind kind)
{
    bool is_ubi = false;
    int ret;
    const u8 *buf;

    if (ursus_up.active)
        return -EBUSY;
    if (ursus_up.stock_candidate) { free(ursus_up.stock_candidate); ursus_up.stock_candidate = NULL; }
    if (ursus_up.stock_readback) { free(ursus_up.stock_readback); ursus_up.stock_readback = NULL; }
    memset(&ursus_up, 0, sizeof(ursus_up));
    ret = ursus_detect_ubi_layout(&is_ubi);
    if (ret)
        return ret;
    ret = ursus_fip_validate_kind(addr, len, !is_ubi, kind);
    if (ret)
        return ret;
    if (kind == URSUS_FIP_KIND_VANILLA) {
        ret = ursus_ubi_installed_bl2_matches_pin();
        if (ret)
            return ret;
    }
    buf = map_sysmem(addr, len);
    if (!buf)
        return -ENOMEM;
    sha256_csum_wd(buf, len, ursus_up.digest, CHUNKSZ_SHA256);
    unmap_sysmem(buf);
    ursus_up.addr = addr;
    ursus_up.len = len;
    ursus_up.ubi_layout = is_ubi;
    ursus_up.kind = kind;
    ursus_up.stage = URSUS_UP_PRECHECK;
    ursus_up.active = true;
    printf("URSUS_UPDATE_ARMED kind=%s layout=%s bytes=%u transport=RAM backend=shared\n",
           ursus_fip_kind_name(kind), is_ubi ? "UBI" : "STOCK", (unsigned int)len);
    if (!is_ubi)
        printf("URSUS_UPDATE_STOCK_POWERLOSS_RISK=1 reason=single-copy-bootloader\n");
    else
        printf("URSUS_UPDATE_UBI_TRANSACTION=fip.new->atomic-promote-fip.old\n");
    return 0;
}

int ursus_fip_update_start(ulong addr, size_t len)
{
    return ursus_fip_update_start_kind(addr, len, URSUS_FIP_KIND_URSUS);
}

int ursus_vanilla_fip_update_start(ulong addr, size_t len)
{
    return ursus_fip_update_start_kind(addr, len, URSUS_FIP_KIND_VANILLA);
}

int ursus_fip_update_step(void)
{
    int ret = 0;
    const u8 *src;

    if (!ursus_up.active)
        return ursus_up.stage == URSUS_UP_COMPLETE ? 1 :
               ursus_up.stage == URSUS_UP_FAILED ? ursus_up.error : 0;
    if (!ursus_up.announced) {
        ursus_up.announced = true;
        ursus_up.announced_at = get_timer(0);
        printf("URSUS_UPDATE_STAGE name=%s percent=%u detail=%s\n",
               ursus_update_stage_name(ursus_up.stage),
               ursus_update_percent_value(ursus_up.stage),
               ursus_update_detail_text(ursus_up.stage));
        return 0;
    }
    if (get_timer(ursus_up.announced_at) < URSUS_UPDATE_STAGE_HOLD_MS)
        return 0;
    ursus_up.announced = false;

    switch (ursus_up.stage) {
    case URSUS_UP_PRECHECK:
        ret = ursus_detect_ubi_layout(&ursus_up.ubi_layout);
        if (ret) return ursus_update_fail_reason(ret, "PRECHECK_LAYOUT_DETECT_FAILED");
        ret = ursus_fip_validate_kind(ursus_up.addr, ursus_up.len, !ursus_up.ubi_layout, ursus_up.kind);
        if (ret) return ursus_update_fail_reason(ret, "PRECHECK_FIP_VALIDATE_FAILED");
        if (ursus_up.kind == URSUS_FIP_KIND_VANILLA) {
            ret = ursus_ubi_installed_bl2_matches_pin();
            if (ret) return ursus_update_fail_reason(ret, "PRECHECK_INSTALLED_BL2_NOT_PINNED");
        }
        ursus_up.last_success_stage = URSUS_UP_PRECHECK;
        if (ursus_up.ubi_layout)
            ursus_up.stage = URSUS_UP_UBI_ATTACH;
        else
            ursus_up.stage = URSUS_UP_STOCK_BACKUP;
        break;

    case URSUS_UP_STOCK_BACKUP: {
        size_t old_end = 0;
        ret = ursus_bad_in_range(ursus_update_nand, 0, URSUS_STOCK_BOOT_SIZE);
        if (ret) return ursus_update_fail(ret < 0 ? ret : -EIO);
        ursus_up.stock_candidate = malloc(URSUS_STOCK_BOOT_SIZE);
        ursus_up.stock_readback = malloc(URSUS_STOCK_BOOT_SIZE);
        if (!ursus_up.stock_candidate || !ursus_up.stock_readback)
            return ursus_update_fail(-ENOMEM);
        ret = ursus_read_exact(ursus_update_nand, 0, URSUS_STOCK_BOOT_SIZE,
                               ursus_up.stock_candidate);
        if (ret) return ursus_update_fail(ret);
        ret = ursus_fip_validate_current(ursus_up.stock_candidate + URSUS_STOCK_FIP_OFF,
                                          URSUS_STOCK_FIP_MAX, &old_end);
        if (ret || old_end >= URSUS_STOCK_FIP_MAX)
            return ursus_update_fail(ret ? ret : -EINVAL);
        memset(ursus_up.stock_candidate + URSUS_STOCK_FIP_OFF, 0xff, URSUS_STOCK_FIP_MAX);
        src = map_sysmem(ursus_up.addr, ursus_up.len);
        if (!src) return ursus_update_fail(-ENOMEM);
        memcpy(ursus_up.stock_candidate + URSUS_STOCK_FIP_OFF, src, ursus_up.len);
        unmap_sysmem(src);
        printf("URSUS_UPDATE_STOCK_BACKUP_OK bytes=0x%lx old_fip=0x%x preserve_prefix=0x800 preserve_env=0x4000\n",
               (ulong)URSUS_STOCK_BOOT_SIZE, (unsigned int)old_end);
        ursus_up.last_success_stage = URSUS_UP_STOCK_BACKUP;
        ursus_up.stage = URSUS_UP_STOCK_ERASE;
        break;
    }

    case URSUS_UP_STOCK_ERASE: {
        ursus_up.write_started = true;
        struct erase_info ei = { .addr = 0, .len = URSUS_STOCK_BOOT_SIZE };
        ursus_up.commit_started = true;
        printf("URSUS_UPDATE_ERASE_BEGIN layout=STOCK off=0 size=0x%lx\n", (ulong)URSUS_STOCK_BOOT_SIZE);
        ret = mtd_erase(ursus_update_nand, &ei);
        if (ret) return ursus_update_fail(ret);
        printf("URSUS_UPDATE_ERASE_OK layout=STOCK\n");
        ursus_up.last_success_stage = URSUS_UP_STOCK_ERASE;
        ursus_up.stage = URSUS_UP_STOCK_WRITE;
        break;
    }

    case URSUS_UP_STOCK_WRITE: {
        size_t written = 0;
        ret = mtd_write(ursus_update_nand, 0, URSUS_STOCK_BOOT_SIZE,
                        &written, ursus_up.stock_candidate);
        if (ret || written != URSUS_STOCK_BOOT_SIZE)
            return ursus_update_fail(ret ? ret : -EIO);
        printf("URSUS_UPDATE_WRITE_OK layout=STOCK bytes=0x%lx\n", (ulong)URSUS_STOCK_BOOT_SIZE);
        ursus_up.last_success_stage = URSUS_UP_STOCK_WRITE;
        ursus_up.stage = URSUS_UP_STOCK_VERIFY;
        break;
    }

    case URSUS_UP_STOCK_VERIFY:
        ret = ursus_read_exact(ursus_update_nand, 0, URSUS_STOCK_BOOT_SIZE,
                               ursus_up.stock_readback);
        if (ret || memcmp(ursus_up.stock_candidate, ursus_up.stock_readback,
                          URSUS_STOCK_BOOT_SIZE))
            return ursus_update_fail(ret ? ret : -EBADMSG);
        printf("URSUS_UPDATE_READBACK_OK layout=STOCK bytes=0x%lx prefix=preserved env=preserved\n",
               (ulong)URSUS_STOCK_BOOT_SIZE);
        ursus_up.last_success_stage = URSUS_UP_STOCK_VERIFY;
        ursus_up.stage = URSUS_UP_COMPLETE;
        ursus_up.active = false;
        free(ursus_up.stock_candidate); ursus_up.stock_candidate = NULL;
        free(ursus_up.stock_readback); ursus_up.stock_readback = NULL;
        printf("URSUS_UPDATE_COMMIT_OK layout=STOCK reboot=MANUAL\n");
        return 1;

    case URSUS_UP_UBI_ATTACH:
        ret = ursus_update_ensure_ubi_attachment();
        if (ret) return ursus_update_fail_reason(ret,
            ret == -EXDEV ? "UBI_ATTACHMENT_MISMATCH" : "UBI_ATTACH_FAILED");
        if (run_command("ubi check fip", 0)) {
            if (ursus_up.kind != URSUS_FIP_KIND_URSUS)
                return ursus_update_fail_reason(-ENOENT, "UBI_ACTIVE_FIP_MISSING");
            ursus_up.ubi_repair_create = true;
            printf("URSUS_UPDATE_RECOVERY_CREATE reason=active-fip-missing candidate=validated-ursusboot preserve=fip.old\n");
        }
        ursus_up.last_success_stage = URSUS_UP_UBI_ATTACH;
        ursus_up.stage = URSUS_UP_UBI_STAGE_PREP;
        break;

    case URSUS_UP_UBI_STAGE_PREP:
        ursus_up.write_started = true;
        if (!run_command("ubi check fip.new", 0))
            run_command("ubi remove fip.new", 0);
        if (!ursus_up.ubi_repair_create && !run_command("ubi check fip.old", 0))
            run_command("ubi remove fip.old", 0);
        if (!run_command("ubi check fip.bad", 0))
            run_command("ubi remove fip.bad", 0);
        ret = run_command("ubi create fip.new 0x100000 static", 0);
        if (ret) return ursus_update_fail(ret);
        printf("URSUS_UPDATE_STAGE_READY layout=UBI volume=fip.new current=%s\n",
               ursus_up.ubi_repair_create ? "fip-missing-recovery-create" : "fip-intact");
        ursus_up.last_success_stage = URSUS_UP_UBI_STAGE_PREP;
        ursus_up.stage = URSUS_UP_UBI_STAGE_WRITE;
        break;

    case URSUS_UP_UBI_STAGE_WRITE:
        ret = run_commandf("ubi write 0x%08lx fip.new 0x%lx", ursus_up.addr, (ulong)ursus_up.len);
        if (ret) return ursus_update_fail(ret);
        printf("URSUS_UPDATE_WRITE_OK layout=UBI volume=fip.new bytes=%u\n", (unsigned int)ursus_up.len);
        ursus_up.last_success_stage = URSUS_UP_UBI_STAGE_WRITE;
        ursus_up.stage = URSUS_UP_UBI_STAGE_VERIFY;
        break;

    case URSUS_UP_UBI_STAGE_VERIFY:
        ret = run_commandf("ubi read 0x%08lx fip.new 0x%lx", URSUS_UBI_READBACK_ADDR, (ulong)ursus_up.len);
        if (ret) return ursus_update_fail(ret);
        ret = ursus_fip_validate_kind(URSUS_UBI_READBACK_ADDR, ursus_up.len, false, ursus_up.kind);
        if (ret) return ursus_update_fail(ret);
        src = map_sysmem(ursus_up.addr, ursus_up.len);
        if (!src) return ursus_update_fail(-ENOMEM);
        {
            const u8 *rb = map_sysmem(URSUS_UBI_READBACK_ADDR, ursus_up.len);
            if (!rb) { unmap_sysmem(src); return ursus_update_fail(-ENOMEM); }
            ret = memcmp(src, rb, ursus_up.len) ? -EBADMSG : 0;
            unmap_sysmem(rb);
        }
        unmap_sysmem(src);
        if (ret) return ursus_update_fail(ret);
        printf("URSUS_UPDATE_READBACK_OK layout=UBI volume=fip.new\n");
        ursus_up.last_success_stage = URSUS_UP_UBI_STAGE_VERIFY;
        ursus_up.stage = URSUS_UP_UBI_PROMOTE;
        break;

    case URSUS_UP_UBI_PROMOTE:
        ursus_up.commit_started = true;
        if (ursus_up.ubi_repair_create) {
            printf("URSUS_UPDATE_COMMIT_BEGIN layout=UBI recovery=create-fip source=fip.new backup=preserved-if-present\n");
            ret = run_command("ubi rename fip.new fip", 0);
            if (ret) return ursus_update_fail_reason(ret, "RECOVERY_FIP_RENAME_FAILED");
            printf("URSUS_UPDATE_PROMOTE_OK layout=UBI recovery=create-fip backup=preserved-if-present\n");
        } else {
            printf("URSUS_UPDATE_COMMIT_BEGIN layout=UBI atomic=fip->fip.old,fip.new->fip\n");
            ret = ursus_ubi_atomic_switch("fip", "fip.new", "fip.old");
            if (ret) return ursus_update_fail(ret);
            printf("URSUS_UPDATE_PROMOTE_OK layout=UBI backup=fip.old\n");
        }
        ursus_up.last_success_stage = URSUS_UP_UBI_PROMOTE;
        ursus_up.stage = URSUS_UP_UBI_VERIFY;
        break;

    case URSUS_UP_UBI_VERIFY:
        ret = run_commandf("ubi read 0x%08lx fip 0x%lx", URSUS_UBI_READBACK_ADDR, (ulong)ursus_up.len);
        if (!ret)
            ret = ursus_fip_validate_kind(URSUS_UBI_READBACK_ADDR, ursus_up.len, false, ursus_up.kind);
        if (ret) {
            if (ursus_up.ubi_repair_create) {
                printf("URSUS_UPDATE_POSTCOMMIT_VERIFY_FAIL ret=%d action=NONE reason=no-active-fip-backup\n", ret);
                return ursus_update_fail_reason(ret, "RECOVERY_FIP_POSTCOMMIT_VERIFY_FAILED");
            }
            printf("URSUS_UPDATE_POSTCOMMIT_VERIFY_FAIL ret=%d action=ROLLBACK\n", ret);
            ursus_up.last_success_stage = URSUS_UP_UBI_VERIFY;
            ursus_up.stage = URSUS_UP_UBI_ROLLBACK;
            break;
        }
        /* Vanilla must not boot with UrsusBoot's environment; if it cannot be
         * reset, keep UrsusBoot (its environment is consistent) instead. */
        if (ursus_up.kind == URSUS_FIP_KIND_VANILLA && ursus_vanilla_reset_env()) {
            printf("URSUS_UPDATE_POSTCOMMIT_ENV_FAIL action=ROLLBACK\n");
            ursus_up.last_success_stage = URSUS_UP_UBI_VERIFY;
            ursus_up.stage = URSUS_UP_UBI_ROLLBACK;
            break;
        }
        ursus_up.stage = URSUS_UP_COMPLETE;
        ursus_up.active = false;
        printf("URSUS_UPDATE_COMMIT_OK layout=UBI kind=%s recovery_create=%u backup=%s reboot=MANUAL\n",
               ursus_fip_kind_name(ursus_up.kind), ursus_up.ubi_repair_create ? 1U : 0U,
               ursus_up.ubi_repair_create ? "preserved-if-present" : "fip.old");
        if (ursus_up.kind == URSUS_FIP_KIND_VANILLA)
            printf("URSUS_VANILLA_REPLACE_COMPLETE fip=VANILLA fip.old=URSUSBOOT env=RESET next_boot=VANILLA_UBOOT\n");
        return 1;

    case URSUS_UP_UBI_ROLLBACK:
        if (!run_command("ubi check fip.bad", 0))
            run_command("ubi remove fip.bad", 0);
        ret = ursus_ubi_atomic_switch("fip", "fip.old", "fip.bad");
        if (ret)
            return ursus_update_fail(ret);
        printf("URSUS_UPDATE_ROLLBACK_OK restored=fip failed=fip.bad\n");
        return ursus_update_fail(-EBADMSG);

    default:
        return ursus_update_fail(-EINVAL);
    }
    return 0;
}

static int ursus_update_run(ulong addr, size_t len, enum ursus_fip_kind kind)
{
    int ret = ursus_fip_update_start_kind(addr, len, kind);
    if (ret)
        return ret;
    while (ursus_fip_update_active()) {
        ret = ursus_fip_update_step();
        if (ret < 0)
            return ret;
        mdelay(10);
    }
    return ursus_fip_update_complete() ? 0 : ursus_fip_update_error();
}

static int do_ursusupdate(struct cmd_tbl *cmdtp, int flag, int argc,
                          char *const argv[])
{
    ulong addr, len;
    bool is_ubi = false;
    int ret;

    if (argc == 4 && !strcmp(argv[1], "check")) {
        addr = hextoul(argv[2], NULL);
        len = hextoul(argv[3], NULL);
        ret = ursus_detect_ubi_layout(&is_ubi);
        if (ret)
            return CMD_RET_FAILURE;
        return ursus_fip_validate(addr, len, !is_ubi) ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
    }
    if (argc == 4 && !strcmp(argv[1], "write")) {
        addr = hextoul(argv[2], NULL);
        len = hextoul(argv[3], NULL);
        ret = ursus_update_run(addr, len, URSUS_FIP_KIND_URSUS);
        return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
    }
    if (argc == 4 && !strcmp(argv[1], "vanilla-check")) {
        addr = hextoul(argv[2], NULL);
        len = hextoul(argv[3], NULL);
        ret = ursus_detect_ubi_layout(&is_ubi);
        if (ret || ursus_vanilla_fip_validate(addr, len, !is_ubi))
            return CMD_RET_FAILURE;
        return ursus_ubi_installed_bl2_matches_pin() ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
    }
    if (argc == 5 && !strcmp(argv[1], "vanilla-write") &&
        !strcmp(argv[4], "REPLACE-URSUSBOOT-WITH-VANILLA")) {
        addr = hextoul(argv[2], NULL);
        len = hextoul(argv[3], NULL);
        ret = ursus_update_run(addr, len, URSUS_FIP_KIND_VANILLA);
        return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
    }
    return CMD_RET_USAGE;
}

U_BOOT_CMD(ursusupdate, 5, 0, do_ursusupdate,
           URSUS_PRODUCT_VERSION " validate/update UrsusBoot FIP from RAM",
           "check <addr> <len>\n"
           "ursusupdate write <addr> <len>\n"
           "  transport examples:\n"
           "  tftpboot ${loadaddr} ursusboot.fip; ursusupdate write ${loadaddr} ${filesize}\n"
           "  loadx ${loadaddr}; ursusupdate write ${loadaddr} ${filesize}\n"
           "  wget ${loadaddr} http://server/ursusboot.fip; ursusupdate write ${loadaddr} ${filesize}\n"
           "ursusupdate vanilla-check <addr> <len>\n"
           "ursusupdate vanilla-write <addr> <len> REPLACE-URSUSBOOT-WITH-VANILLA\n"
           "  one-way: pinned Vanilla OpenWrt U-Boot FIP into UBI fip; UrsusBoot kept as fip.old");
