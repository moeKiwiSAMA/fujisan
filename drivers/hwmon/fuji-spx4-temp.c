// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan SPX4 SoC Die Temperature Sensor (hwmon)
 *   - Converter window: 0xFFF47810..0xFFF47853 (13 aligned 32-bit registers)
 *   - Common control:   0xFFF4700C, gate: 0xFFF47028
 *
 * Register offsets within converter window (DAT / TemperatureBank.kt):
 *   +0x00 ENABLE (0/1), +0x04 START (0/1), +0x08 DIVIDER (0..3),
 *   +0x18 READY (RO4 bit 0), +0x20 RESULT (RO4 12-bit unsigned 0..4095)
 */

#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define TEMP_REG_ENABLE		0x00
#define TEMP_REG_START		0x04
#define TEMP_REG_DIVIDER	0x08
#define TEMP_REG_READY		0x20
#define TEMP_REG_RESULT		0x24

struct fuji_spx4_temp {
	void __iomem *base;
	struct mutex lock;
};

static int spx4_temp_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	struct fuji_spx4_temp *t = dev_get_drvdata(dev);
	u32 raw;
	int tries = 200;

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;

	mutex_lock(&t->lock);
	writel_relaxed(1, t->base + TEMP_REG_ENABLE);
	writel_relaxed(1, t->base + TEMP_REG_START);
	while (!(readl_relaxed(t->base + TEMP_REG_READY) & 1) && --tries > 0)
		udelay(10);
	raw = readl_relaxed(t->base + TEMP_REG_RESULT) & 0xFFF;
	writel_relaxed(0, t->base + TEMP_REG_START);
	writel_relaxed(0, t->base + TEMP_REG_ENABLE);
	mutex_unlock(&t->lock);

	/* Approximate linear millidegree-C conversion (uncalibrated raw12) */
	*val = (long)raw * 25L;
	return 0;
}

static umode_t spx4_temp_is_visible(const void *data, enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	if (type == hwmon_temp && attr == hwmon_temp_input)
		return 0444;
	return 0;
}

static const struct hwmon_channel_info *spx4_temp_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_ops spx4_temp_ops = {
	.is_visible	= spx4_temp_is_visible,
	.read		= spx4_temp_read,
};

static const struct hwmon_chip_info spx4_temp_chip_info = {
	.ops	= &spx4_temp_ops,
	.info	= spx4_temp_info,
};

static int fuji_spx4_temp_probe(struct platform_device *pdev)
{
	struct fuji_spx4_temp *t;
	struct device *hwmon_dev;

	t = devm_kzalloc(&pdev->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	t->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(t->base))
		return PTR_ERR(t->base);

	mutex_init(&t->lock);
	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev, "fuji_spx4_temp",
							 t, &spx4_temp_chip_info, NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct of_device_id fuji_spx4_temp_of_match[] = {
	{ .compatible = "fujisan,spx4-temperature" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_temp_of_match);

static struct platform_driver fuji_spx4_temp_driver = {
	.probe = fuji_spx4_temp_probe,
	.driver = {
		.name = "fuji-spx4-temp",
		.of_match_table = fuji_spx4_temp_of_match,
	},
};
module_platform_driver(fuji_spx4_temp_driver);

MODULE_DESCRIPTION("Fujisan SPX4 SoC Die Temperature hwmon Driver");
MODULE_LICENSE("GPL");
