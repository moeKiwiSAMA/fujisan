// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI Master Controller Driver for Fujisan SPX4 (soc-spi @ 0xFFF32000..0xFFF38000)
 *   - SPI0 @ 0xFFF32000 (Lens mount)
 *   - SPI1 @ 0xFFF33000 (Power-Peer PMIC / RTC / Power Switch, CS GPIO 12.5 active-HIGH)
 *   - SPI2 @ 0xFFF34000 (BISCOM / system controller)
 *   - SPI4 @ 0xFFF36000 (WM8962 Audio Codec, 16-bit, divider 28, GPIOs 13.3/4/5/7)
 *   - SPI5 @ 0xFFF37000 (Display panel serial interface)
 *
 * Register layout (DAT verified via 0x011D6748 & SpiFactory.kt):
 *   +0x00 CTRL      bits[3:0] = bits_per_word - 1 (7=8bit, 15=16bit), bits[15:8]=factor
 *   +0x04 ENABLE    bit 1 = enable master, bit 3 = CS/mode (0x0A active)
 *   +0x08 DATA      TX write / RX read FIFO (depth 8)
 *   +0x0C STATUS    bit0=TXE, bit1=TXNF, bit2=RXNE, bit3=RXF, bit4=BUSY
 *   +0x10 DIVIDER   even clock divider 2..254
 *   +0x14 CAUSES    IRQ/event status (W1C)
 *   +0x20 FIFO_CLR  bit0=TX clear, bit1=RX clear
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define SPX4_SPI_CTRL		0x00
#define SPX4_SPI_ENABLE		0x04
#define SPX4_SPI_DATA		0x08
#define SPX4_SPI_STATUS		0x0C
#define SPX4_SPI_DIVIDER	0x10
#define SPX4_SPI_CAUSES		0x14
#define SPX4_SPI_FIFO_CLR	0x20

#define SPX4_SPI_ST_RXNE	BIT(2)
#define SPX4_SPI_ST_BUSY	BIT(4)

struct fuji_spx4_spi {
	void __iomem *base;
	u32 bus_clk;
	u32 default_div;
};

static int spx4_spi_transfer_one(struct spi_controller *ctlr,
				 struct spi_device *spi,
				 struct spi_transfer *t)
{
	struct fuji_spx4_spi *s = spi_controller_get_devdata(ctlr);
	u8 bpw = t->bits_per_word ? t->bits_per_word : 8;
	u32 div = s->default_div;
	unsigned int words, i;
	const u8 *tx8 = t->tx_buf;
	u8 *rx8 = t->rx_buf;
	const u16 *tx16 = t->tx_buf;
	u16 *rx16 = t->rx_buf;

	if (bpw != 8 && bpw != 16)
		return -EINVAL;

	if (t->speed_hz && s->bus_clk) {
		div = DIV_ROUND_UP(s->bus_clk, t->speed_hz);
		if (div < 2)
			div = 2;
		if (div > 254)
			div = 254;
		if (div & 1)
			div++;
	}

	/* Disable, clear FIFOs, configure word width & divider, re-enable */
	writel_relaxed(0x00, s->base + SPX4_SPI_ENABLE);
	writel_relaxed(0x03, s->base + SPX4_SPI_FIFO_CLR);
	writel_relaxed((u32)(bpw - 1), s->base + SPX4_SPI_CTRL);
	writel_relaxed(div, s->base + SPX4_SPI_DIVIDER);
	writel_relaxed(0x0A, s->base + SPX4_SPI_ENABLE);

	words = (bpw == 16) ? (t->len / 2) : t->len;
	for (i = 0; i < words; i++) {
		u32 txw = 0, rxw;
		int tries = 5000;

		if (bpw == 16)
			txw = tx16 ? tx16[i] : 0;
		else
			txw = tx8 ? tx8[i] : 0;

		writel_relaxed(txw, s->base + SPX4_SPI_DATA);

		while (!(readl_relaxed(s->base + SPX4_SPI_STATUS) & SPX4_SPI_ST_RXNE)) {
			if (--tries <= 0) {
				writel_relaxed(0x00, s->base + SPX4_SPI_ENABLE);
				return -ETIMEDOUT;
			}
			cpu_relax();
		}

		rxw = readl_relaxed(s->base + SPX4_SPI_DATA);
		if (bpw == 16 && rx16)
			rx16[i] = (u16)rxw;
		else if (bpw == 8 && rx8)
			rx8[i] = (u8)rxw;
	}

	writel_relaxed(0x01, s->base + SPX4_SPI_CAUSES);
	return 0;
}

static int fuji_spx4_spi_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct fuji_spx4_spi *s;
	int ret;

	ctlr = devm_spi_alloc_master(&pdev->dev, sizeof(*s));
	if (!ctlr)
		return -ENOMEM;

	s = spi_controller_get_devdata(ctlr);
	s->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s->base))
		return PTR_ERR(s->base);

	s->bus_clk = 54000000;
	s->default_div = 28;
	of_property_read_u32(pdev->dev.of_node, "clock-frequency", &s->bus_clk);
	of_property_read_u32(pdev->dev.of_node, "fujisan,default-divider", &s->default_div);
	if (s->default_div < 2 || s->default_div > 254)
		s->default_div = 28;
	s->default_div &= ~1U;

	ctlr->dev.of_node = pdev->dev.of_node;
	ctlr->bus_num = of_alias_get_id(pdev->dev.of_node, "spi");
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8) | SPI_BPW_MASK(16);
	ctlr->use_gpio_descriptors = true;
	ctlr->transfer_one = spx4_spi_transfer_one;

	platform_set_drvdata(pdev, ctlr);
	ret = devm_spi_register_controller(&pdev->dev, ctlr);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "Fujisan SPX4 SPI bus %d registered (div=%u)\n",
		 ctlr->bus_num, s->default_div);
	return 0;
}

static const struct of_device_id fuji_spx4_spi_of_match[] = {
	{ .compatible = "fujisan,spx4-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_spi_of_match);

static struct platform_driver fuji_spx4_spi_driver = {
	.probe	= fuji_spx4_spi_probe,
	.driver	= {
		.name		= "fuji-spx4-spi",
		.of_match_table	= fuji_spx4_spi_of_match,
	},
};
module_platform_driver(fuji_spx4_spi_driver);

MODULE_DESCRIPTION("Fujisan SPX4 SPI master controller driver");
MODULE_LICENSE("GPL");
