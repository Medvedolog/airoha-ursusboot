// SPDX-License-Identifier: GPL-2.0+
#include <button.h>
#include <command.h>
#include <env.h>
#include <mapmem.h>
#include <mtd.h>
#include <time.h>
#include <linux/delay.h>
#include <linux/libfdt.h>
#include <linux/mtd/mtd.h>
#include <ursus_led.h>
#include <ursus_ubi.h>
#include <ursus_dispatch.h>
#include <ursus_version.h>

#define URSUS_RESET_HOLD_MS 5000UL
#define URSUS_RESET_BOOT_DEBOUNCE_MS 750UL
#define URSUS_RESET_POLL_US 20000UL
#define URSUS_FACTORY_KERNEL_OFF  0x000c0000ULL
#define URSUS_FACTORY_KERNEL_SIZE 0x00800000ULL
#define URSUS_FACTORY_LOAD_ADDR   0x90000000UL

static bool ursus_recovery_latched;
static char ursus_boot_reason[32] = "POWER_ON";
static char ursus_recovery_reason[32] = "NONE";

const char *ursus_dispatch_boot_reason(void)
{
    return ursus_boot_reason;
}

const char *ursus_dispatch_recovery_reason(void)
{
    return ursus_recovery_reason;
}

static void ursus_dispatch_set_reason(char *dst, size_t dst_len, const char *reason)
{
    snprintf(dst, dst_len, "%s", reason ? reason : "UNKNOWN");
}

static struct mtd_info *ursus_master_nand(void)
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

static bool ursus_ubi_present(struct mtd_info *nand)
{
    u8 hdr[4];
    size_t retlen = 0;
    int ret;

    if (!nand)
        return false;
    ret = mtd_read(nand, 0x00020000ULL, sizeof(hdr), &retlen, hdr);
    return (ret == 0 || ret == -EUCLEAN) && retlen == sizeof(hdr) &&
           !memcmp(hdr, "UBI#", 4);
}

static bool ursus_factory_kernel_present(struct mtd_info *nand)
{
    u8 hdr[4];
    size_t retlen = 0;
    int ret;

    if (!nand)
        return false;
    ret = mtd_read(nand, URSUS_FACTORY_KERNEL_OFF, sizeof(hdr), &retlen, hdr);
    return (ret == 0 || ret == -EUCLEAN) && retlen == sizeof(hdr) &&
           hdr[0] == 0xd0 && hdr[1] == 0x0d && hdr[2] == 0xfe && hdr[3] == 0xed;
}

static int ursus_enter_webfailsafe(const char *reason, ulong elapsed_ms)
{
    int ret;

    ursus_recovery_latched = true;
    ursus_dispatch_set_reason(ursus_recovery_reason, sizeof(ursus_recovery_reason), reason);
    ursus_led_recovery_latched();
    printf("URSUS_RECOVERY_LATCHED\nURSUS_RECOVERY_REASON=%s\n", reason);
    printf("URSUS_RECOVERY_HOLD_MS=%lu\nURSUS_RECOVERY_STICKY=1\n", elapsed_ms);
    ret = run_command("ursusweb", 0);
    printf("URSUS_WEB_RETURNED ret=%d\nURSUS_RECOVERY_STILL_LATCHED=1\n", ret);
    if (!ret) {
        printf("URSUS_RECOVERY_UART_SHELL\n");
        return CMD_RET_SUCCESS;
    }
#if URSUS_LED_PRODUCTION_ENABLED
    ursus_led_fatal_wait("WEBFAILSAFE_RETURN");
#endif
    return CMD_RET_FAILURE;
}

static int ursus_normal_boot(void)
{
    struct mtd_info *nand = ursus_master_nand();
    int ret;

    ursus_dispatch_set_reason(ursus_boot_reason, sizeof(ursus_boot_reason), "NORMAL_BOOT");
    printf("URSUS_NORMAL_BOOT_SELECTED\n");
    if (ursus_ubi_present(nand)) {
        printf("URSUS_UBI_BOOT_SELECTED\n");
        ret = ursus_ubi_boot();
        printf("URSUS_UBI_BOOT_RETURNED ret=%d\n", ret);
        return ursus_enter_webfailsafe(ursus_ubi_last_boot_reason(), 0);
    }
    if (ursus_factory_kernel_present(nand)) {
        printf("URSUS_FACTORY_BOOT_SELECTED\n");
        ret = run_commandf("mtd read %s 0x%08lx 0x%llx 0x%llx",
                           nand->name, URSUS_FACTORY_LOAD_ADDR,
                           (unsigned long long)URSUS_FACTORY_KERNEL_OFF,
                           (unsigned long long)URSUS_FACTORY_KERNEL_SIZE);
        if (!ret) {
            printf("URSUS_FACTORY_BOOTM addr=0x%08lx\n", URSUS_FACTORY_LOAD_ADDR);
            ret = run_commandf("bootm 0x%08lx", URSUS_FACTORY_LOAD_ADDR);
        }
        printf("URSUS_FACTORY_BOOT_RETURNED ret=%d\n", ret);
        return ursus_enter_webfailsafe("FACTORY_BOOT_RETURN", 0);
    }

    printf("URSUS_STOCK_BOOT_SELECTED\n");
    ret = run_command("ursusstockboot", 0);
    printf(ret ? "URSUS_STOCKBOOT_RETURNED ret=%d\n" :
                 "URSUS_STOCKBOOT_UNEXPECTED_RETURN ret=%d\n", ret);
    return ursus_enter_webfailsafe("STOCKBOOT_RETURN", 0);
}

static int do_ursusdispatch(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
    struct udevice *button;
    enum button_state_t state;
    ulong start, elapsed;
    int ret;

    printf("URSUS_DISPATCH_BEGIN\n");
    printf("URSUS_RESET_POLICY=HELD_AT_BOOT debounce_ms=%lu legacy_hold_ms=%lu\n",
           URSUS_RESET_BOOT_DEBOUNCE_MS, URSUS_RESET_HOLD_MS);
    if (ursus_recovery_latched)
        return ursus_enter_webfailsafe("STICKY_REENTRY", URSUS_RESET_BOOT_DEBOUNCE_MS);
    ret = button_get_by_label("reset", &button);
    if (ret) {
        printf("URSUS_RESET_DM_UNAVAILABLE ret=%d\nURSUS_RESET_FAILSAFE_POLICY=NORMAL_BOOT\n", ret);
        return ursus_normal_boot();
    }
    state = button_get_state(button);
    if ((int)state < 0) {
        printf("URSUS_RESET_READ_ERROR ret=%d\n", (int)state);
        return ursus_normal_boot();
    }
    if (state != BUTTON_ON) {
        printf("URSUS_RESET_INITIAL=RELEASED\n");
        return ursus_normal_boot();
    }
    printf("URSUS_RESET_INITIAL=ASSERTED\n");
    printf("URSUS_RESET_BOOT_DEBOUNCE_BEGIN ms=%lu\n", URSUS_RESET_BOOT_DEBOUNCE_MS);
    start = get_timer(0);
    for (;;) {
        state = button_get_state(button);
        elapsed = get_timer(start);
        if ((int)state < 0) {
            printf("URSUS_RESET_READ_ERROR ret=%d elapsed_ms=%lu\n", (int)state, elapsed);
            return ursus_normal_boot();
        }
        if (state != BUTTON_ON) {
            printf("URSUS_RESET_RELEASED_DURING_BOOT_DEBOUNCE elapsed_ms=%lu\n", elapsed);
            return ursus_normal_boot();
        }
        if (elapsed >= URSUS_RESET_BOOT_DEBOUNCE_MS) {
            printf("URSUS_RESET_BOOT_DEBOUNCE_OK elapsed_ms=%lu\n", elapsed);
            ursus_dispatch_set_reason(ursus_boot_reason, sizeof(ursus_boot_reason), "RESET_BUTTON");
            return ursus_enter_webfailsafe("RESET_HELD_AT_BOOT", elapsed);
        }
        udelay(URSUS_RESET_POLL_US);
    }
}

U_BOOT_CMD(ursusdispatch, 1, 0, do_ursusdispatch,
           URSUS_PRODUCT_VERSION " boot-held Reset / stock-layout + UBI dispatcher", "");

/*
 * t72: environment ownership.  The UBI env can be written by another boot
 * loader (Vanilla OpenWrt U-Boot, e.g. after UrsusBoot was restored over
 * Vanilla via UART): its bootcmd replaced ursusdispatch, so boot-held Reset,
 * the red LED and WebFailsafe never ran.  Called from main_loop() before
 * preboot/bootcmd (a direct hook: CONFIG_EVENT is not enabled on every
 * board).  Only builds whose default env carries ursus_env_rev take part.
 *
 * - ursus_env_rev == default: nothing to do.
 * - no ursus_env_rev and bootcmd is not ursusdispatch: FOREIGN, full reset.
 * - UrsusBoot env of another revision (or pre-t72 without the variable but
 *   with ursusdispatch): MIGRATE, reset keeping the whitelist.
 * The reset is in RAM only.  Saving here would let an UrsusBoot loaded into
 * RAM over UART overwrite Vanilla's own environment (the t64 problem, the
 * other way round); UrsusBoot's own writes (migration, updates) persist it.
 * Boot is never blocked.
 */
static const char *const ursus_env_keep[] = {
    "rootfs_data_max",  /* written by the STOCK->UBI migration */
    "ethaddr",          /* persisted fallback MAC on the stock layout */
};

void ursus_env_guard_hook(void)
{
    char def_rev[16], keep[ARRAY_SIZE(ursus_env_keep)][64];
    const char *cur_rev = env_get("ursus_env_rev");
    const char *cur_boot = env_get("bootcmd");
    bool own, button_on = false;
    struct udevice *button;
    unsigned int i;

    if (env_get_default_into("ursus_env_rev", def_rev, sizeof(def_rev)) <= 0)
        return;
    if (!cur_rev || strcmp(cur_rev, def_rev)) {
        own = cur_rev || (cur_boot && strstr(cur_boot, "ursusdispatch"));
        for (i = 0; i < ARRAY_SIZE(ursus_env_keep); i++) {
            const char *v = own ? env_get(ursus_env_keep[i]) : NULL;

            snprintf(keep[i], sizeof(keep[i]), "%s", v ? v : "");
        }
        printf("URSUS_ENV_%s rev=%s want=%s bootcmd=\"%s\" action=RESET_TO_URSUSBOOT_DEFAULT\n",
               own ? "MIGRATE" : "FOREIGN", cur_rev ? cur_rev : "<none>", def_rev,
               cur_boot ? cur_boot : "<none>");
        env_set_default(NULL, 0);
        for (i = 0; i < ARRAY_SIZE(ursus_env_keep); i++) {
            if (keep[i][0] && !env_set(ursus_env_keep[i], keep[i]))
                printf("URSUS_ENV_KEEP %s=%s\n", ursus_env_keep[i], keep[i]);
        }
        printf("URSUS_ENV_RESET scope=RAM flash_env=UNCHANGED\n");
        cur_boot = env_get("bootcmd");
    }

    /* Recovery must not depend on the environment: a hand-edited bootcmd
     * without ursusdispatch still gets boot-held Reset -> WebFailsafe. */
    if (cur_boot && strstr(cur_boot, "ursusdispatch"))
        return;
    if (!button_get_by_label("reset", &button))
        button_on = button_get_state(button) == BUTTON_ON;
    if (button_on) {
        printf("URSUS_ENV_BOOTCMD_BYPASS reason=reset-held bootcmd=\"%s\"\n",
               cur_boot ? cur_boot : "<none>");
        run_command("ursusdispatch", 0);
    }
}
