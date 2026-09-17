// SPDX-License-Identifier: GPL-2.0+
/* UrsusBoot Nokia XG-040G-MD persistent StockBridge. */

#include <command.h>
#include <cpu_func.h>
#include <env.h>
#include <image.h>
#include <linux/errno.h>
#include <linux/mtd/mtd.h>
#include <linux/byteorder/little_endian.h>
#include <linux/libfdt.h>
#include <malloc.h>
#include <mtd.h>
#include <u-boot/crc.h>
#include <ursus_version.h>

#define STOCK_MASTER_BASE     0x000c0000ULL
#define STOCK_SLAVE_BASE      0x02940000ULL
#define STOCK_SLOT_SIZE       0x02880000ULL
#define STOCK_ENV_BASE        0x0007c000ULL
#define STOCK_ENV_SIZE        0x00004000U
#define STOCK_TRX_HDR_SIZE    0x00000100U
#define FIP_TOC_HEADER_NAME   0xaa640001U
#define FIP_TOC_SERIAL        0x12345678U
#define HDR2_SIZE             STOCK_TRX_HDR_SIZE
#define FIT_LOAD_ADDR         0x88000000UL
#define FIT_MAX_SIZE          0x02800000U
#define FIP_TOC_READ          0x1000U
#define READ_CHUNK            0x00100000U
#define MAX_TOC_ENTRIES       32
#define BOOTARGS_MAX          1536

/* Full-backup oracle values. A mismatch warns but does not hard-fail stock boot. */
#define ORACLE_FIT_SIZE        0x020645f7U
#define ORACLE_KERNEL_OFF      0x00004cc4ULL
#define ORACLE_KERNEL_SIZE     0x003aa916U
#define ORACLE_ROOTFS_OFF      0x003af6d8ULL
#define ORACLE_ROOTFS_SIZE     0x01cb6bddU

static const u8 nokia_nt_fw_uuid[16] = {
    0xd6, 0xd0, 0xee, 0xa7, 0xfc, 0xea, 0xd5, 0x4b,
    0x97, 0x82, 0x99, 0x34, 0xf2, 0x34, 0xb6, 0xe4
};

struct fip_toc_header_local {
    __le32 name;
    __le32 serial_number;
    __le64 flags;
} __packed;

struct fip_toc_entry_local {
    u8 uuid[16];
    __le64 offset_address;
    __le64 size;
    __le64 flags;
} __packed;

struct stock_slot {
    const char *name;
    u64 base;
    u64 nt_off;
    u64 nt_size;
    u64 fit_phys;
    u32 fit_size;
    int bad_blocks;
};

struct stock_layout {
    u64 kernel_off; /* stock tclinux_info offset: FIT data offset + FIP NT-FW offset */
    u32 kernel_size;
    u64 rootfs_off;
    u32 rootfs_size;
};

struct stock_env {
    u8 *buf;
    bool crc_ok;
};

static bool uuid_is_zero(const u8 *u)
{
    int i;

    for (i = 0; i < 16; i++)
        if (u[i])
            return false;
    return true;
}

static struct mtd_info *find_master_nand(void)
{
    struct mtd_info *mtd;

    mtd_probe_devices();
    mtd_for_each_device(mtd) {
        if (mtd_is_partition(mtd))
            continue;
        if (mtd->type == MTD_NANDFLASH || mtd->type == MTD_MLCNANDFLASH)
            return mtd;
    }
    return NULL;
}

static int read_exact(struct mtd_info *mtd, loff_t off, size_t len, void *buf)
{
    size_t retlen = 0;
    int ret = mtd_read(mtd, off, len, &retlen, buf);

    if (ret == -EUCLEAN)
        ret = 0;
    if (ret || retlen != len) {
        printf("URSUS_STOCKBOOT_READ_FAIL off=0x%llx len=0x%zx ret=%d retlen=0x%zx\n",
               (unsigned long long)off, len, ret, retlen);
        return -EIO;
    }
    return 0;
}

/* Read an arbitrary byte range without relying on partial-page NAND reads. */
static int read_range(struct mtd_info *mtd, u64 off, size_t len, u8 *dst)
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
            if (direct > READ_CHUNK)
                direct = READ_CHUNK - (READ_CHUNK % page);
            if (read_exact(mtd, off, direct, dst)) {
                ret = -EIO;
                break;
            }
            off += direct;
            dst += direct;
            len -= direct;
            continue;
        }

        if (read_exact(mtd, base, page, scratch)) {
            ret = -EIO;
            break;
        }
        take = min_t(size_t, len, page - in_page);
        memcpy(dst, scratch + in_page, take);
        off += take;
        dst += take;
        len -= take;
    }

    free(scratch);
    return ret;
}

static int count_bad_blocks(struct mtd_info *mtd, u64 start, u64 len)
{
    u64 eb, end;
    int bad = 0;

    if (!mtd->erasesize)
        return -EINVAL;
    eb = start - (start % mtd->erasesize);
    end = start + len;
    for (; eb < end; eb += mtd->erasesize) {
        int ret = mtd_block_isbad(mtd, eb);
        if (ret < 0)
            return ret;
        if (ret > 0) {
            printf("URSUS_STOCKBOOT_BADBLOCK phys=0x%llx\n",
                   (unsigned long long)eb);
            bad++;
        }
    }
    return bad;
}

static int inspect_slot(struct mtd_info *mtd, struct stock_slot *slot)
{
    u8 *tocbuf;
    struct fip_toc_header_local *hdr;
    struct fip_toc_entry_local *ent;
    u8 hdrpage[2048];
    u8 fithead[64];
    int i, ret = -EINVAL;

    if (slot->base + STOCK_SLOT_SIZE > mtd->size)
        return -ERANGE;

    tocbuf = malloc(FIP_TOC_READ);
    if (!tocbuf)
        return -ENOMEM;
    if (read_range(mtd, slot->base, FIP_TOC_READ, tocbuf))
        goto out;

    hdr = (struct fip_toc_header_local *)tocbuf;
    if (le32_to_cpu(hdr->name) != FIP_TOC_HEADER_NAME ||
        le32_to_cpu(hdr->serial_number) != FIP_TOC_SERIAL) {
        printf("URSUS_STOCKBOOT_%s_FIP_INVALID name=0x%08x serial=0x%08x\n",
               slot->name, le32_to_cpu(hdr->name), le32_to_cpu(hdr->serial_number));
        goto out;
    }

    ent = (struct fip_toc_entry_local *)(tocbuf + sizeof(*hdr));
    for (i = 0; i < MAX_TOC_ENTRIES; i++, ent++) {
        if ((u8 *)(ent + 1) > tocbuf + FIP_TOC_READ)
            break;
        if (uuid_is_zero(ent->uuid))
            break;
        if (!memcmp(ent->uuid, nokia_nt_fw_uuid, sizeof(nokia_nt_fw_uuid))) {
            slot->nt_off = le64_to_cpu(ent->offset_address);
            slot->nt_size = le64_to_cpu(ent->size);
            break;
        }
    }
    if (!slot->nt_off || slot->nt_size <= HDR2_SIZE) {
        printf("URSUS_STOCKBOOT_%s_NTFW_NOT_FOUND\n", slot->name);
        goto out;
    }

    if (slot->base + slot->nt_off + slot->nt_size > slot->base + STOCK_SLOT_SIZE) {
        printf("URSUS_STOCKBOOT_%s_NTFW_RANGE_INVALID\n", slot->name);
        goto out;
    }

    if (read_range(mtd, slot->base + slot->nt_off, sizeof(hdrpage), hdrpage))
        goto out;
    if (memcmp(hdrpage, "HDR2", 4)) {
        printf("URSUS_STOCKBOOT_%s_HDR2_INVALID bytes=%02x%02x%02x%02x\n",
               slot->name, hdrpage[0], hdrpage[1], hdrpage[2], hdrpage[3]);
        goto out;
    }

    slot->fit_phys = slot->base + slot->nt_off + HDR2_SIZE;
    memcpy(fithead, hdrpage + HDR2_SIZE, sizeof(fithead));
    if (fdt_check_header(fithead)) {
        printf("URSUS_STOCKBOOT_%s_FIT_HEADER_INVALID phys=0x%llx\n",
               slot->name, (unsigned long long)slot->fit_phys);
        goto out;
    }
    slot->fit_size = fdt_totalsize(fithead);
    if (!slot->fit_size || slot->fit_size > FIT_MAX_SIZE ||
        slot->fit_size > slot->nt_size - HDR2_SIZE) {
        printf("URSUS_STOCKBOOT_%s_FIT_SIZE_INVALID size=0x%x nt_size=0x%llx\n",
               slot->name, slot->fit_size, (unsigned long long)slot->nt_size);
        goto out;
    }

    slot->bad_blocks = count_bad_blocks(mtd, slot->fit_phys, slot->fit_size);
    if (slot->bad_blocks < 0)
        goto out;

    printf("URSUS_STOCKBOOT_SLOT %s base=0x%llx nt_off=0x%llx nt_size=0x%llx fit=0x%llx fit_size=0x%x bad_blocks=%d\n",
           slot->name, (unsigned long long)slot->base,
           (unsigned long long)slot->nt_off,
           (unsigned long long)slot->nt_size,
           (unsigned long long)slot->fit_phys, slot->fit_size,
           slot->bad_blocks);
    ret = 0;
out:
    free(tocbuf);
    return ret;
}

static int fit_image_info(void *fit, u64 nt_off, struct stock_layout *layout)
{
    int images, node;
    int len;
    const void *data;
    uintptr_t base = (uintptr_t)fit;
    uintptr_t ptr;

    if (fdt_check_header(fit))
        return -EINVAL;
    images = fdt_path_offset(fit, "/images");
    if (images < 0)
        return images;

    node = fdt_subnode_offset(fit, images, "kernel@1");
    if (node < 0)
        return node;
    data = fdt_getprop(fit, node, "data", &len);
    if (!data || len <= 0)
        return -EINVAL;
    ptr = (uintptr_t)data;
    if (ptr < base || ptr + len > base + fdt_totalsize(fit))
        return -ERANGE;
    layout->kernel_off = (u64)(ptr - base) + nt_off;
    layout->kernel_size = len;

    node = fdt_subnode_offset(fit, images, "filesystem@1");
    if (node < 0)
        return node;
    data = fdt_getprop(fit, node, "data", &len);
    if (!data || len <= 0)
        return -EINVAL;
    ptr = (uintptr_t)data;
    if (ptr < base || ptr + len > base + fdt_totalsize(fit))
        return -ERANGE;
    layout->rootfs_off = (u64)(ptr - base) + nt_off;
    layout->rootfs_size = len;

    return 0;
}

static int stock_env_load(struct mtd_info *mtd, struct stock_env *se)
{
    u32 stored, calc;

    memset(se, 0, sizeof(*se));
    se->buf = malloc(STOCK_ENV_SIZE);
    if (!se->buf)
        return -ENOMEM;
    if (read_range(mtd, STOCK_ENV_BASE, STOCK_ENV_SIZE, se->buf))
        return -EIO;
    memcpy(&stored, se->buf, sizeof(stored));
    stored = le32_to_cpu(stored);
    calc = crc32(0, se->buf + 4, STOCK_ENV_SIZE - 4);
    se->crc_ok = stored == calc;
    printf("URSUS_STOCKBOOT_ENV_CRC stored=0x%08x calc=0x%08x status=%s\n",
           stored, calc, se->crc_ok ? "OK" : "FAIL");
    return se->crc_ok ? 0 : -EBADMSG;
}

static void stock_env_free(struct stock_env *se)
{
    free(se->buf);
    se->buf = NULL;
}

static const char *stock_env_get(const struct stock_env *se, const char *key)
{
    const char *p, *end;
    size_t keylen;

    if (!se->buf || !se->crc_ok)
        return NULL;
    p = (const char *)se->buf + 4;
    end = (const char *)se->buf + STOCK_ENV_SIZE;
    keylen = strlen(key);

    while (p < end && *p) {
        size_t left = end - p;
        size_t n = strnlen(p, left);
        if (n == left)
            break;
        if (n > keylen && !memcmp(p, key, keylen) && p[keylen] == '=')
            return p + keylen + 1;
        p += n + 1;
    }
    return NULL;
}

static int append_arg(char *dst, size_t cap, const char *key, const char *val)
{
    size_t used;
    int n;

    if (!val || !*val)
        return 0;
    used = strlen(dst);
    if (used >= cap)
        return -ENOSPC;
    n = snprintf(dst + used, cap - used, "%s=%s ", key, val);
    if (n < 0 || (size_t)n >= cap - used)
        return -ENOSPC;
    return 0;
}

static int build_stock_bootargs(const struct stock_env *se,
                                const struct stock_slot *master,
                                const struct stock_slot *slave,
                                const struct stock_layout *layout,
                                int bootflag, char *out, size_t outsz)
{
    static const char *const passthrough[] = {
        "sdram_conf", "vendor_name", "product_name", "ethaddr",
        "snmp_sysobjid", "country_code", "ether_gpio", "power_gpio",
        "dsl_gpio", "internet_gpio", "multi_upgrade_gpio",
        "onu_type", "qdma_init", "dram_limit",
        "serdes_pon", "serdes_ethernet", "serdes_usb1", "board_args",
    };
    char tclinux[256];
    const char *root, *console;
    size_t i;

    out[0] = '\0';
    for (i = 0; i < ARRAY_SIZE(passthrough); i++) {
        const char *v = stock_env_get(se, passthrough[i]);
        if (append_arg(out, outsz, passthrough[i], v))
            return -ENOSPC;
    }

    root = bootflag ? "/dev/mtdblock5 ro" : stock_env_get(se, "root");
    if (!root)
        root = bootflag ? "/dev/mtdblock5 ro" : "/dev/mtdblock3 ro";
    console = stock_env_get(se, "console");
    if (!console)
        console = "ttyS0,115200n8 earlycon";

    if (append_arg(out, outsz, "root", root) ||
        append_arg(out, outsz, "console", console))
        return -ENOSPC;

    /*
     * Nokia MD tcboot adjusts SerDes routing after reading BoardID from RI.
     * Persistent env contains wifi1=01,wifi2=01,usb2=00, but the stock boot
     * path for XG040GMC2P5G explicitly powers those unused lanes down and
     * passes wifi1=05,wifi2=05,usb2=02 to Linux.  Omitting this tcboot board
     * fix makes the vendor hw_nat module panic with "No PCIE for SerDes-WiFi1".
     *
     * This MD-specific path reproduces the observed tcboot
     * runtime arguments instead of pretending the persistent env is final.
     */
    if (append_arg(out, outsz, "serdes_wifi1", "05") ||
        append_arg(out, outsz, "serdes_wifi2", "05") ||
        append_arg(out, outsz, "serdes_usb2", "02"))
        return -ENOSPC;

    /* Match the Nokia stock boot ABI tclinux_info shape.  The non-booted
     * slave bank is represented by the same placeholders tcboot emits; Linux
     * then recreates the vendor fallback slave partition geometry. */
    if (!bootflag) {
        snprintf(tclinux, sizeof(tclinux),
                 "0x%x,0x%llx,0x%x,0x%llx,0x%x,0x0,0x2000,0x0,0x2000,0x0",
                 master->fit_size,
                 (unsigned long long)layout->kernel_off, layout->kernel_size,
                 (unsigned long long)layout->rootfs_off, layout->rootfs_size);
    } else {
        snprintf(tclinux, sizeof(tclinux),
                 "0x0,0x2000,0x0,0x2000,0x0,0x%x,0x%llx,0x%x,0x%llx,0x%x",
                 slave->fit_size,
                 (unsigned long long)layout->kernel_off, layout->kernel_size,
                 (unsigned long long)layout->rootfs_off, layout->rootfs_size);
    }

    if (append_arg(out, outsz, "bootflag", bootflag ? "1" : "0") ||
        append_arg(out, outsz, "tclinux_info", tclinux))
        return -ENOSPC;

    return 0;
}

static int do_ursusstockboot(struct cmd_tbl *cmdtp, int flag, int argc,
                             char *const argv[])
{
    struct mtd_info *mtd;
    struct stock_slot master = { .name = "MASTER", .base = STOCK_MASTER_BASE };
    struct stock_slot slave = { .name = "SLAVE", .base = STOCK_SLAVE_BASE };
    struct stock_slot *chosen;
    struct stock_layout layout;
    struct stock_env se;
    void *fit = (void *)FIT_LOAD_ADDR;
    char bootargs[BOOTARGS_MAX];
    u32 bootargs_crc;
    int bootflag = 0;
    int mr, sr, ret;
    bool want_slave = argc > 1 && !strcmp(argv[1], "slave");

    printf("URSUS_STOCKBOOT_BEGIN\n");
    printf("URSUS_STOCKBOOT_PERSISTENT\n");
    printf("URSUS_STOCKBOOT_NAND_WRITES=0\n");
    printf("URSUS_STOCKBOOT_POLICY requested=%s\n", want_slave ? "SLAVE" : "MASTER");

    mtd = find_master_nand();
    if (!mtd) {
        printf("URSUS_STOCKBOOT_FAIL reason=MTD_NOT_FOUND\n");
        return CMD_RET_FAILURE;
    }
    printf("URSUS_STOCKBOOT_MTD name=%s size=0x%llx writesize=0x%x erasesize=0x%x\n",
           mtd->name, (unsigned long long)mtd->size, mtd->writesize, mtd->erasesize);

    mr = inspect_slot(mtd, &master);
    sr = inspect_slot(mtd, &slave);
    chosen = want_slave ? &slave : &master;
    ret = want_slave ? sr : mr;
    if (ret) {
        printf("URSUS_STOCKBOOT_FAIL reason=REQUESTED_SLOT_INVALID slot=%s ret=%d\n",
               chosen->name, ret);
        return CMD_RET_FAILURE;
    }
    if (chosen->bad_blocks) {
        printf("URSUS_STOCKBOOT_FAIL reason=REQUESTED_SLOT_HAS_BADBLOCKS slot=%s bad=%d\n",
               chosen->name, chosen->bad_blocks);
        return CMD_RET_FAILURE;
    }
    bootflag = want_slave ? 1 : 0;

    printf("URSUS_STOCKBOOT_CHOSEN slot=%s bootflag=%d\n", chosen->name, bootflag);
    printf("URSUS_STOCKBOOT_LOAD phys=0x%llx ram=0x%lx bytes=0x%x\n",
           (unsigned long long)chosen->fit_phys, FIT_LOAD_ADDR, chosen->fit_size);

    ret = read_range(mtd, chosen->fit_phys, chosen->fit_size, fit);
    if (ret) {
        printf("URSUS_STOCKBOOT_FAIL reason=FIT_READ ret=%d\n", ret);
        return CMD_RET_FAILURE;
    }
    flush_cache(FIT_LOAD_ADDR, chosen->fit_size);

    if (fdt_check_header(fit) || fdt_totalsize(fit) != chosen->fit_size) {
        printf("URSUS_STOCKBOOT_FAIL reason=FIT_RECHECK totalsize=0x%x expected=0x%x\n",
               fdt_check_header(fit) ? 0 : fdt_totalsize(fit), chosen->fit_size);
        return CMD_RET_FAILURE;
    }
    printf("URSUS_STOCKBOOT_FIT_LOADED size=0x%x\n", chosen->fit_size);

    if (!fit_all_image_verify(fit)) {
        printf("URSUS_STOCKBOOT_FAIL reason=FIT_HASH_VERIFY\n");
        return CMD_RET_FAILURE;
    }
    printf("URSUS_STOCKBOOT_FIT_HASHES_OK\n");

    memset(&layout, 0, sizeof(layout));
    ret = fit_image_info(fit, chosen->nt_off, &layout);
    if (ret) {
        printf("URSUS_STOCKBOOT_FAIL reason=FIT_PARSE ret=%d\n", ret);
        return CMD_RET_FAILURE;
    }
    printf("URSUS_STOCKBOOT_TCLINUX_LAYOUT fit_size=0x%x kernel_off=0x%llx kernel_size=0x%x rootfs_off=0x%llx rootfs_size=0x%x\n",
           chosen->fit_size, (unsigned long long)layout.kernel_off, layout.kernel_size,
           (unsigned long long)layout.rootfs_off, layout.rootfs_size);

    if (chosen->fit_size == ORACLE_FIT_SIZE &&
        layout.kernel_off == ORACLE_KERNEL_OFF && layout.kernel_size == ORACLE_KERNEL_SIZE &&
        layout.rootfs_off == ORACLE_ROOTFS_OFF && layout.rootfs_size == ORACLE_ROOTFS_SIZE)
        printf("URSUS_STOCKBOOT_BACKUP_ORACLE_MATCH\n");
    else
        printf("URSUS_STOCKBOOT_BACKUP_ORACLE_DIFFERENT\n");

    ret = stock_env_load(mtd, &se);
    if (ret) {
        printf("URSUS_STOCKBOOT_FAIL reason=STOCK_ENV ret=%d\n", ret);
        stock_env_free(&se);
        return CMD_RET_FAILURE;
    }

    ret = build_stock_bootargs(&se, &master, &slave, &layout, bootflag,
                               bootargs, sizeof(bootargs));
    stock_env_free(&se);
    if (ret) {
        printf("URSUS_STOCKBOOT_FAIL reason=BOOTARGS_BUILD ret=%d\n", ret);
        return CMD_RET_FAILURE;
    }

    env_set("bootargs", bootargs);
    env_set("verify", "yes");
    env_set_hex("loadaddr", FIT_LOAD_ADDR);
    bootargs_crc = crc32(0, (const u8 *)bootargs, strlen(bootargs));
    printf("URSUS_STOCKBOOT_BOOTARGS_READY len=%zu crc32=0x%08x root_mtd=%s sensitive_env=OMITTED\n",
           strlen(bootargs), bootargs_crc, bootflag ? "mtd5" : "mtd3");
    printf("URSUS_STOCKBOOT_TCBOOT_MD_ARGS wifi1=05 wifi2=05 usb2=02\n");
    printf("URSUS_STOCKBOOT_VALIDATED\n");
    printf("URSUS_STOCKBOOT_BOOTM addr=0x%lx config=conf@1\n", FIT_LOAD_ADDR);

    ret = run_commandf("bootm 0x%lx#conf@1", FIT_LOAD_ADDR);
    printf("URSUS_STOCKBOOT_BOOTM_RETURN ret=%d\n", ret);
    printf("URSUS_STOCKBOOT_FAIL reason=BOOTM_RETURNED\n");
    return CMD_RET_FAILURE;
}

U_BOOT_CMD(
    ursusstockboot, 2, 0, do_ursusstockboot,
    URSUS_PRODUCT_VERSION " StockBridge boot with Nokia tcboot board-argument parity",
    "[master|slave]"
);
