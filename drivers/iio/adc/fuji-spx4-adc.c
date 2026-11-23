// SPDX-License-Identifier: GPL-2.0-only
/*
 * 12-Channel 12-Bit SAR ADC Driver for Fujisan SPX4 (SPX4-MF soc-adc @ 0xFFF40000)
 *
 * Wave-3 Hardware Verification (DAT @ section4.bin 0x011F5D48..0x011F5FFC):
 *   - Strictly bound to 0xFFF40000..0xFFF40047 (distinct from soc-keyed-io @ 0xFFF42000!)
 *   - Interleaved two-phase hardware scanner (0x011F5D48):
 *       Even phase mask 0x555: CH0, 2, 4, 6, 8, 10
 *       Odd  phase mask 0xAAA: CH1, 3, 5, 7, 9, 11
 *   - Register layout:
 *       +0x00  CTRL    bits[27:16] = 12-ch scan mask, bit 1 = continuous, bit 0 = start
 *       +0x04  DIV     sample clock divider (< 0x100000)
 *       +0x08  GATE    clock/power gate (0/1)
 *       +0x0C  STATUS  bit 0 = conversion done (W1C)
 *       +0x10..+0x3C   DATA[0..11] 12-bit unsigned samples (0..4095)
 *       +0x40  TRIM    8-bit calibration trim (0..255)
 *       (+0x44 AUX is never written so minimal emulator configs remain safe.)
 */

#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "fuji_spx4_hw.h"

struct fuji_spx4_adc {
	void __iomem *base;
	struct mutex lock;
	u16 cache[SPX4_ADC_CH_COUNT];
};

static struct fuji_spx4_adc *g_spx4_adc;

/* Run a single phase (even 0x555 or odd 0xAAA) and latch matching channels */
static void spx4_adc_scan_phase_locked(struct fuji_spx4_adc *adc, u32 ch_mask)
{
	u32 ctrl = ((ch_mask & SPX4_ADC_SCAN_MASK_ALL) << SPX4_ADC_CTRL_MASK_SHIFT) |
		   SPX4_ADC_CTRL_START_BIT;
	int tries = 200;
	unsigned int ch;

	writel_relaxed(1U, adc->base + SPX4_ADC_REG_STATUS);
	writel_relaxed(ctrl, adc->base + SPX4_ADC_REG_CTRL);

	while (!(readl_relaxed(adc->base + SPX4_ADC_REG_STATUS) & 1U) && --tries > 0)
		udelay(5);

	for (ch = 0; ch < SPX4_ADC_CH_COUNT; ch++) {
		if (ch_mask & BIT(ch))
			adc->cache[ch] = readl_relaxed(adc->base + SPX4_ADC_REG_DATA(ch)) & 0xFFFU;
	}

	writel_relaxed(1U, adc->base + SPX4_ADC_REG_STATUS);
}

int fuji_spx4_adc_read_channel(unsigned int ch, int *val)
{
	struct fuji_spx4_adc *adc = g_spx4_adc;
	u32 phase_mask;

	if (!adc || ch >= SPX4_ADC_CH_COUNT || !val)
		return -ENODEV;

	/* Sample the hardware phase (0x555 even or 0xAAA odd) owning this channel */
	phase_mask = (ch & 1U) ? SPX4_ADC_SCAN_MASK_ODD : SPX4_ADC_SCAN_MASK_EVEN;

	mutex_lock(&adc->lock);
	spx4_adc_scan_phase_locked(adc, phase_mask);
	*val = adc->cache[ch];
	mutex_unlock(&adc->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(fuji_spx4_adc_read_channel);

int fuji_spx4_adc_read_all(u16 samples[SPX4_ADC_CH_COUNT])
{
	struct fuji_spx4_adc *adc = g_spx4_adc;
	unsigned int i;

	if (!adc || !samples)
		return -ENODEV;

	mutex_lock(&adc->lock);
	/* Exact DAT interleaved scan: 0x555 (CH0,2,4,6,8,10) then 0xAAA (CH1,3,5,7,9,11) */
	spx4_adc_scan_phase_locked(adc, SPX4_ADC_SCAN_MASK_EVEN);
	spx4_adc_scan_phase_locked(adc, SPX4_ADC_SCAN_MASK_ODD);
	for (i = 0; i < SPX4_ADC_CH_COUNT; i++)
		samples[i] = adc->cache[i];
	mutex_unlock(&adc->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(fuji_spx4_adc_read_all);

static int spx4_adc_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	if (fuji_spx4_adc_read_channel(chan->channel, val))
		return -EIO;
	return IIO_VAL_INT;
}

static const struct iio_info spx4_adc_info = {
	.read_raw = spx4_adc_read_raw,
};

#define SPX4_ADC_CHAN(n) {				\
	.type = IIO_VOLTAGE,				\
	.indexed = 1,					\
	.channel = (n),					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),	\
}

static const struct iio_chan_spec spx4_adc_channels[SPX4_ADC_CH_COUNT] = {
	SPX4_ADC_CHAN(0),  SPX4_ADC_CHAN(1),  SPX4_ADC_CHAN(2),  SPX4_ADC_CHAN(3),
	SPX4_ADC_CHAN(4),  SPX4_ADC_CHAN(5),  SPX4_ADC_CHAN(6),  SPX4_ADC_CHAN(7),
	SPX4_ADC_CHAN(8),  SPX4_ADC_CHAN(9),  SPX4_ADC_CHAN(10), SPX4_ADC_CHAN(11),
};

static int fuji_spx4_adc_probe(struct platform_device *pdev)
{
	struct iio_dev *indio_dev;
	struct fuji_spx4_adc *adc;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	adc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adc->base))
		return PTR_ERR(adc->base);

	mutex_init(&adc->lock);

	/* Enable ADC clock gate and conservative divider */
	writel_relaxed(0U, adc->base + SPX4_ADC_REG_CTRL);
	writel_relaxed(8U, adc->base + SPX4_ADC_REG_DIV);
	writel_relaxed(1U, adc->base + SPX4_ADC_REG_GATE);
	writel_relaxed(1U, adc->base + SPX4_ADC_REG_STATUS);

	indio_dev->name = "fuji-spx4-adc";
	indio_dev->info = &spx4_adc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = spx4_adc_channels;
	indio_dev->num_channels = SPX4_ADC_CH_COUNT;

	g_spx4_adc = adc;
	platform_set_drvdata(pdev, indio_dev);

	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret) {
		g_spx4_adc = NULL;
		return ret;
	}

	dev_info(&pdev->dev,
		 "Fujisan SPX4 12-ch 12-bit SAR ADC registered (interleaved 0x555/0xAAA)\n");
	return 0;
}

static const struct of_device_id fuji_spx4_adc_of_match[] = {
	{ .compatible = "fujisan,spx4-adc" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_adc_of_match);

static struct platform_driver fuji_spx4_adc_driver = {
	.probe	= fuji_spx4_adc_probe,
	.driver	= {
		.name		= "fuji-spx4-adc",
		.of_match_table	= fuji_spx4_adc_of_match,
	},
};
module_platform_driver(fuji_spx4_adc_driver);

MODULE_DESCRIPTION("Fujisan SPX4 12-channel 12-bit interleaved SAR ADC driver");
MODULE_LICENSE("GPL");
