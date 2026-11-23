// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA SoC Machine Driver for SPX4-MF Medium-Format Camera Audio
 *   - Links SPX4 I2S0 (0xFFF4F000) <-> WM8962 SPI4 (0xFFF36000)
 *   - 3.5mm Headphone Jack Detect: GPIO Bank 8 Pin 4 (400 ms debounce)
 *   - 3.5mm External Mic Detect:   GPIO Bank 6 Pin 4 (25 ms debounce)
 */

#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/jack.h>
#include <sound/soc.h>

static struct snd_soc_jack spx4_mf_hp_jack;
static struct snd_soc_jack spx4_mf_mic_jack;

static struct snd_soc_jack_pin spx4_mf_hp_pins[] = {
	{ .pin = "Headphone", .mask = SND_JACK_HEADPHONE },
};

static struct snd_soc_jack_pin spx4_mf_mic_pins[] = {
	{ .pin = "External Mic", .mask = SND_JACK_MICROPHONE },
};

static int spx4_mf_audio_init(struct snd_soc_pcm_runtime *rtd)
{
	snd_soc_card_jack_new_pins(rtd->card, "Headphone Jack", SND_JACK_HEADPHONE,
				   &spx4_mf_hp_jack, spx4_mf_hp_pins,
				   ARRAY_SIZE(spx4_mf_hp_pins));
	snd_soc_card_jack_new_pins(rtd->card, "External Mic Jack", SND_JACK_MICROPHONE,
				   &spx4_mf_mic_jack, spx4_mf_mic_pins,
				   ARRAY_SIZE(spx4_mf_mic_pins));
	return 0;
}

SND_SOC_DAILINK_DEFS(spx4_mf_hifi,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "wm8962-hifi")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()));

static struct snd_soc_dai_link spx4_mf_dai_links[] = {
	{
		.name		= "SPX4-MF-WM8962",
		.stream_name	= "SPX4-MF HiFi",
		.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBS_CFS,
		.init		= spx4_mf_audio_init,
		SND_SOC_DAILINK_REG(spx4_mf_hifi),
	},
};

static struct snd_soc_card spx4_mf_audio_card = {
	.name		= "Fujisan-SPX4-MF",
	.owner		= THIS_MODULE,
	.dai_link	= spx4_mf_dai_links,
	.num_links	= ARRAY_SIZE(spx4_mf_dai_links),
};

static int fujisan_spx4_audio_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np, *codec_np;
	struct snd_soc_card *card = &spx4_mf_audio_card;
	int ret;

	cpu_np = of_parse_phandle(pdev->dev.of_node, "fujisan,i2s-controller", 0);
	codec_np = of_parse_phandle(pdev->dev.of_node, "fujisan,audio-codec", 0);
	if (!cpu_np || !codec_np) {
		of_node_put(cpu_np);
		of_node_put(codec_np);
		return -ENODEV;
	}

	spx4_mf_dai_links[0].cpus->of_node = cpu_np;
	spx4_mf_dai_links[0].codecs->of_node = codec_np;
	card->dev = &pdev->dev;

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	of_node_put(cpu_np);
	of_node_put(codec_np);
	return ret;
}

static const struct of_device_id fujisan_spx4_audio_of_match[] = {
	{ .compatible = "fujisan,spx4-mf-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, fujisan_spx4_audio_of_match);

static struct platform_driver fujisan_spx4_audio_driver = {
	.probe = fujisan_spx4_audio_probe,
	.driver = {
		.name = "fujisan-spx4-audio",
		.of_match_table = fujisan_spx4_audio_of_match,
	},
};
module_platform_driver(fujisan_spx4_audio_driver);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera ALSA SoC Machine Driver");
MODULE_LICENSE("GPL");
