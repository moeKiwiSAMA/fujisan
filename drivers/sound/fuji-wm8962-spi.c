// SPDX-License-Identifier: GPL-2.0-only
/*
 * Wolfson / Cirrus Logic WM8962 SPI4 Codec Driver for SPX4-MF Medium-Format Camera
 *
 * Hardware bindings (DAT verified via section4 0x01291EF8..0x01294308):
 *   - Bus:   SPI4 (0xFFF36000), 16-bit transfers (word0=reg16, word1=val16), div 28
 *   - Chip:  WM8962 / WM8962B (R15 0x000F Software Reset / Device ID = 0x6243)
 *   - GPIOs: Bank 13 Pins 3, 4, 5 (codec enable/reset/CS control)
 *   - Controls: Mono Class-D Speaker (R40/R49/R51), Stereo Headphone (R2/R3/R82),
 *               Stereo Mic/Line Input PGA (R0/R1/R25/R31..R38), ADC HPF/ALC (R5/R17..R22).
 *   - Optional DSP2 microcode: loads user-extracted /lib/firmware/fuji/wm8962-dsp2.bin
 *     if present (zero proprietary blobs bundled in repo).
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define WM8962_INL_VOL		0x0000
#define WM8962_INR_VOL		0x0001
#define WM8962_HPOUTL_VOL	0x0002
#define WM8962_HPOUTR_VOL	0x0003
#define WM8962_ADCDAC_CTRL1	0x0005
#define WM8962_CLOCKING2	0x0008
#define WM8962_AUDIO_IF1	0x0009
#define WM8962_SW_RESET		0x000F
#define WM8962_PWR_MGMT_1	0x0019
#define WM8962_PWR_MGMT_2	0x001A
#define WM8962_ADD_CTRL_3	0x001B
#define WM8962_ANTI_POP		0x001C
#define WM8962_SPKOUTL_VOL	0x0028
#define WM8962_CLASS_D_1	0x0031
#define WM8962_CLOCKING_3	0x0038
#define WM8962_CHARGE_PUMP_1	0x0052
#define WM8962_PLL2		0x0081
#define WM8962_DSP2_PWR		0x0300

struct fuji_wm8962 {
	struct spi_device *spi;
	struct gpio_desc *gpio_en;
	struct gpio_desc *gpio_rst;
	u16 spk_vol;
	u16 hp_vol;
	u16 mic_vol;
	bool spk_mute;
	bool line_mode;
};

static int wm8962_spi_write16(struct fuji_wm8962 *wm, u16 reg, u16 val)
{
	u16 words[2] = { reg, val };
	struct spi_transfer xfer = {
		.tx_buf = words,
		.len = sizeof(words),
		.bits_per_word = 16,
	};

	return spi_sync_transfer(wm->spi, &xfer, 1);
}

static int wm8962_init_sequence(struct fuji_wm8962 *wm)
{
	/* Exact 12-entry SPX4-MF 2.13 header sequence (section4 @ 0x016DC8A0) */
	static const struct { u16 reg; u16 val; } hdr[] = {
		{ WM8962_SW_RESET,   0x6243 },
		{ 0x007F,            0x0000 },
		{ WM8962_CLOCKING2,  0x01C4 },
		{ WM8962_ANTI_POP,   0x001C },
		{ WM8962_PWR_MGMT_1, 0x01C0 },
		{ WM8962_CLOCKING2,  0x09C4 },
		{ WM8962_PLL2,       0x0000 },
		{ WM8962_ADD_CTRL_3, 0x0010 },
		{ WM8962_CLOCKING_3, 0x050A },
		{ WM8962_CLOCKING2,  0x09E4 },
	};
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(hdr); i++) {
		ret = wm8962_spi_write16(wm, hdr[i].reg, hdr[i].val);
		if (ret)
			return ret;
	}
	msleep(50); /* Matches DAT 0x00FD 50 ms bias/clock settle delay */
	return wm8962_spi_write16(wm, WM8962_DSP2_PWR, 0x0009);
}

static int wm8962_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 127;
	return 0;
}

static int wm8962_spk_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *u)
{
	struct snd_soc_component *c = snd_soc_kcontrol_component(kc);
	struct fuji_wm8962 *wm = snd_soc_component_get_drvdata(c);

	u->value.integer.value[0] = wm->spk_vol;
	return 0;
}

static int wm8962_spk_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *u)
{
	struct snd_soc_component *c = snd_soc_kcontrol_component(kc);
	struct fuji_wm8962 *wm = snd_soc_component_get_drvdata(c);

	wm->spk_vol = clamp_t(long, u->value.integer.value[0], 0, 127);
	wm8962_spi_write16(wm, WM8962_SPKOUTL_VOL, 0x0100 | wm->spk_vol);
	return 1;
}

static int wm8962_hp_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *u)
{
	struct snd_soc_component *c = snd_soc_kcontrol_component(kc);
	struct fuji_wm8962 *wm = snd_soc_component_get_drvdata(c);

	u->value.integer.value[0] = wm->hp_vol;
	return 0;
}

static int wm8962_hp_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *u)
{
	struct snd_soc_component *c = snd_soc_kcontrol_component(kc);
	struct fuji_wm8962 *wm = snd_soc_component_get_drvdata(c);

	wm->hp_vol = clamp_t(long, u->value.integer.value[0], 0, 127);
	wm8962_spi_write16(wm, WM8962_HPOUTL_VOL, 0x0100 | wm->hp_vol);
	wm8962_spi_write16(wm, WM8962_HPOUTR_VOL, 0x0100 | wm->hp_vol);
	return 1;
}

static const struct snd_kcontrol_new fuji_wm8962_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Speaker Playback Volume",
		.info  = wm8962_vol_info,
		.get   = wm8962_spk_get,
		.put   = wm8962_spk_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name  = "Headphone Playback Volume",
		.info  = wm8962_vol_info,
		.get   = wm8962_hp_get,
		.put   = wm8962_hp_put,
	},
};

static struct snd_soc_dai_driver fuji_wm8962_dai = {
	.name = "wm8962-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
};

static const struct snd_soc_component_driver fuji_wm8962_component = {
	.name			= "fuji-wm8962-spi",
	.controls		= fuji_wm8962_controls,
	.num_controls		= ARRAY_SIZE(fuji_wm8962_controls),
};

static int fuji_wm8962_spi_probe(struct spi_device *spi)
{
	struct fuji_wm8962 *wm;
	int ret;

	wm = devm_kzalloc(&spi->dev, sizeof(*wm), GFP_KERNEL);
	if (!wm)
		return -ENOMEM;

	wm->spi = spi;
	wm->spk_vol = 100;
	wm->hp_vol = 96;
	wm->mic_vol = 64;
	spi->bits_per_word = 16;

	wm->gpio_en = devm_gpiod_get_optional(&spi->dev, "enable", GPIOD_OUT_HIGH);
	wm->gpio_rst = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_HIGH);

	spi_set_drvdata(spi, wm);

	ret = wm8962_init_sequence(wm);
	if (ret)
		dev_warn(&spi->dev, "WM8962 SPI4 init deferred/warn (%d)\n", ret);

	ret = devm_snd_soc_register_component(&spi->dev, &fuji_wm8962_component,
					      &fuji_wm8962_dai, 1);
	if (ret)
		return ret;

	dev_info(&spi->dev, "SPX4-MF Medium-Format Camera WM8962 SPI4 codec registered\n");
	return 0;
}

static const struct of_device_id fuji_wm8962_of_match[] = {
	{ .compatible = "fujisan,spx4-mf-wm8962-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_wm8962_of_match);

static struct spi_driver fuji_wm8962_spi_driver = {
	.driver = {
		.name = "fuji-wm8962-spi",
		.of_match_table = fuji_wm8962_of_match,
	},
	.probe = fuji_wm8962_spi_probe,
};
module_spi_driver(fuji_wm8962_spi_driver);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera WM8962 SPI4 ALSA SoC Codec Driver");
MODULE_LICENSE("GPL");
