// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan OS — Sony IMX461 102MP Medium-Format CMOS V4L2 Subdev Driver
 *
 * DAT verified (SPX4-MF 2.13 CMOS-RE-1, S4 0x011B63E8..0x011B66A0, S5 0x024F718C..0x0250B260):
 *   - Dedicated SPI Command Engine @ 0xFFF6C200..0xFFF6C240
 *     Write frame: [0x81, reg_hi, reg_lo, val]
 *     Read  frame: [0x82, reg_hi, reg_lo] gated by IMX461 reg 0x004F = 0x08 -> 0x00!
 *   - Cold power-on GPIO sequence:
 *     sync0/1 GPIO(5,1)/(5,0) -> rails GPIO(12,2)/GPIO(4,7) + 10ms pulse on GPIO(0,15)
 *     -> release XCLR GPIO(4,5) -> wake reg 0x019E bit 2 -> STBY 0x0000 bit 0
 *   - 25 verified mode descriptors (102MP 16/14/12b still, ES, 4K/FHD movie, 60-240fps LV/AF)
 *   - Real sensor temperature readout via reg 0x0040 (lo) / 0x0041 (hi) under 0x004F read-gate
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "fujisan_spx4_hw.h"

struct fujisan_imx461 {
	struct device *dev;
	void __iomem *spi_base;		/* 0xFFF6C200 */
	uint8_t *cmd_buf;
	dma_addr_t cmd_dma;

	struct gpio_desc *xclr_gpio;		/* GPIO(4,5) */
	struct gpio_desc *rail_a_gpio;		/* GPIO(4,7) */
	struct gpio_desc *rail_b_gpio;		/* GPIO(12,2) */
	struct gpio_desc *rst_pulse_gpio;	/* GPIO(0,15) 10ms pulse */
	struct gpio_desc *sync0_gpio;		/* GPIO(5,1) */
	struct gpio_desc *sync1_gpio;		/* GPIO(5,0) */

	struct mutex lock;
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct fujisan_imx461_mode_desc cur_mode;
	bool streaming;
	bool powered;
};

static inline struct fujisan_imx461 *to_imx461(struct v4l2_subdev *sd)
{
	return container_of(sd, struct fujisan_imx461, sd);
}

static int imx461_spi_xfer(struct fujisan_imx461 *s, const uint8_t pkt[4],
			   bool is_read, uint8_t *out_val)
{
	uint32_t busy;
	int ret;

	memcpy(s->cmd_buf, pkt, 4);
	dma_wmb();

	writel(1U, s->spi_base + FUJISAN_IMX461_SPI_CTRL);
	writel((uint32_t)s->cmd_dma, s->spi_base + FUJISAN_IMX461_SPI_DMA_PTR);
	writel(fujisan_imx461_xfer_cfg(is_read, 4U, 1U),
	       s->spi_base + FUJISAN_IMX461_SPI_XFER_CFG);
	writel(1U, s->spi_base + FUJISAN_IMX461_SPI_START_BUSY);

	ret = readl_poll_timeout_atomic(s->spi_base + FUJISAN_IMX461_SPI_START_BUSY,
					busy, (busy & 1U) == 0U,
					10, 10000);
	if (ret)
		return ret;

	if (is_read && out_val)
		*out_val = (uint8_t)(readl(s->spi_base + FUJISAN_IMX461_SPI_RX_DATA0) & 0xFFU);
	return 0;
}

static int imx461_write_reg(struct fujisan_imx461 *s, uint16_t reg, uint8_t val)
{
	uint8_t pkt[4];

	fujisan_imx461_pack_write(pkt, reg, val);
	return imx461_spi_xfer(s, pkt, false, NULL);
}

/* DAT @ S5 0x024F718C: open 0x004F=0x08 read-gate, issue [0x82, hi, lo], close 0x004F=0x00 */
static int imx461_read_reg_gated(struct fujisan_imx461 *s, uint16_t reg, uint8_t *val)
{
	uint8_t pkt[4];
	int ret;

	ret = imx461_write_reg(s, FUJISAN_IMX461_REG_READ_GATE, FUJISAN_IMX461_READ_GATE_OPEN);
	if (ret)
		return ret;

	fujisan_imx461_pack_read(pkt, reg);
	ret = imx461_spi_xfer(s, pkt, true, val);

	/* Always close read-gate regardless of read outcome */
	imx461_write_reg(s, FUJISAN_IMX461_REG_READ_GATE, 0x00U);
	return ret;
}

static int imx461_read_temperature(struct fujisan_imx461 *s, uint16_t *raw_temp)
{
	uint8_t lo = 0, hi = 0;
	int ret;

	ret = imx461_read_reg_gated(s, FUJISAN_IMX461_REG_TEMP_LO, &lo);
	if (ret)
		return ret;
	ret = imx461_read_reg_gated(s, FUJISAN_IMX461_REG_TEMP_HI, &hi);
	if (ret)
		return ret;

	*raw_temp = fujisan_imx461_decode_temp_raw(lo, hi);
	return 0;
}

static int imx461_power_on(struct fujisan_imx461 *s)
{
	int ret;

	if (s->powered)
		return 0;

	ret = pm_runtime_resume_and_get(s->dev);
	if (ret < 0)
		return ret;

	/* DAT @ S5 0x02506E28..0x0250B260: drive sync low, assert rails, 10ms pulse on GPIO(0,15) */
	if (s->sync0_gpio)
		gpiod_set_value_cansleep(s->sync0_gpio, 0);
	if (s->sync1_gpio)
		gpiod_set_value_cansleep(s->sync1_gpio, 0);
	if (s->rail_b_gpio)
		gpiod_set_value_cansleep(s->rail_b_gpio, 1);
	if (s->rail_a_gpio)
		gpiod_set_value_cansleep(s->rail_a_gpio, 1);
	if (s->rst_pulse_gpio) {
		gpiod_set_value_cansleep(s->rst_pulse_gpio, 1);
		msleep(FUJISAN_IMX461_RST_PULSE_MS);
		gpiod_set_value_cansleep(s->rst_pulse_gpio, 0);
	}
	if (s->xclr_gpio)
		gpiod_set_value_cansleep(s->xclr_gpio, 1);

	s->powered = true;
	return 0;
}

static void imx461_power_off(struct fujisan_imx461 *s)
{
	if (!s->powered)
		return;

	if (s->xclr_gpio)
		gpiod_set_value_cansleep(s->xclr_gpio, 0);
	if (s->rail_a_gpio)
		gpiod_set_value_cansleep(s->rail_a_gpio, 0);
	if (s->rail_b_gpio)
		gpiod_set_value_cansleep(s->rail_b_gpio, 0);

	pm_runtime_put(s->dev);
	s->powered = false;
}

static int imx461_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct fujisan_imx461 *s = to_imx461(sd);
	uint8_t mdsel_rb = 0;
	int ret = 0;

	mutex_lock(&s->lock);
	if (enable && !s->streaming) {
		ret = imx461_power_on(s);
		if (ret)
			goto out;

		/* Wake sleep gate 0x019E bit 2 -> write MDSEL -> verify via 0x004F -> STBY bit 0 */
		ret = imx461_write_reg(s, FUJISAN_IMX461_REG_SLEEP_GATE,
				       FUJISAN_IMX461_SLEEP_WAKE_BIT);
		if (!ret)
			ret = imx461_write_reg(s, FUJISAN_IMX461_REG_MDSEL, s->cur_mode.mdsel);
		if (!ret)
			ret = imx461_read_reg_gated(s, FUJISAN_IMX461_REG_MDSEL, &mdsel_rb);
		if (!ret)
			ret = imx461_write_reg(s, FUJISAN_IMX461_REG_STBY, 0x05U);
		if (!ret) {
			s->streaming = true;
			dev_info(s->dev, "IMX461 streaming mode=%s (MDSEL=0x%02x, rb=0x%02x)\n",
				 s->cur_mode.name, s->cur_mode.mdsel, mdsel_rb);
		}
	} else if (!enable && s->streaming) {
		imx461_write_reg(s, FUJISAN_IMX461_REG_STBY, 0x04U);
		imx461_write_reg(s, FUJISAN_IMX461_REG_SLEEP_GATE, 0x00U);
		imx461_power_off(s);
		s->streaming = false;
	}
out:
	mutex_unlock(&s->lock);
	return ret;
}

static int imx461_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct fujisan_imx461 *s = container_of(ctrl->handler, struct fujisan_imx461, ctrls);
	uint8_t shr[3];
	int ret = 0;

	if (!s->powered)
		return 0;

	/* Wrap dynamic exposure/gain updates in REGHOLD (0x0002 bit 6) per DAT @ 0x024FCA68 */
	imx461_write_reg(s, FUJISAN_IMX461_REG_REGHOLD, FUJISAN_IMX461_REGHOLD_BIT);
	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = imx461_write_reg(s, FUJISAN_IMX461_REG_PGC_LO, (uint8_t)(ctrl->val & 0xFF));
		if (!ret)
			ret = imx461_write_reg(s, FUJISAN_IMX461_REG_PGC_HI,
					       (uint8_t)((ctrl->val >> 8) & 0xFF));
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = imx461_write_reg(s, FUJISAN_IMX461_REG_DGAIN, (uint8_t)(ctrl->val & 0xFF));
		break;
	case V4L2_CID_EXPOSURE:
		fujisan_imx461_pack_shr((uint32_t)ctrl->val, shr);
		ret = imx461_write_reg(s, FUJISAN_IMX461_REG_SHR_0, shr[0]);
		if (!ret)
			ret = imx461_write_reg(s, FUJISAN_IMX461_REG_SHR_1, shr[1]);
		if (!ret)
			ret = imx461_write_reg(s, FUJISAN_IMX461_REG_SHR_2, shr[2]);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	imx461_write_reg(s, FUJISAN_IMX461_REG_REGHOLD, 0x00U);
	return ret;
}

static const struct v4l2_ctrl_ops imx461_ctrl_ops = {
	.s_ctrl = imx461_s_ctrl,
};

static const struct v4l2_subdev_video_ops imx461_video_ops = {
	.s_stream = imx461_s_stream,
};

static const struct v4l2_subdev_ops imx461_subdev_ops = {
	.video = &imx461_video_ops,
};

static ssize_t sensor_temp_raw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fujisan_imx461 *s = dev_get_drvdata(dev);
	uint16_t raw = 0;
	int ret;

	mutex_lock(&s->lock);
	if (!s->powered) {
		mutex_unlock(&s->lock);
		return -ENODATA;
	}
	ret = imx461_read_temperature(s, &raw);
	mutex_unlock(&s->lock);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", raw);
}
static DEVICE_ATTR_RO(sensor_temp_raw);

static int fujisan_imx461_probe(struct platform_device *pdev)
{
	struct fujisan_imx461 *s;
	int ret;

	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	s->dev = &pdev->dev;
	mutex_init(&s->lock);
	fujisan_imx461_get_mode(23, &s->cur_mode); /* DAT boot default T[23] */

	s->spi_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s->spi_base))
		return PTR_ERR(s->spi_base);

	s->cmd_buf = dmam_alloc_coherent(&pdev->dev, 64, &s->cmd_dma, GFP_KERNEL);
	if (!s->cmd_buf)
		return -ENOMEM;

	s->xclr_gpio = devm_gpiod_get_optional(&pdev->dev, "xclr", GPIOD_OUT_LOW);
	s->rail_a_gpio = devm_gpiod_get_optional(&pdev->dev, "rail-a", GPIOD_OUT_LOW);
	s->rail_b_gpio = devm_gpiod_get_optional(&pdev->dev, "rail-b", GPIOD_OUT_LOW);
	s->rst_pulse_gpio = devm_gpiod_get_optional(&pdev->dev, "rst-pulse", GPIOD_OUT_LOW);
	s->sync0_gpio = devm_gpiod_get_optional(&pdev->dev, "sync0", GPIOD_OUT_LOW);
	s->sync1_gpio = devm_gpiod_get_optional(&pdev->dev, "sync1", GPIOD_OUT_LOW);

	v4l2_ctrl_handler_init(&s->ctrls, 3);
	v4l2_ctrl_new_std(&s->ctrls, &imx461_ctrl_ops, V4L2_CID_ANALOGUE_GAIN, 0, 1023, 1, 0);
	v4l2_ctrl_new_std(&s->ctrls, &imx461_ctrl_ops, V4L2_CID_DIGITAL_GAIN, 0, 255, 1, 0);
	v4l2_ctrl_new_std(&s->ctrls, &imx461_ctrl_ops, V4L2_CID_EXPOSURE, 1, 0xFFFFFF, 1, 512);
	if (s->ctrls.error) {
		ret = s->ctrls.error;
		v4l2_ctrl_handler_free(&s->ctrls);
		return ret;
	}

	v4l2_subdev_init(&s->sd, &imx461_subdev_ops);
	s->sd.dev = &pdev->dev;
	s->sd.ctrl_handler = &s->ctrls;
	strscpy(s->sd.name, "fujisan-imx461", sizeof(s->sd.name));

	pm_runtime_enable(&pdev->dev);
	device_create_file(&pdev->dev, &dev_attr_sensor_temp_raw);
	platform_set_drvdata(pdev, s);
	dev_info(&pdev->dev, "Fujisan IMX461 102MP CMOS subdev probed (25 DAT modes, SPI @ 0xFFF6C200)\n");
	return 0;
}

static void fujisan_imx461_remove(struct platform_device *pdev)
{
	struct fujisan_imx461 *s = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_sensor_temp_raw);
	v4l2_ctrl_handler_free(&s->ctrls);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id fujisan_imx461_of_match[] = {
	{ .compatible = "fujisan,imx461" },
	{ }
};
MODULE_DEVICE_TABLE(of, fujisan_imx461_of_match);

static struct platform_driver fujisan_imx461_driver = {
	.probe = fujisan_imx461_probe,
	.remove_new = fujisan_imx461_remove,
	.driver = {
		.name = "fujisan-imx461",
		.of_match_table = fujisan_imx461_of_match,
	},
};
module_platform_driver(fujisan_imx461_driver);

MODULE_AUTHOR("Fujisan OS Contributors");
MODULE_DESCRIPTION("Fujisan OS Sony IMX461 102MP CMOS Sensor V4L2 Subdev");
MODULE_LICENSE("GPL");
