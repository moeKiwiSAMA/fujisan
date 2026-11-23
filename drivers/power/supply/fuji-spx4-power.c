// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPX4-MF Medium-Format Camera Power-Peer SPI1 Checksum Protocol, Battery Telemetry,
 * Checksummed Retained NVMEM & Clean Hardware Power-Off Driver
 *
 * Wave-3 Hardware Protocol (DAT verified via boot.bin & section4.bin):
 *   - Bus: SPI1 (0xFFF33000), 8-bit big-endian transfers, CS GPIO Bank 12 Pin 5 (ACTIVE-HIGH!)
 *   - 4-byte 1's-complement checksum frame (retries up to 3x on xfer/checksum error):
 *       TX[0] = (write ? 0x80 : 0x00) | (selector & 0x7F)
 *       TX[1] = write ? (val >> 8) : 0x00
 *       TX[2] = write ? (val & 0xFF) : 0x00
 *       TX[3] = (~(TX[0] + TX[1] + TX[2])) & 0xFF   (sum of 4 bytes & 0xFF == 0xFF)
 *       RX[1..2] = big-endian 16-bit response, RX[3] verified against ~(RX[0]+RX[1]+RX[2])
 *   - Battery telemetry:
 *       sel 0x03 (10-bit voltage ADC) + ADC CH0 fallback
 *       sel 0x30 (smart-battery SoC clamped 1..100%) + ADC CH0 fallback
 *       sel 0x32 (0.1 K SMBus temperature -> (raw - 2732) decidegrees C)
 *   - Retained RAM (sel 0x1F..0x22):
 *       8-byte checksummed nvmem (sum(bytes[0..7]) & 0xFF == 0), committed via
 *       sel 0x27 (bit 0 = 1, bit 9 = 0), exposed via Linux NVMEM & exported API.
 *   - Clean camera power-off (devm_register_sys_off_handler):
 *       sel 0x02 |= bit 2, sel 0x0F = wake-source mask (sel0E.b0, ~sel00.b3 -> b14,
 *       ~sel00.b14 -> b15), then sel 0x11 = 0x0800.
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
#include <linux/spi/spi.h>

#include "fuji_spx4_hw.h"

extern int fuji_spx4_adc_read_channel(unsigned int ch, int *val);

struct fuji_spx4_power {
	struct spi_device *spi;
	struct gpio_desc *cs_gpio;
	struct power_supply *psy;
	struct nvmem_device *nvmem;
	struct mutex lock;
	u32 read_errors;
	u32 write_errors;
};

static struct fuji_spx4_power *g_spx4_power;

static int spx4_power_xfer4(struct fuji_spx4_power *p, u8 reg, bool write,
			    u16 wval, u16 *rval)
{
	u8 tx[4], rx[4];
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = 4,
		.bits_per_word = 8,
	};
	int attempt, ret = -EIO;

	spx4_power_encode_frame(tx, reg, write, wval);

	mutex_lock(&p->lock);
	for (attempt = 0; attempt < SPX4_PWR_MAX_RETRIES; attempt++) {
		memset(rx, 0, sizeof(rx));
		if (p->cs_gpio)
			gpiod_set_value_cansleep(p->cs_gpio, 1); /* Active-HIGH CS */
		ret = spi_sync_transfer(p->spi, &xfer, 1);
		if (p->cs_gpio)
			gpiod_set_value_cansleep(p->cs_gpio, 0);

		if (ret)
			continue;

		if (!write) {
			u16 decoded = 0;

			if (!spx4_power_decode_response(rx, &decoded)) {
				ret = -EBADMSG;
				continue;
			}
			if (rval)
				*rval = decoded;
		}
		mutex_unlock(&p->lock);
		return 0;
	}

	if (write)
		p->write_errors++;
	else
		p->read_errors++;
	mutex_unlock(&p->lock);
	return ret;
}

int fuji_spx4_power_read_reg(u8 reg, u16 *val)
{
	if (!g_spx4_power)
		return -ENODEV;
	return spx4_power_xfer4(g_spx4_power, reg, false, 0, val);
}
EXPORT_SYMBOL_GPL(fuji_spx4_power_read_reg);

int fuji_spx4_power_write_reg(u8 reg, u16 val)
{
	if (!g_spx4_power)
		return -ENODEV;
	return spx4_power_xfer4(g_spx4_power, reg, true, val, NULL);
}
EXPORT_SYMBOL_GPL(fuji_spx4_power_write_reg);

int fuji_spx4_power_nvmem_read(u8 buf[SPX4_PWR_NVMEM_BYTES])
{
	u16 words[SPX4_PWR_NVMEM_WORDS];
	unsigned int i;
	int ret;

	if (!g_spx4_power || !buf)
		return -ENODEV;

	for (i = 0; i < SPX4_PWR_NVMEM_WORDS; i++) {
		ret = spx4_power_xfer4(g_spx4_power,
				       (u8)(SPX4_PWR_SEL_NVMEM_W0 + i),
				       false, 0, &words[i]);
		if (ret)
			return ret;
	}

	spx4_power_nvmem_unpack_words(words, buf);
	return 0;
}
EXPORT_SYMBOL_GPL(fuji_spx4_power_nvmem_read);

int fuji_spx4_power_nvmem_write(const u8 buf[SPX4_PWR_NVMEM_BYTES])
{
	u8 framed[SPX4_PWR_NVMEM_BYTES];
	u16 words[SPX4_PWR_NVMEM_WORDS];
	u16 ctrl = 0;
	unsigned int i;
	int ret;

	if (!g_spx4_power || !buf)
		return -ENODEV;

	memcpy(framed, buf, 7);
	framed[7] = spx4_power_nvmem_checksum(framed);
	spx4_power_nvmem_pack_words(framed, words);

	for (i = 0; i < SPX4_PWR_NVMEM_WORDS; i++) {
		ret = spx4_power_xfer4(g_spx4_power,
				       (u8)(SPX4_PWR_SEL_NVMEM_W0 + i),
				       true, words[i], NULL);
		if (ret)
			return ret;
	}

	/* Commit retained RAM block via sel 0x27: bit 0 = 1, bit 9 = 0 (DAT @ 0x0120D2D0) */
	if (spx4_power_xfer4(g_spx4_power, SPX4_PWR_SEL_RTC_CTRL, false, 0, &ctrl))
		ctrl = SPX4_PWR_RTC_CTRL_TIME_VALID;
	ctrl = (ctrl & ~SPX4_PWR_RTC_CTRL_LOAD_STROBE) | SPX4_PWR_RTC_CTRL_NVMEM_COMMIT;
	return spx4_power_xfer4(g_spx4_power, SPX4_PWR_SEL_RTC_CTRL, true, ctrl, NULL);
}
EXPORT_SYMBOL_GPL(fuji_spx4_power_nvmem_write);

static int spx4_nvmem_reg_read(void *priv, unsigned int offset, void *val, size_t bytes)
{
	u8 buf[SPX4_PWR_NVMEM_BYTES];
	int ret;

	if (offset + bytes > SPX4_PWR_NVMEM_BYTES)
		return -EINVAL;

	ret = fuji_spx4_power_nvmem_read(buf);
	if (ret)
		return ret;

	memcpy(val, buf + offset, bytes);
	return 0;
}

static int spx4_nvmem_reg_write(void *priv, unsigned int offset, void *val, size_t bytes)
{
	u8 buf[SPX4_PWR_NVMEM_BYTES] = { 0 };
	int ret;

	if (offset + bytes > SPX4_PWR_NVMEM_BYTES)
		return -EINVAL;

	if (offset != 0 || bytes != SPX4_PWR_NVMEM_BYTES) {
		ret = fuji_spx4_power_nvmem_read(buf);
		if (ret)
			memset(buf, 0, sizeof(buf));
	}
	memcpy(buf + offset, val, bytes);
	return fuji_spx4_power_nvmem_write(buf);
}

static int spx4_power_off_handler(struct sys_off_data *data)
{
	struct fuji_spx4_power *p = data->cb_data;
	u16 sel00 = 0, sel02 = 0, sel0E = 0;
	u16 wake_mask;

	if (!p)
		return NOTIFY_DONE;

	/* DAT @ section4.bin 0x012EDA94..0x012EDB18 */
	if (spx4_power_xfer4(p, SPX4_PWR_SEL_CTRL, false, 0, &sel02) == 0)
		spx4_power_xfer4(p, SPX4_PWR_SEL_CTRL, true,
				 sel02 | SPX4_PWR_CTRL_SHUTDOWN_FLAG, NULL);

	spx4_power_xfer4(p, SPX4_PWR_SEL_STATUS, false, 0, &sel00);
	spx4_power_xfer4(p, SPX4_PWR_SEL_FLAGS, false, 0, &sel0E);
	wake_mask = spx4_power_build_shutdown_mask(sel00, sel0E);

	spx4_power_xfer4(p, SPX4_PWR_SEL_SHUTDOWN_MASK, true, wake_mask, NULL);
	spx4_power_xfer4(p, SPX4_PWR_SEL_CMD, true, SPX4_PWR_CMD_POWER_OFF, NULL);
	return NOTIFY_DONE;
}

static enum power_supply_property spx4_batt_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static int spx4_batt_get_prop(struct power_supply *psy,
			      enum power_supply_property psp,
			      union power_supply_propval *val)
{
	struct fuji_spx4_power *p = power_supply_get_drvdata(psy);
	u16 reg16 = 0;
	s32 decidegc = 250;
	int raw = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		return 0;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* Prefer Sub-MCU sel 0x03 (10-bit ADC), fallback to SoC ADC CH0 */
		if (p && spx4_power_xfer4(p, SPX4_PWR_SEL_BATT_ADC, false, 0, &reg16) == 0 &&
		    (reg16 & 0x3FFU) > 0) {
			val->intval = 6200000 + ((int)(reg16 & 0x3FFU) * 2200000 / 1023);
		} else if (fuji_spx4_adc_read_channel(0, &raw) == 0 && raw > 0) {
			val->intval = 6200000 + (raw * 2200000 / 4095);
		} else {
			val->intval = 7400000;
		}
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		/* Smart-battery sel 0x30 clamped 1..100% (DAT @ 0x0120D37C), else ADC CH0 */
		if (p && spx4_power_xfer4(p, SPX4_PWR_SEL_SMART_SOC, false, 0, &reg16) == 0 &&
		    reg16 >= 1 && reg16 <= 100) {
			val->intval = (int)reg16;
		} else if (fuji_spx4_adc_read_channel(0, &raw) == 0 && raw > 0) {
			val->intval = clamp((raw * 100) / 4095, 5, 100);
		} else {
			val->intval = 85;
		}
		return 0;
	case POWER_SUPPLY_PROP_TEMP:
		/* DAT-proven sel 0x32: 0.1 K -> (raw - 2732) decidegrees C */
		if (p && spx4_power_xfer4(p, SPX4_PWR_SEL_BATT_TEMP_DK, false, 0, &reg16) == 0 &&
		    spx4_power_temp_dk_to_decidegc(reg16, &decidegc)) {
			val->intval = decidegc;
		} else {
			val->intval = 250; /* 25.0 degC safe default if Sub-MCU absent */
		}
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc spx4_batt_desc = {
	.name		= "spx4-mf-battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= spx4_batt_props,
	.num_properties	= ARRAY_SIZE(spx4_batt_props),
	.get_property	= spx4_batt_get_prop,
};

static int fuji_spx4_power_probe(struct spi_device *spi)
{
	struct fuji_spx4_power *p;
	struct power_supply_config cfg = {};
	struct nvmem_config nvcfg = {};
	int ret;

	p = devm_kzalloc(&spi->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->spi = spi;
	mutex_init(&p->lock);
	p->cs_gpio = devm_gpiod_get_optional(&spi->dev, "cs-high", GPIOD_OUT_LOW);
	if (IS_ERR(p->cs_gpio))
		return PTR_ERR(p->cs_gpio);

	cfg.of_node = spi->dev.of_node;
	cfg.drv_data = p;
	p->psy = devm_power_supply_register(&spi->dev, &spx4_batt_desc, &cfg);
	if (IS_ERR(p->psy))
		return PTR_ERR(p->psy);

	g_spx4_power = p;
	spi_set_drvdata(spi, p);

	/* Expose sel 0x1F..0x22 8-byte checksummed retained RAM via NVMEM */
	if (IS_ENABLED(CONFIG_NVMEM)) {
		nvcfg.name = "spx4-mf-power-nvmem";
		nvcfg.id = NVMEM_DEVID_NONE;
		nvcfg.dev = &spi->dev;
		nvcfg.owner = THIS_MODULE;
		nvcfg.type = NVMEM_TYPE_BATTERY_BACKED;
		nvcfg.size = SPX4_PWR_NVMEM_BYTES;
		nvcfg.word_size = 1;
		nvcfg.stride = 1;
		nvcfg.reg_read = spx4_nvmem_reg_read;
		nvcfg.reg_write = spx4_nvmem_reg_write;
		nvcfg.priv = p;
		p->nvmem = devm_nvmem_register(&spi->dev, &nvcfg);
		if (IS_ERR(p->nvmem)) {
			dev_warn(&spi->dev, "retained nvmem register failed: %ld\n",
				 PTR_ERR(p->nvmem));
			p->nvmem = NULL;
		}
	}

	/* Register clean camera power-off handler (sel 0x0F + sel 0x11=0x0800) */
	ret = devm_register_sys_off_handler(&spi->dev,
					    SYS_OFF_MODE_POWER_OFF,
					    SYS_OFF_PRIO_DEFAULT,
					    spx4_power_off_handler, p);
	if (ret)
		dev_warn(&spi->dev, "sys-off power-off registration failed: %d\n", ret);

	dev_info(&spi->dev,
		 "SPX4-MF Medium-Format Camera power-peer SPI1 registered (battery temp/SoC, 8B nvmem, sys-off)\n");
	return 0;
}

static const struct of_device_id fuji_spx4_power_of_match[] = {
	{ .compatible = "fujisan,spx4-mf-power-peer" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_power_of_match);

static struct spi_driver fuji_spx4_power_driver = {
	.driver = {
		.name = "fuji-spx4-power",
		.of_match_table = fuji_spx4_power_of_match,
	},
	.probe = fuji_spx4_power_probe,
};
module_spi_driver(fuji_spx4_power_driver);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera Power-Peer SPI1, Battery, NVMEM & Sys-Off Driver");
MODULE_LICENSE("GPL");
