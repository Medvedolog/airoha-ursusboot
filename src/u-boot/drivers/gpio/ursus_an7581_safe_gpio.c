// SPDX-License-Identifier: GPL-2.0+
/*
 * UrsusBoot safe GPIO bridge for Nokia XG-040G-MD / AN7581.
 *
 * The controller register layout follows the Airoha U-Boot GPIO driver.
 * Output writes are deliberately restricted to board-evidenced LED lines;
 * reset remains input-only. This is not a generic GPIO discovery driver.
 */
#include <dm.h>
#include <errno.h>
#include <asm/gpio.h>
#include <asm/io.h>
#include <linux/bitops.h>

#define URSUS_AN7581_GPIO_COUNT       50U
#define URSUS_AN7581_GPIO_RESET       0U
/* HW_PROVEN on Nokia XG-040G-MD: active-low front-panel indicators. */
#define URSUS_AN7581_GPIO_STATUS_RED 19U
#define URSUS_AN7581_GPIO_USB2       34U
#define URSUS_AN7581_GPIO_USB1       35U

#define REG_GPIO_CTRL0 0x0000
#define REG_GPIO_DATA0 0x0004
#define REG_GPIO_OE0   0x0014
#define REG_GPIO_CTRL1 0x0020
#define REG_GPIO_CTRL2 0x0060
#define REG_GPIO_CTRL3 0x0064
#define REG_GPIO_DATA1 0x0070
#define REG_GPIO_OE1   0x0078

struct ursus_gpio_priv {
	void __iomem *base;
};

static const u32 data_regs[] = { REG_GPIO_DATA0, REG_GPIO_DATA1 };
static const u32 oe_regs[] = { REG_GPIO_OE0, REG_GPIO_OE1 };
static const u32 dir_regs[] = {
	REG_GPIO_CTRL0, REG_GPIO_CTRL1, REG_GPIO_CTRL2, REG_GPIO_CTRL3
};

static bool ursus_gpio_valid(unsigned int gpio)
{
	return gpio < URSUS_AN7581_GPIO_COUNT;
}

static bool ursus_gpio_safe_output(unsigned int gpio)
{
	return gpio == URSUS_AN7581_GPIO_STATUS_RED ||
	       gpio == URSUS_AN7581_GPIO_USB1 ||
	       gpio == URSUS_AN7581_GPIO_USB2;
}

static int ursus_gpio_raw_get(struct ursus_gpio_priv *priv, unsigned int gpio)
{
	u32 bank, bit, val;

	if (!ursus_gpio_valid(gpio))
		return -EINVAL;
	bank = gpio / 32U;
	bit = gpio % 32U;
	val = readl(priv->base + data_regs[bank]);
	return !!(val & BIT(bit));
}

static int ursus_gpio_raw_set_any(struct ursus_gpio_priv *priv, unsigned int gpio,
				  int value)
{
	u32 bank, bit, val;

	if (!ursus_gpio_valid(gpio))
		return -EINVAL;
	bank = gpio / 32U;
	bit = gpio % 32U;
	val = readl(priv->base + data_regs[bank]);
	if (value)
		val |= BIT(bit);
	else
		val &= ~BIT(bit);
	writel(val, priv->base + data_regs[bank]);
	return 0;
}

static int ursus_gpio_raw_set(struct ursus_gpio_priv *priv, unsigned int gpio,
			      int value)
{
	if (!ursus_gpio_safe_output(gpio))
		return -EPERM;
	return ursus_gpio_raw_set_any(priv, gpio, value);
}

static int ursus_gpio_get_direction_raw(struct ursus_gpio_priv *priv,
					unsigned int gpio)
{
	u32 index, shift, field;

	if (!ursus_gpio_valid(gpio))
		return -EINVAL;
	index = gpio / 16U;
	shift = 2U * (gpio % 16U);
	field = (readl(priv->base + dir_regs[index]) >> shift) & 0x3U;
	if (field > 1U)
		return -EINVAL;
	return field ? GPIOF_OUTPUT : GPIOF_INPUT;
}

static int ursus_gpio_set_direction_raw(struct ursus_gpio_priv *priv,
					unsigned int gpio, bool input)
{
	u32 bank, bit, index, shift, mask, val;

	if (!ursus_gpio_valid(gpio))
		return -EINVAL;
	if (input) {
		if (gpio != URSUS_AN7581_GPIO_RESET)
			return -EPERM;
	} else if (!ursus_gpio_safe_output(gpio)) {
		return -EPERM;
	}

	bank = gpio / 32U;
	bit = gpio % 32U;
	val = readl(priv->base + oe_regs[bank]);
	if (input)
		val &= ~BIT(bit);
	else
		val |= BIT(bit);
	writel(val, priv->base + oe_regs[bank]);

	index = gpio / 16U;
	shift = 2U * (gpio % 16U);
	mask = 0x3U << shift;
	val = readl(priv->base + dir_regs[index]);
	val &= ~mask;
	if (!input)
		val |= 0x1U << shift;
	writel(val, priv->base + dir_regs[index]);
	return 0;
}

static int ursus_gpio_direction_input(struct udevice *dev, unsigned int offset)
{
	/* Preserve the already HW-proven reset path: do not rewrite its mux/OE. */
	if (offset == URSUS_AN7581_GPIO_RESET)
		return 0;
	return -EPERM;
}

static int ursus_gpio_direction_output(struct udevice *dev, unsigned int offset,
				       int value)
{
	struct ursus_gpio_priv *priv = dev_get_priv(dev);
	int ret;

	if (!ursus_gpio_safe_output(offset))
		return -EPERM;
	/* Program the inactive/initial level before enabling the output. */
	ret = ursus_gpio_raw_set(priv, offset, value);
	if (ret)
		return ret;
	return ursus_gpio_set_direction_raw(priv, offset, false);
}

static int ursus_gpio_get_value(struct udevice *dev, unsigned int offset)
{
	return ursus_gpio_raw_get(dev_get_priv(dev), offset);
}

static int ursus_gpio_set_value(struct udevice *dev, unsigned int offset,
				int value)
{
	return ursus_gpio_raw_set(dev_get_priv(dev), offset, value);
}

static int ursus_gpio_get_function(struct udevice *dev, unsigned int offset)
{
	if (offset == URSUS_AN7581_GPIO_RESET)
		return GPIOF_INPUT;
	return ursus_gpio_get_direction_raw(dev_get_priv(dev), offset);
}

static const struct dm_gpio_ops ursus_gpio_ops = {
	.direction_input = ursus_gpio_direction_input,
	.direction_output = ursus_gpio_direction_output,
	.get_value = ursus_gpio_get_value,
	.set_value = ursus_gpio_set_value,
	.get_function = ursus_gpio_get_function,
};

static int ursus_gpio_probe(struct udevice *dev)
{
	struct ursus_gpio_priv *priv = dev_get_priv(dev);
	struct gpio_dev_priv *uc_priv = dev_get_uclass_priv(dev);
	fdt_addr_t addr = dev_read_addr(dev);

	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;
	priv->base = (void __iomem *)(uintptr_t)addr;
	uc_priv->bank_name = "ursus-safe";
	uc_priv->gpio_count = URSUS_AN7581_GPIO_COUNT;
	return 0;
}

static const struct udevice_id ursus_gpio_ids[] = {
	{ .compatible = "ursus,an7581-safe-gpio" },
	{ }
};

U_BOOT_DRIVER(ursus_an7581_safe_gpio) = {
	.name = "ursus_an7581_safe_gpio",
	.id = UCLASS_GPIO,
	.of_match = ursus_gpio_ids,
	.ops = &ursus_gpio_ops,
	.probe = ursus_gpio_probe,
	.priv_auto = sizeof(struct ursus_gpio_priv),
};
