// SPDX-License-Identifier: GPL-2.0-only
/*
 * SDHCI Platform Glue Driver for Fujisan SPX4 SDXC Slots & BCM43438A1 SDIO
 *   - SD Slot 1:   0xFFFC4000 (CD GPIO 10.4 active-LOW, addr-hi 0xFFF4A888, GIC 224/230)
 *   - SD Slot 2:   0xFFFC8000 (CD GPIO 10.8 active-LOW, addr-hi 0xFFF4A894, GIC 225/235)
 *   - Wi-Fi SDIO:  0xFFFC0000 (BCM43438A1 brcmfmac, non-removable)
 *
 * Strictly uses 32-bit aligned SDHCI MMIO accesses (SDHCI_QUIRK_32BIT_DMA_ADDR
 * + 32-bit reg wrappers) and programs the DAT address-high sideband word to 0
 * for 32-bit DDR SDMA transfers.
 */

#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "sdhci-pltfm.h"

struct fuji_spx4_sdhci {
	void __iomem *addr_hi_reg;
};

static void spx4_sdhci_writel(struct sdhci_host *host, u32 val, int reg)
{
	struct sdhci_pltfm_host *pltfm = sdhci_priv(host);
	struct fuji_spx4_sdhci *priv = sdhci_pltfm_priv(pltfm);

	if (reg == SDHCI_DMA_ADDRESS && priv->addr_hi_reg)
		writel_relaxed(0, priv->addr_hi_reg);
	writel_relaxed(val, host->ioaddr + reg);
}

static u32 spx4_sdhci_readl(struct sdhci_host *host, int reg)
{
	return readl_relaxed(host->ioaddr + reg);
}

static u16 spx4_sdhci_readw(struct sdhci_host *host, int reg)
{
	u32 val = readl_relaxed(host->ioaddr + (reg & ~3));

	return (val >> ((reg & 2) * 8)) & 0xFFFF;
}

static u8 spx4_sdhci_readb(struct sdhci_host *host, int reg)
{
	u32 val = readl_relaxed(host->ioaddr + (reg & ~3));

	return (val >> ((reg & 3) * 8)) & 0xFF;
}

static void spx4_sdhci_writew(struct sdhci_host *host, u16 val, int reg)
{
	u32 shift = (reg & 2) * 8;
	u32 old = readl_relaxed(host->ioaddr + (reg & ~3));
	u32 nw = (old & ~(0xFFFFU << shift)) | ((u32)val << shift);

	writel_relaxed(nw, host->ioaddr + (reg & ~3));
}

static void spx4_sdhci_writeb(struct sdhci_host *host, u8 val, int reg)
{
	u32 shift = (reg & 3) * 8;
	u32 old = readl_relaxed(host->ioaddr + (reg & ~3));
	u32 nw = (old & ~(0xFFU << shift)) | ((u32)val << shift);

	writel_relaxed(nw, host->ioaddr + (reg & ~3));
}

static unsigned int spx4_sdhci_get_max_clock(struct sdhci_host *host)
{
	return 50000000;
}

static const struct sdhci_ops spx4_sdhci_ops = {
	.read_l			= spx4_sdhci_readl,
	.read_w			= spx4_sdhci_readw,
	.read_b			= spx4_sdhci_readb,
	.write_l		= spx4_sdhci_writel,
	.write_w		= spx4_sdhci_writew,
	.write_b		= spx4_sdhci_writeb,
	.set_clock		= sdhci_set_clock,
	.get_max_clock		= spx4_sdhci_get_max_clock,
	.set_bus_width		= sdhci_set_bus_width,
	.reset			= sdhci_reset,
	.set_uhs_signaling	= sdhci_set_uhs_signaling,
};

static const struct sdhci_pltfm_data spx4_sdhci_pdata = {
	.ops	= &spx4_sdhci_ops,
	.quirks	= SDHCI_QUIRK_32BIT_DMA_ADDR |
		  SDHCI_QUIRK_32BIT_DMA_SIZE |
		  SDHCI_QUIRK_BROKEN_ADMA |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN |
		  SDHCI_QUIRK_NO_HISPD_BIT,
};

static int fuji_spx4_sdhci_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm;
	struct fuji_spx4_sdhci *priv;
	u32 hi_pa = 0;
	int ret;

	host = sdhci_pltfm_init(pdev, &spx4_sdhci_pdata, sizeof(*priv));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm);

	if (!of_property_read_u32(pdev->dev.of_node, "fujisan,addr-high-reg", &hi_pa) && hi_pa)
		priv->addr_hi_reg = devm_ioremap(&pdev->dev, hi_pa, 4);

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto err_free;

	ret = sdhci_add_host(host);
	if (ret)
		goto err_free;

	dev_info(&pdev->dev, "Fujisan SPX4 SDHCI host registered (addr_hi=0x%08x)\n", hi_pa);
	return 0;

err_free:
	sdhci_pltfm_free(pdev);
	return ret;
}

static const struct of_device_id fuji_spx4_sdhci_of_match[] = {
	{ .compatible = "fujisan,spx4-sdhci" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_sdhci_of_match);

static struct platform_driver fuji_spx4_sdhci_driver = {
	.driver = {
		.name		= "sdhci-fuji-spx4",
		.of_match_table	= fuji_spx4_sdhci_of_match,
	},
	.probe	= fuji_spx4_sdhci_probe,
	.remove_new = sdhci_pltfm_remove,
};
module_platform_driver(fuji_spx4_sdhci_driver);

MODULE_DESCRIPTION("Fujisan SPX4 SDXC / SDIO SDHCI Platform Driver");
MODULE_LICENSE("GPL");
