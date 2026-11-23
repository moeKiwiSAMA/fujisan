// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan OS — GF-Mount LENSCOM SPI0 Frame32 Lens Controller (v4l2-subdev)
 *
 * DAT verified (SPX4-MF 2.13 M9 LENSCOM Atlas):
 *   - SPI0 (0xFFF32000), GPIO mount detect (0.11=0), lens power (15.0=1), LENS_RDY (15.1=1 / IRQ 182)
 *   - S12 Sentinel handshake: tx=0xA55A3CC3 -> rx=0xC33CA55A, next rx=0xA55A3CC3
 *   - SPI0 Bulk Mode: 0x018 enter, 0x014 read 128-byte Lens Identity Header + 0xFFFF END,
 *     0x015 0xFF00 + 0x5A ACK, 0x016 exit
 *   - AF focus drive (0x040/0x050/0x054) & Iris aperture control (0x080/0x0A8) via V4L2 controls
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "fujisan_spx4_hw.h"

struct fujisan_lenscom {
	struct spi_device *spi;
	struct gpio_desc *mount_det_gpio;	/* GPIO(0,11) active-LOW */
	struct gpio_desc *lens_pwr_gpio;	/* GPIO(15,0) */
	struct gpio_desc *lens_rdy_gpio;	/* GPIO(15,1) IRQ 182 */

	struct mutex lock;
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct fujisan_lens_identity ident;
	uint8_t seq;
	bool link_ready;
};

static int lenscom_xfer32(struct fujisan_lenscom *lc, uint32_t tx_word, uint32_t *rx_word)
{
	__be32 tx = cpu_to_be32(tx_word);
	__be32 rx = 0;
	struct spi_transfer t = {
		.tx_buf = &tx,
		.rx_buf = &rx,
		.len = sizeof(tx),
	};
	int ret = spi_sync_transfer(lc->spi, &t, 1);

	if (!ret && rx_word)
		*rx_word = be32_to_cpu(rx);
	return ret;
}

static int lenscom_handshake_and_probe_header(struct fujisan_lenscom *lc)
{
	uint32_t rx0 = 0, rx1 = 0;
	uint8_t hdr[FUJISAN_LENSCOM_HDR_BYTES];
	int ret;

	if (lc->lens_pwr_gpio)
		gpiod_set_value_cansleep(lc->lens_pwr_gpio, 1);

	/* S12 Sentinel handshake: 0xA55A3CC3 -> 0xC33CA55A -> 0xA55A3CC3 */
	ret = lenscom_xfer32(lc, FUJISAN_LENSCOM_SENTINEL_TX, &rx0);
	if (ret)
		return ret;
	ret = lenscom_xfer32(lc, 0U, &rx1);
	if (ret)
		return ret;
	if (!fujisan_lenscom_verify_sentinel(rx0, rx1)) {
		dev_warn(&lc->spi->dev, "LENSCOM sentinel mismatch (rx0=0x%08x rx1=0x%08x)\n",
			 rx0, rx1);
		return -ENODEV;
	}

	/* Enter bulk mode (0x018), fetch 128B identity header (0x014), exit (0x016) */
	lenscom_xfer32(lc, fujisan_lenscom_pack_frame32(FUJISAN_LENSCOM_CMD_BULK_ENTER,
							lc->seq++ & 0xFU, 0U), NULL);
	memset(hdr, 0, sizeof(hdr));
	spi_read(lc->spi, hdr, sizeof(hdr));
	lenscom_xfer32(lc, fujisan_lenscom_pack_frame32(FUJISAN_LENSCOM_CMD_BULK_EXIT,
							lc->seq++ & 0xFU, 0U), NULL);

	fujisan_lenscom_parse_identity(hdr, &lc->ident);
	lc->link_ready = true;
	return 0;
}

static int lenscom_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct fujisan_lenscom *lc = container_of(ctrl->handler, struct fujisan_lenscom, ctrls);
	uint32_t frame;
	int ret = 0;

	mutex_lock(&lc->lock);
	if (!lc->link_ready) {
		mutex_unlock(&lc->lock);
		return 0;
	}
	switch (ctrl->id) {
	case V4L2_CID_FOCUS_ABSOLUTE:
		frame = fujisan_lenscom_pack_frame32(FUJISAN_LENSCOM_CMD_AF_DRIVE,
						     lc->seq++ & 0xFU,
						     (uint8_t)(ctrl->val & 0xFF));
		ret = lenscom_xfer32(lc, frame, NULL);
		break;
	case V4L2_CID_IRIS_ABSOLUTE:
		frame = fujisan_lenscom_pack_frame32(FUJISAN_LENSCOM_CMD_IRIS_SET,
						     lc->seq++ & 0xFU,
						     (uint8_t)(ctrl->val & 0xFF));
		ret = lenscom_xfer32(lc, frame, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&lc->lock);
	return ret;
}

static const struct v4l2_ctrl_ops lenscom_ctrl_ops = {
	.s_ctrl = lenscom_s_ctrl,
};

static const struct v4l2_subdev_ops lenscom_subdev_ops = { };

static int fujisan_lenscom_probe(struct spi_device *spi)
{
	struct fujisan_lenscom *lc;
	int ret;

	lc = devm_kzalloc(&spi->dev, sizeof(*lc), GFP_KERNEL);
	if (!lc)
		return -ENOMEM;

	lc->spi = spi;
	mutex_init(&lc->lock);

	lc->mount_det_gpio = devm_gpiod_get_optional(&spi->dev, "mount-det", GPIOD_IN);
	lc->lens_pwr_gpio = devm_gpiod_get_optional(&spi->dev, "lens-pwr", GPIOD_OUT_LOW);
	lc->lens_rdy_gpio = devm_gpiod_get_optional(&spi->dev, "lens-rdy", GPIOD_IN);

	v4l2_ctrl_handler_init(&lc->ctrls, 2);
	v4l2_ctrl_new_std(&lc->ctrls, &lenscom_ctrl_ops, V4L2_CID_FOCUS_ABSOLUTE, 0, 255, 1, 128);
	v4l2_ctrl_new_std(&lc->ctrls, &lenscom_ctrl_ops, V4L2_CID_IRIS_ABSOLUTE, 0, 255, 1, 64);
	if (lc->ctrls.error) {
		ret = lc->ctrls.error;
		v4l2_ctrl_handler_free(&lc->ctrls);
		return ret;
	}

	v4l2_subdev_init(&lc->sd, &lenscom_subdev_ops);
	lc->sd.dev = &spi->dev;
	lc->sd.ctrl_handler = &lc->ctrls;
	strscpy(lc->sd.name, "fujisan-lenscom", sizeof(lc->sd.name));

	if (lenscom_handshake_and_probe_header(lc) == 0)
		dev_info(&spi->dev, "GF lens linked (FW BCD %02x.%02x)\n",
			 lc->ident.fw_major_bcd, lc->ident.fw_minor_bcd);
	else
		dev_info(&spi->dev, "GF lens mount idle (no lens or waiting for attach)\n");

	spi_set_drvdata(spi, lc);
	return 0;
}

static void fujisan_lenscom_remove(struct spi_device *spi)
{
	struct fujisan_lenscom *lc = spi_get_drvdata(spi);

	v4l2_ctrl_handler_free(&lc->ctrls);
}

static const struct of_device_id fujisan_lenscom_of_match[] = {
	{ .compatible = "fujisan,spx4-mf-lenscom" },
	{ }
};
MODULE_DEVICE_TABLE(of, fujisan_lenscom_of_match);

static struct spi_driver fujisan_lenscom_driver = {
	.probe = fujisan_lenscom_probe,
	.remove = fujisan_lenscom_remove,
	.driver = {
		.name = "fujisan-lenscom",
		.of_match_table = fujisan_lenscom_of_match,
	},
};
module_spi_driver(fujisan_lenscom_driver);

MODULE_AUTHOR("Fujisan OS Contributors");
MODULE_DESCRIPTION("Fujisan OS GF-Mount LENSCOM SPI0 Frame32 Lens Subdev");
MODULE_LICENSE("GPL");
