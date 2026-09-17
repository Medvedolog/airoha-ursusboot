// SPDX-License-Identifier: GPL-2.0+
#include <command.h>
#include <console.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <led.h>
#include <time.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <u-boot/schedule.h>
#include <ursus_led.h>
#include <ursus_version.h>

#define URSUS_LED_SHORT_MS 150UL
#define URSUS_LED_LONG_MS  450UL
#define URSUS_LED_GAP_MS    150UL
#define URSUS_LED_LETTER_MS 450UL
#define URSUS_LED_WORD_MS   1050UL

#define LED_STATUS_RED "status-red"
#define LED_USB1_GREEN "usb1-green"
#define LED_USB2_GREEN "usb2-green"

/* Nokia XG-040G-MD / AN7581 LAN2-LAN4 front-panel PHY LED0 routing.
 * OpenWrt Linux uses the same mapping and marks these LEDs active-low:
 *   gpio44=phy2_led0, gpio45=phy3_led0, gpio46=phy4_led0.
 *
 * SCU pin mux alone is not sufficient: the internal MTK/Airoha GPHY LED0
 * controller must first be programmed through the switch MDIO Clause-45
 * window.  Only after all six LED registers read back exactly do we hand
 * GPIO44..46 to the PHY LED outputs. */
#define URSUS_AN7581_CHIP_SCU_BASE       0x1fa20000UL
#define URSUS_AN7581_GPIO_2ND_I2C_MODE   0x0214UL
#define URSUS_AN7581_LAN_LED0_MAPPING    0x027cUL
#define URSUS_LAN_LED_MODE_MASK          (BIT(5) | BIT(7) | BIT(9))
#define URSUS_LAN_LED_MAP_MASK           (GENMASK(6, 4) | GENMASK(10, 8) | GENMASK(14, 12))
#define URSUS_LAN_LED_MAP_VALUE          ((1U << 4) | (2U << 8) | (3U << 12))

#define URSUS_MT7531_SWITCH_BASE         0x1fb58000UL
#define URSUS_MT7531_PHY_IAC_OFF         0x0000701cUL
#define URSUS_MT7531_PHY_IAC             (URSUS_MT7531_SWITCH_BASE + URSUS_MT7531_PHY_IAC_OFF)
#define URSUS_MT7531_MDIO_BUSY           BIT(31)
#define URSUS_MT7531_MDIO_PHY(_p)        ((u32)(_p) << 20)
#define URSUS_MT7531_MDIO_DEV31          (31U << 25)
#define URSUS_MT7531_MDIO_CMD_WRITE      (1U << 18)
#define URSUS_MT7531_MDIO_CMD_READ_C45   (3U << 18)
#define URSUS_MT7531_LED0_ON_CTRL        0x24U
#define URSUS_MT7531_LED0_BLINK_CTRL     0x25U
#define URSUS_MT7531_LED0_ON_VALUE       0xc007U /* enable + active-low + link 10/100/1000 */
#define URSUS_MT7531_LED0_BLINK_VALUE    0x003fU /* TX/RX activity at 10/100/1000 */
#define URSUS_MT7531_MDIO_SPINS          0x00100000U

static u32 ursus_scu_read(ulong off)
{
    return readl((void __iomem *)(uintptr_t)(URSUS_AN7581_CHIP_SCU_BASE + off));
}

static void ursus_scu_write(ulong off, u32 val)
{
    writel(val, (void __iomem *)(uintptr_t)(URSUS_AN7581_CHIP_SCU_BASE + off));
}

static int ursus_lanphy_wait_idle(void)
{
    unsigned int n = URSUS_MT7531_MDIO_SPINS;

    while (n--) {
        if (!(readl((void __iomem *)(uintptr_t)URSUS_MT7531_PHY_IAC) &
              URSUS_MT7531_MDIO_BUSY))
            return 0;
    }
    return -1;
}

static int ursus_lanphy_c45_write(unsigned int phy, unsigned int reg, u16 val)
{
    u32 cmd;

    if (ursus_lanphy_wait_idle())
        return -1;
    cmd = URSUS_MT7531_MDIO_BUSY | URSUS_MT7531_MDIO_DEV31 |
          URSUS_MT7531_MDIO_PHY(phy) | reg;
    writel(cmd, (void __iomem *)(uintptr_t)URSUS_MT7531_PHY_IAC);
    __asm__ __volatile__("dmb sy" ::: "memory");
    if (ursus_lanphy_wait_idle())
        return -1;

    cmd = URSUS_MT7531_MDIO_BUSY | URSUS_MT7531_MDIO_DEV31 |
          URSUS_MT7531_MDIO_PHY(phy) | URSUS_MT7531_MDIO_CMD_WRITE | val;
    writel(cmd, (void __iomem *)(uintptr_t)URSUS_MT7531_PHY_IAC);
    __asm__ __volatile__("dmb sy" ::: "memory");
    return ursus_lanphy_wait_idle();
}

static int ursus_lanphy_c45_read(unsigned int phy, unsigned int reg)
{
    u32 cmd;

    if (ursus_lanphy_wait_idle())
        return -1;
    cmd = URSUS_MT7531_MDIO_BUSY | URSUS_MT7531_MDIO_DEV31 |
          URSUS_MT7531_MDIO_PHY(phy) | reg;
    writel(cmd, (void __iomem *)(uintptr_t)URSUS_MT7531_PHY_IAC);
    __asm__ __volatile__("dmb sy" ::: "memory");
    if (ursus_lanphy_wait_idle())
        return -1;

    cmd = URSUS_MT7531_MDIO_BUSY | URSUS_MT7531_MDIO_DEV31 |
          URSUS_MT7531_MDIO_PHY(phy) | URSUS_MT7531_MDIO_CMD_READ_C45;
    writel(cmd, (void __iomem *)(uintptr_t)URSUS_MT7531_PHY_IAC);
    __asm__ __volatile__("dmb sy" ::: "memory");
    if (ursus_lanphy_wait_idle())
        return -1;
    return readl((void __iomem *)(uintptr_t)URSUS_MT7531_PHY_IAC) & 0xffff;
}

void ursus_lan_led_enable(void)
{
    unsigned int phy;
    u32 mode, map;

    for (phy = 0x0a; phy <= 0x0c; phy++) {
        int on, blink, ret;

        ret = ursus_lanphy_c45_write(phy, URSUS_MT7531_LED0_ON_CTRL,
                                     URSUS_MT7531_LED0_ON_VALUE);
        if (ret) {
            printf("URSUS_LANPHY_FAIL phy=%x reg=%x val=%x\n",
                   phy, URSUS_MT7531_LED0_ON_CTRL, (unsigned int)ret);
            return;
        }
        ret = ursus_lanphy_c45_write(phy, URSUS_MT7531_LED0_BLINK_CTRL,
                                     URSUS_MT7531_LED0_BLINK_VALUE);
        if (ret) {
            printf("URSUS_LANPHY_FAIL phy=%x reg=%x val=%x\n",
                   phy, URSUS_MT7531_LED0_BLINK_CTRL, (unsigned int)ret);
            return;
        }
        on = ursus_lanphy_c45_read(phy, URSUS_MT7531_LED0_ON_CTRL);
        if (on != URSUS_MT7531_LED0_ON_VALUE) {
            printf("URSUS_LANPHY_FAIL phy=%x reg=%x val=%x\n",
                   phy, URSUS_MT7531_LED0_ON_CTRL, (unsigned int)on);
            return;
        }
        blink = ursus_lanphy_c45_read(phy, URSUS_MT7531_LED0_BLINK_CTRL);
        if (blink != URSUS_MT7531_LED0_BLINK_VALUE) {
            printf("URSUS_LANPHY_FAIL phy=%x reg=%x val=%x\n",
                   phy, URSUS_MT7531_LED0_BLINK_CTRL, (unsigned int)blink);
            return;
        }
    }
    printf("URSUS_LANPHY_OK\n");

    mode = ursus_scu_read(URSUS_AN7581_GPIO_2ND_I2C_MODE);
    map = ursus_scu_read(URSUS_AN7581_LAN_LED0_MAPPING);
    mode |= URSUS_LAN_LED_MODE_MASK;
    map = (map & ~URSUS_LAN_LED_MAP_MASK) | URSUS_LAN_LED_MAP_VALUE;
    ursus_scu_write(URSUS_AN7581_GPIO_2ND_I2C_MODE, mode);
    ursus_scu_write(URSUS_AN7581_LAN_LED0_MAPPING, map);

    mode = ursus_scu_read(URSUS_AN7581_GPIO_2ND_I2C_MODE);
    map = ursus_scu_read(URSUS_AN7581_LAN_LED0_MAPPING);
    printf("URSUS_LAN_LED_MAP lan2=gpio44/phy2_led0 lan3=gpio45/phy3_led0 "
           "lan4=gpio46/phy4_led0 mode=0x%08x map=0x%08x state=%s\n",
           mode, map,
           ((mode & URSUS_LAN_LED_MODE_MASK) == URSUS_LAN_LED_MODE_MASK &&
            (map & URSUS_LAN_LED_MAP_MASK) == URSUS_LAN_LED_MAP_VALUE) ?
           "ENABLED" : "VERIFY_FAILED");
}

struct ursus_led_step {
	bool on;
	unsigned short ms;
};

/* Two short, three long, then a visible separator and steady red. */
static const struct ursus_led_step recovery_steps[] = {
	{ true, 150 }, { false, 150 }, { true, 150 }, { false, 300 },
	{ true, 450 }, { false, 150 }, { true, 450 }, { false, 150 },
	{ true, 450 }, { false, 300 },
};

/* Morse SOS: ... --- ... ; repeats after a word gap. */
static const struct ursus_led_step sos_steps[] = {
	{ true, 150 }, { false, 150 }, { true, 150 }, { false, 150 },
	{ true, 150 }, { false, 450 },
	{ true, 450 }, { false, 150 }, { true, 450 }, { false, 150 },
	{ true, 450 }, { false, 450 },
	{ true, 150 }, { false, 150 }, { true, 150 }, { false, 150 },
	{ true, 150 }, { false, 1050 },
};

enum ursus_led_pattern {
	URSUS_LED_PATTERN_NONE,
	URSUS_LED_PATTERN_RECOVERY,
	URSUS_LED_PATTERN_SOS,
};

static enum ursus_led_pattern active_pattern;
static unsigned int active_step;
static ulong next_change_ms;
static struct udevice *status_red;
static struct udevice *usb1_green;
static struct udevice *usb2_green;
#if URSUS_LED_PRODUCTION_ENABLED
static bool led_lookup_done;

static void ursus_led_lookup(void)
{
	int r0, r1, r2;

	if (led_lookup_done)
		return;
	led_lookup_done = true;
	r0 = led_get_by_label(LED_STATUS_RED, &status_red);
	r1 = led_get_by_label(LED_USB1_GREEN, &usb1_green);
	r2 = led_get_by_label(LED_USB2_GREEN, &usb2_green);
	printf("URSUS_LED_MAP status-red=%s(ret=%d) usb1-green=%s(ret=%d) usb2-green=%s(ret=%d)\n",
	       r0 ? "UNAVAILABLE" : "READY", r0,
	       r1 ? "UNAVAILABLE" : "READY", r1,
	       r2 ? "UNAVAILABLE" : "READY", r2);
}
#endif

static void ursus_led_one(struct udevice *dev, bool on)
{
	if (dev)
		led_set_state(dev, on ? LEDST_ON : LEDST_OFF);
}

static void ursus_led_usb_pair(bool on)
{
	ursus_led_one(usb1_green, on);
	ursus_led_one(usb2_green, on);
}

#if URSUS_LED_PRODUCTION_ENABLED
static void ursus_led_start(enum ursus_led_pattern pattern)
{
	ursus_led_lookup();
	active_pattern = pattern;
	active_step = 0;
	next_change_ms = 0;
}
#endif

void ursus_led_recovery_latched(void)
{
#if URSUS_LED_PRODUCTION_ENABLED
	printf("URSUS_LED_STATE=RECOVERY_LATCHED\n");
	ursus_led_start(URSUS_LED_PATTERN_RECOVERY);
	printf("URSUS_LED_RECOVERY_PATTERN_SYNC=1\n");
	while (active_pattern != URSUS_LED_PATTERN_NONE) {
		ursus_led_poll();
		if (active_pattern != URSUS_LED_PATTERN_NONE) {
			schedule();
			udelay(1000);
		}
	}
#else
	printf("URSUS_LED_PRODUCTION_GATED=1 state=RECOVERY_LATCHED version=%s\n",
	       URSUS_VERSION);
#endif
}

void ursus_led_poll(void)
{
	const struct ursus_led_step *steps;
	size_t count;
	ulong now;

	if (active_pattern == URSUS_LED_PATTERN_NONE)
		return;
	now = get_timer(0);
	if (next_change_ms && (long)(now - next_change_ms) < 0)
		return;

	if (active_pattern == URSUS_LED_PATTERN_RECOVERY) {
		steps = recovery_steps;
		count = ARRAY_SIZE(recovery_steps);
		if (active_step >= count) {
			ursus_led_one(status_red, true);
			active_pattern = URSUS_LED_PATTERN_NONE;
			if (status_red)
				printf("URSUS_LED_RECOVERY_PATTERN_DONE steady=red\n");
			else
				printf("URSUS_LED_RECOVERY_PATTERN_DONE steady=unavailable\n");
			return;
		}
		ursus_led_one(status_red, steps[active_step].on);
	} else {
		steps = sos_steps;
		count = ARRAY_SIZE(sos_steps);
		if (active_step >= count)
			active_step = 0;
		ursus_led_usb_pair(steps[active_step].on);
	}
	next_change_ms = now + steps[active_step].ms;
	active_step++;
}

void ursus_led_fatal_wait(const char *reason)
{
#if URSUS_LED_PRODUCTION_ENABLED
	printf("URSUS_FATAL_RECOVERY_WAIT reason=%s\n", reason ? reason : "UNKNOWN");
	printf("URSUS_FATAL_TRANSPORTS=tftp:UNSUPPORTED,usb:UNSUPPORTED\n");
	ursus_led_start(URSUS_LED_PATTERN_SOS);
	while (!ctrlc()) {
		ursus_led_poll();
		schedule();
		udelay(1000);
	}
	printf("URSUS_FATAL_RECOVERY_UART_ABORT\n");
#else
	printf("URSUS_FATAL_RECOVERY_LED_GATED=1 reason=%s version=%s\n",
	       reason ? reason : "UNKNOWN", URSUS_VERSION);
#endif
}

static int do_ursuslanled(struct cmd_tbl *cmdtp, int flag, int argc,
                           char *const argv[])
{
    u32 mode = ursus_scu_read(URSUS_AN7581_GPIO_2ND_I2C_MODE);
    u32 map = ursus_scu_read(URSUS_AN7581_LAN_LED0_MAPPING);

    if (argc != 2)
        return CMD_RET_USAGE;
    if (!strcmp(argv[1], "enable")) {
        ursus_lan_led_enable();
        return CMD_RET_SUCCESS;
    }
    if (!strcmp(argv[1], "status")) {
        printf("URSUS_LAN_LED_STATUS mode=0x%08x map=0x%08x "
               "mode_mask=0x%08x map_mask=0x%08x expected_map=0x%08x\n",
               mode, map, (unsigned int)URSUS_LAN_LED_MODE_MASK,
               (unsigned int)URSUS_LAN_LED_MAP_MASK,
               (unsigned int)URSUS_LAN_LED_MAP_VALUE);
        return CMD_RET_SUCCESS;
    }
    return CMD_RET_USAGE;
}

U_BOOT_CMD(ursuslanled, 2, 0, do_ursuslanled,
           "Nokia LAN2-LAN4 hardware PHY LED routing",
           "<status|enable>");

