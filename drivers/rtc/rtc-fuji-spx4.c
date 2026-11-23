// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPX4-MF Medium-Format Camera Hardware RTC & 24-Bit Wake-Alarm Driver (over Power-Peer SPI1)
 *   - Epoch: 2000-01-01T00:00:00Z (Unix 946684800)
 *   - Read time:  sel 0x23 (low 16b), sel 0x24 (high 16b), sel 0x27 (bit 1 valid)
 *   - Write time: sel 0x1D (load low 16b), sel 0x1E (load high 16b), sel 0x27 |= 0x0202
 *   - Wake alarm (DAT @ section4.bin 0x0124C610..688, 0x012CA7E4..968):
 *       sel 0x12 (bit 8 = wake select, bit 0 = arm countdown)
 *       sel 0x25 (countdown low 16b seconds), sel 0x26 (countdown high 8b seconds)
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>

#include "fuji_spx4_hw.h"

extern int fuji_spx4_power_read_reg(u8 reg, u16 *val);
extern int fuji_spx4_power_write_reg(u8 reg, u16 val);

#define GFX_RTC_EPOCH_UNIX	946684800ULL

static int spx4_rtc_read_secs(u64 *out_unix)
{
	u16 lo = 0, hi = 0;
	int ret;

	ret = fuji_spx4_power_read_reg(SPX4_PWR_SEL_RTC_SEC_LO, &lo);
	if (ret)
		return ret;
	ret = fuji_spx4_power_read_reg(SPX4_PWR_SEL_RTC_SEC_HI, &hi);
	if (ret)
		return ret;

	*out_unix = GFX_RTC_EPOCH_UNIX + (((u64)hi << 16) | lo);
	return 0;
}

static int spx4_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	u64 secs = 0;
	int ret;

	ret = spx4_rtc_read_secs(&secs);
	if (ret)
		return ret;

	rtc_time64_to_tm((time64_t)secs, tm);
	return 0;
}

static int spx4_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	time64_t unix_sec = rtc_tm_to_time64(tm);
	u32 gfx_sec;
	u16 ctrl = 0;
	int ret;

	if (unix_sec < (time64_t)GFX_RTC_EPOCH_UNIX)
		return -EINVAL;

	gfx_sec = (u32)(unix_sec - GFX_RTC_EPOCH_UNIX);
	ret = fuji_spx4_power_write_reg(SPX4_PWR_SEL_RTC_LOAD_LO, (u16)(gfx_sec & 0xFFFFU));
	if (ret)
		return ret;
	ret = fuji_spx4_power_write_reg(SPX4_PWR_SEL_RTC_LOAD_HI, (u16)(gfx_sec >> 16));
	if (ret)
		return ret;

	/* Preserve existing sel 0x27 bits while setting time-valid (b1) + load strobe (b9) */
	if (fuji_spx4_power_read_reg(SPX4_PWR_SEL_RTC_CTRL, &ctrl))
		ctrl = 0;
	ctrl |= SPX4_PWR_RTC_COMMIT_BITS;
	return fuji_spx4_power_write_reg(SPX4_PWR_SEL_RTC_CTRL, ctrl);
}

static int spx4_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	u16 ctrl = 0, lo = 0, hi = 0;
	u64 now_secs = 0;
	u32 delta;
	int ret;

	ret = fuji_spx4_power_read_reg(SPX4_PWR_SEL_WAKE_CTRL, &ctrl);
	if (ret)
		return ret;
	ret = fuji_spx4_power_read_reg(SPX4_PWR_SEL_WAKE_CNT_LO, &lo);
	if (ret)
		return ret;
	ret = fuji_spx4_power_read_reg(SPX4_PWR_SEL_WAKE_CNT_HI, &hi);
	if (ret)
		return ret;
	ret = spx4_rtc_read_secs(&now_secs);
	if (ret)
		return ret;

	delta = spx4_power_unpack_wake_countdown(lo, hi);
	alrm->enabled = ((ctrl & SPX4_PWR_WAKE_CTRL_ARMED) == SPX4_PWR_WAKE_CTRL_ARMED);
	alrm->pending = 0;
	rtc_time64_to_tm((time64_t)(now_secs + delta), &alrm->time);
	return 0;
}

static int spx4_rtc_alarm_irq_enable(struct device *dev, unsigned int enabled)
{
	u16 ctrl = 0;
	int ret;

	ret = fuji_spx4_power_read_reg(SPX4_PWR_SEL_WAKE_CTRL, &ctrl);
	if (ret)
		ctrl = 0;

	if (enabled)
		ctrl |= SPX4_PWR_WAKE_CTRL_ARMED;
	else
		ctrl &= ~SPX4_PWR_WAKE_CTRL_ARM;

	return fuji_spx4_power_write_reg(SPX4_PWR_SEL_WAKE_CTRL, ctrl);
}

static int spx4_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alrm)
{
	time64_t target_sec = rtc_tm_to_time64(&alrm->time);
	u64 now_secs = 0;
	u32 delta = 0;
	u16 ctrl = 0, lo = 0, hi = 0;
	int ret;

	ret = spx4_rtc_read_secs(&now_secs);
	if (ret)
		return ret;

	if (alrm->enabled) {
		if (target_sec <= (time64_t)now_secs)
			delta = 1;
		else if ((u64)(target_sec - (time64_t)now_secs) > SPX4_PWR_WAKE_MAX_SECS)
			delta = SPX4_PWR_WAKE_MAX_SECS;
		else
			delta = (u32)(target_sec - (time64_t)now_secs);
	}

	spx4_power_pack_wake_countdown(delta, &lo, &hi);

	/* DAT arming sequence (0x0124C614..688): disarm bit0 + set bit8, write 0x25/0x26, arm bit0 */
	if (fuji_spx4_power_read_reg(SPX4_PWR_SEL_WAKE_CTRL, &ctrl))
		ctrl = 0;
	ctrl = (ctrl & ~SPX4_PWR_WAKE_CTRL_ARM) | SPX4_PWR_WAKE_CTRL_SEL;
	ret = fuji_spx4_power_write_reg(SPX4_PWR_SEL_WAKE_CTRL, ctrl);
	if (ret)
		return ret;

	ret = fuji_spx4_power_write_reg(SPX4_PWR_SEL_WAKE_CNT_LO, lo);
	if (ret)
		return ret;
	ret = fuji_spx4_power_write_reg(SPX4_PWR_SEL_WAKE_CNT_HI, hi);
	if (ret)
		return ret;

	if (alrm->enabled) {
		ctrl |= SPX4_PWR_WAKE_CTRL_ARM;
		ret = fuji_spx4_power_write_reg(SPX4_PWR_SEL_WAKE_CTRL, ctrl);
	}
	return ret;
}

static const struct rtc_class_ops spx4_rtc_ops = {
	.read_time		= spx4_rtc_read_time,
	.set_time		= spx4_rtc_set_time,
	.read_alarm		= spx4_rtc_read_alarm,
	.set_alarm		= spx4_rtc_set_alarm,
	.alarm_irq_enable	= spx4_rtc_alarm_irq_enable,
};

static int fuji_spx4_rtc_probe(struct platform_device *pdev)
{
	struct rtc_device *rtc;

	rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc))
		return PTR_ERR(rtc);

	rtc->ops = &spx4_rtc_ops;
	rtc->range_min = GFX_RTC_EPOCH_UNIX;
	rtc->range_max = GFX_RTC_EPOCH_UNIX + U32_MAX;

	return devm_rtc_register_device(rtc);
}

static const struct of_device_id fuji_spx4_rtc_of_match[] = {
	{ .compatible = "fujisan,spx4-mf-rtc" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_rtc_of_match);

static struct platform_driver fuji_spx4_rtc_driver = {
	.probe = fuji_spx4_rtc_probe,
	.driver = {
		.name = "rtc-fuji-spx4",
		.of_match_table = fuji_spx4_rtc_of_match,
	},
};
module_platform_driver(fuji_spx4_rtc_driver);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera Hardware RTC & 24-Bit Wake Alarm Driver");
MODULE_LICENSE("GPL");
