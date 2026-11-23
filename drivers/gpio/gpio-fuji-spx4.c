// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO Controller Driver for Fujisan SPX4 (SPX4-MF soc-gpio @ 0xFFF41000)
 *
 * Register layout (DAT verified via SPX4-MF 2.13 0x011DACD8 & GpioFactory.kt):
 *   16 banks (0..15) x 16 pins (0..15) = 256 GPIOs.
 *   Bank stride: 0x40 bytes; valid 32-bit aligned offsets in each bank: 0x00..0x1C.
 *     +0x00  DATA_OUT  (RW4) Output level bitmask [15:0]
 *     +0x08  DIR       (RW4) Direction bitmask (1 = output, 0 = input)
 *     +0x14  DATA_IN   (RO4) Sampled pin input level bitmask [15:0]
 *
 * Linear GPIO numbering: gpio = bank * 16 + pin (0..255).
 */

#include <linux/bitops.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define SPX4_GPIO_BANKS		16
#define SPX4_GPIO_PINS_PER_BANK	16
#define SPX4_GPIO_NGPIO		(SPX4_GPIO_BANKS * SPX4_GPIO_PINS_PER_BANK)
#define SPX4_GPIO_BANK_STRIDE	0x40

#define SPX4_GPIO_OUT_OFF	0x00
#define SPX4_GPIO_DIR_OFF	0x08
#define SPX4_GPIO_IN_OFF	0x14

struct fuji_spx4_gpio {
	struct gpio_chip gc;
	void __iomem *base;
	raw_spinlock_t lock;
};

static inline void __iomem *spx4_bank_reg(struct fuji_spx4_gpio *g,
					  unsigned int bank, unsigned int reg)
{
	return g->base + (bank * SPX4_GPIO_BANK_STRIDE) + reg;
}

static int spx4_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct fuji_spx4_gpio *g = gpiochip_get_data(gc);
	unsigned int bank = offset / SPX4_GPIO_PINS_PER_BANK;
	unsigned int pin = offset % SPX4_GPIO_PINS_PER_BANK;
	u32 dir = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_DIR_OFF));
	u32 val;

	if (dir & BIT(pin))
		val = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_OUT_OFF));
	else
		val = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_IN_OFF));

	return !!(val & BIT(pin));
}

static void spx4_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct fuji_spx4_gpio *g = gpiochip_get_data(gc);
	unsigned int bank = offset / SPX4_GPIO_PINS_PER_BANK;
	unsigned int pin = offset % SPX4_GPIO_PINS_PER_BANK;
	unsigned long flags;
	u32 reg;

	raw_spin_lock_irqsave(&g->lock, flags);
	reg = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_OUT_OFF));
	if (value)
		reg |= BIT(pin);
	else
		reg &= ~BIT(pin);
	writel_relaxed(reg, spx4_bank_reg(g, bank, SPX4_GPIO_OUT_OFF));
	raw_spin_unlock_irqrestore(&g->lock, flags);
}

static int spx4_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct fuji_spx4_gpio *g = gpiochip_get_data(gc);
	unsigned int bank = offset / SPX4_GPIO_PINS_PER_BANK;
	unsigned int pin = offset % SPX4_GPIO_PINS_PER_BANK;
	unsigned long flags;
	u32 dir;

	raw_spin_lock_irqsave(&g->lock, flags);
	dir = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_DIR_OFF));
	dir &= ~BIT(pin);
	writel_relaxed(dir, spx4_bank_reg(g, bank, SPX4_GPIO_DIR_OFF));
	raw_spin_unlock_irqrestore(&g->lock, flags);
	return 0;
}

static int spx4_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				      int value)
{
	struct fuji_spx4_gpio *g = gpiochip_get_data(gc);
	unsigned int bank = offset / SPX4_GPIO_PINS_PER_BANK;
	unsigned int pin = offset % SPX4_GPIO_PINS_PER_BANK;
	unsigned long flags;
	u32 dir, out;

	raw_spin_lock_irqsave(&g->lock, flags);
	out = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_OUT_OFF));
	if (value)
		out |= BIT(pin);
	else
		out &= ~BIT(pin);
	writel_relaxed(out, spx4_bank_reg(g, bank, SPX4_GPIO_OUT_OFF));

	dir = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_DIR_OFF));
	dir |= BIT(pin);
	writel_relaxed(dir, spx4_bank_reg(g, bank, SPX4_GPIO_DIR_OFF));
	raw_spin_unlock_irqrestore(&g->lock, flags);
	return 0;
}

static int spx4_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct fuji_spx4_gpio *g = gpiochip_get_data(gc);
	unsigned int bank = offset / SPX4_GPIO_PINS_PER_BANK;
	unsigned int pin = offset % SPX4_GPIO_PINS_PER_BANK;
	u32 dir = readl_relaxed(spx4_bank_reg(g, bank, SPX4_GPIO_DIR_OFF));

	return (dir & BIT(pin)) ? GPIO_LINE_DIRECTION_OUT : GPIO_LINE_DIRECTION_IN;
}

static int fuji_spx4_gpio_probe(struct platform_device *pdev)
{
	struct fuji_spx4_gpio *g;
	int ret;

	g = devm_kzalloc(&pdev->dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	g->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(g->base))
		return PTR_ERR(g->base);

	raw_spin_lock_init(&g->lock);

	g->gc.label = dev_name(&pdev->dev);
	g->gc.parent = &pdev->dev;
	g->gc.owner = THIS_MODULE;
	g->gc.base = -1;
	g->gc.ngpio = SPX4_GPIO_NGPIO;
	g->gc.get = spx4_gpio_get;
	g->gc.set = spx4_gpio_set;
	g->gc.direction_input = spx4_gpio_direction_input;
	g->gc.direction_output = spx4_gpio_direction_output;
	g->gc.get_direction = spx4_gpio_get_direction;
	g->gc.can_sleep = false;

	ret = devm_gpiochip_add_data(&pdev->dev, &g->gc, g);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Fujisan SPX4 GPIO registered (%u pins across %u banks)\n",
		 SPX4_GPIO_NGPIO, SPX4_GPIO_BANKS);
	return 0;
}

static const struct of_device_id fuji_spx4_gpio_of_match[] = {
	{ .compatible = "fujisan,spx4-gpio" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_gpio_of_match);

static struct platform_driver fuji_spx4_gpio_driver = {
	.probe	= fuji_spx4_gpio_probe,
	.driver	= {
		.name		= "fuji-spx4-gpio",
		.of_match_table	= fuji_spx4_gpio_of_match,
	},
};

static int __init fuji_spx4_gpio_init(void)
{
	return platform_driver_register(&fuji_spx4_gpio_driver);
}
postcore_initcall(fuji_spx4_gpio_init);

MODULE_DESCRIPTION("Fujisan SPX4 16-bank GPIO controller driver");
MODULE_LICENSE("GPL");
