// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALSA SoC Dual-Channel I2S / PCM CPU DAI Driver for Fujisan SPX4
 *   - I2S Channel 0: 0xFFF4F000..0xFFF4F02B (WM8962 codec audio)
 *   - I2S Channel 1: 0xFFF78000..0xFFF7802B (Aux / HDMI audio)
 *   - DMA: soc-dma HW Ch 4 (req 28, GIC 211), Ch 7 (req 29, GIC 214), Ch 8 (req 30, GIC 215)
 *
 * Register layout (DAT verified via section4 0x011E1440..0x011E1B4C):
 *   +0x00 TX_FIFO   +0x04 RX_FIFO
 *   +0x08 MODE      0x1000254B (ch0) / 0x1000252B (ch1)
 *   +0x0C WIDTH     7=8bit, 15=16bit, 23=24bit, 31=32bit
 *   +0x10 CH_EN     0x00000003
 *   +0x14 CTRL_EXT  0x00000000
 *   +0x18 START     bit 16 (0x00010000) = TX start, bit 24 (0x01000000) = RX start
 *   +0x1C RESET     bit 0 self-clearing soft reset
 *   +0x20 FIFO_CTRL 0x7D3F0F0F (TX) / 0x7F3D0F0F (RX)
 *   +0x28 DMA_EN    0x01000000 (TX) / 0x00000100 (RX)
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define SPX4_I2S_TX_FIFO	0x00
#define SPX4_I2S_RX_FIFO	0x04
#define SPX4_I2S_MODE		0x08
#define SPX4_I2S_WIDTH		0x0C
#define SPX4_I2S_CH_EN		0x10
#define SPX4_I2S_EXT		0x14
#define SPX4_I2S_START		0x18
#define SPX4_I2S_RESET		0x1C
#define SPX4_I2S_FIFO_CTRL	0x20
#define SPX4_I2S_DMA_EN		0x28

struct fuji_spx4_i2s {
	void __iomem *base;
	u32 mode_word;
};

static void spx4_i2s_soft_reset(struct fuji_spx4_i2s *i2s)
{
	int tries = 1000;

	writel_relaxed(1, i2s->base + SPX4_I2S_RESET);
	while ((readl_relaxed(i2s->base + SPX4_I2S_RESET) & 1) && --tries > 0)
		cpu_relax();
}

static int spx4_i2s_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *dai)
{
	struct fuji_spx4_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	bool is_tx = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	u32 width_code;

	switch (params_width(params)) {
	case 16:
		width_code = 15;
		break;
	case 24:
		width_code = 23;
		break;
	case 32:
		width_code = 31;
		break;
	default:
		return -EINVAL;
	}

	spx4_i2s_soft_reset(i2s);
	writel_relaxed(i2s->mode_word, i2s->base + SPX4_I2S_MODE);
	writel_relaxed((width_code << 8) | width_code, i2s->base + SPX4_I2S_WIDTH);
	writel_relaxed(0x00000003U, i2s->base + SPX4_I2S_CH_EN);
	writel_relaxed(0x00000000U, i2s->base + SPX4_I2S_EXT);
	writel_relaxed(is_tx ? 0x7D3F0F0FU : 0x7F3D0F0FU, i2s->base + SPX4_I2S_FIFO_CTRL);
	writel_relaxed(is_tx ? 0x01000000U : 0x00000100U, i2s->base + SPX4_I2S_DMA_EN);
	return 0;
}

static int spx4_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
			    struct snd_soc_dai *dai)
{
	struct fuji_spx4_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	bool is_tx = (substream->stream == SNDRV_PCM_STREAM_PLAYBACK);
	u32 bit = is_tx ? 0x00010000U : 0x01000000U;
	u32 val = readl_relaxed(i2s->base + SPX4_I2S_START);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		writel_relaxed(val | bit, i2s->base + SPX4_I2S_START);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		writel_relaxed(val & ~bit, i2s->base + SPX4_I2S_START);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct snd_soc_dai_ops spx4_i2s_dai_ops = {
	.hw_params	= spx4_i2s_hw_params,
	.trigger	= spx4_i2s_trigger,
};

static struct snd_soc_dai_driver spx4_i2s_dai = {
	.name = "fuji-spx4-i2s",
	.playback = {
		.stream_name = "I2S Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,
	},
	.capture = {
		.stream_name = "I2S Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
	.ops = &spx4_i2s_dai_ops,
	.symmetric_rate = 1,
};

static const struct snd_soc_component_driver spx4_i2s_component = {
	.name = "fuji-spx4-i2s",
};

static int fuji_spx4_i2s_probe(struct platform_device *pdev)
{
	struct fuji_spx4_i2s *i2s;
	int ret;

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	i2s->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2s->base))
		return PTR_ERR(i2s->base);

	i2s->mode_word = 0x1000254BU;
	of_property_read_u32(pdev->dev.of_node, "fujisan,i2s-mode-word", &i2s->mode_word);

	platform_set_drvdata(pdev, i2s);
	ret = devm_snd_soc_register_component(&pdev->dev, &spx4_i2s_component,
					      &spx4_i2s_dai, 1);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Fujisan SPX4 I2S DAI registered (mode=0x%08x)\n", i2s->mode_word);
	return 0;
}

static const struct of_device_id fuji_spx4_i2s_of_match[] = {
	{ .compatible = "fujisan,spx4-i2s" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_i2s_of_match);

static struct platform_driver fuji_spx4_i2s_driver = {
	.probe = fuji_spx4_i2s_probe,
	.driver = {
		.name = "fuji-spx4-i2s",
		.of_match_table = fuji_spx4_i2s_of_match,
	},
};
module_platform_driver(fuji_spx4_i2s_driver);

MODULE_DESCRIPTION("Fujisan SPX4 Dual-Channel I2S/PCM ALSA SoC CPU DAI driver");
MODULE_LICENSE("GPL");
