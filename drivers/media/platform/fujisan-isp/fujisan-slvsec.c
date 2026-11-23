// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan OS — Dual-Bank 8-Lane SLVS-EC RX + SerDes PHY + CCDTG Timing Generator
 *
 * DAT verified (SPX4-MF 2.13 CMOS-RE-1):
 *   - SLVS-EC RX Bank 0: 0xFFF69000..0xFFF69250 (Global + Ch0 +0x100 + Ch1 +0x200)
 *   - SLVS-EC RX Bank 1: 0xFFF6A000..0xFFF6A250 (mirror bank)
 *   - SLVS-EC PHY:       0xFFF6B004..0xFFF6B0F4
 *   - CCDTG Timing Gen:  0xFFF6C000..0xFFF6C42C (C004/C008/C00C/C024 + 6 pulse channels)
 *   - GIC IRQs: 296 (Ch0 Normal), 298 (Ch1 Normal), 297 (Shared Error), 100/102 (CCDTG)
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "fujisan_spx4_hw.h"

struct fujisan_slvsec {
	struct device *dev;
	void __iomem *bank0;	/* 0xFFF69000 */
	void __iomem *bank1;	/* 0xFFF6A000 */
	void __iomem *phy;	/* 0xFFF6B000 */
	void __iomem *ccdtg;	/* 0xFFF6C000 */
	atomic_t frame_irqs;
	atomic_t error_irqs;
};

static irqreturn_t slvsec_ch0_irq(int irq, void *dev_id)
{
	struct fujisan_slvsec *rx = dev_id;
	uint32_t st = readl(rx->bank0 + FUJISAN_SLVSEC_CH_BASE(0) + FUJISAN_SLVSEC_CH_NORM_STAT);

	if (!(st & FUJISAN_SLVSEC_NORM_IRQ_MASK))
		return IRQ_NONE;

	writel(st & FUJISAN_SLVSEC_NORM_IRQ_MASK,
	       rx->bank0 + FUJISAN_SLVSEC_CH_BASE(0) + FUJISAN_SLVSEC_CH_NORM_STAT);
	atomic_inc(&rx->frame_irqs);
	return IRQ_HANDLED;
}

static irqreturn_t slvsec_ch1_irq(int irq, void *dev_id)
{
	struct fujisan_slvsec *rx = dev_id;
	uint32_t st = readl(rx->bank0 + FUJISAN_SLVSEC_CH_BASE(1) + FUJISAN_SLVSEC_CH_NORM_STAT);

	if (!(st & FUJISAN_SLVSEC_NORM_IRQ_MASK))
		return IRQ_NONE;

	writel(st & FUJISAN_SLVSEC_NORM_IRQ_MASK,
	       rx->bank0 + FUJISAN_SLVSEC_CH_BASE(1) + FUJISAN_SLVSEC_CH_NORM_STAT);
	atomic_inc(&rx->frame_irqs);
	return IRQ_HANDLED;
}

static irqreturn_t slvsec_err_irq(int irq, void *dev_id)
{
	struct fujisan_slvsec *rx = dev_id;
	uint32_t pcs, link, mac, fifo;
	unsigned int ch;

	pcs = readl(rx->bank0 + FUJISAN_SLVSEC_GLB_PCS_ERR);
	if (pcs)
		writel(pcs, rx->bank0 + FUJISAN_SLVSEC_GLB_PCS_ERR);

	for (ch = 0; ch < 2; ch++) {
		void __iomem *c = rx->bank0 + FUJISAN_SLVSEC_CH_BASE(ch);

		link = readl(c + FUJISAN_SLVSEC_CH_LINK_ERR) & FUJISAN_SLVSEC_LINK_ERR_MASK;
		mac = readl(c + FUJISAN_SLVSEC_CH_MAC_ERR) & FUJISAN_SLVSEC_MAC_ERR_MASK;
		fifo = readl(c + FUJISAN_SLVSEC_CH_FIFO_ERR) & 0xFFFFU;
		if (link)
			writel(link, c + FUJISAN_SLVSEC_CH_LINK_ERR);
		if (mac)
			writel(mac, c + FUJISAN_SLVSEC_CH_MAC_ERR);
		if (fifo)
			writel(fifo, c + FUJISAN_SLVSEC_CH_FIFO_ERR);
	}

	atomic_inc(&rx->error_irqs);
	return IRQ_HANDLED;
}

int fujisan_slvsec_configure(struct fujisan_slvsec *rx,
			     const struct fujisan_imx461_mode_desc *mode,
			     uint8_t active_lane_mask)
{
	uint32_t pdn = (uint32_t)fujisan_slvsec_lane_pdn_mask(active_lane_mask);
	uint32_t bpp_code = (mode->bpp == 16U) ? 3U : (mode->bpp == 14U ? 2U : 1U);

	/* Program CCDTG route (C024), bit-depth/sync (C008), H/V timing (C00C) */
	writel(fujisan_ccdtg_route_word(mode->ccdtg_b4, mode->ccdtg_b5),
	       rx->ccdtg + FUJISAN_CCDTG_ROUTE);
	writel((bpp_code << 2) | bpp_code, rx->ccdtg + FUJISAN_CCDTG_FMT_SYNC);
	writel(fujisan_ccdtg_hv_word(mode->width, mode->height),
	       rx->ccdtg + FUJISAN_CCDTG_HV_TIMING);

	/* Enable active-low 8 lanes on both Bank 0 & Bank 1 (0=enabled, 1=powered down) */
	writel((readl(rx->bank0 + FUJISAN_SLVSEC_GLB_LANEPDN) & ~0xFFU) | pdn,
	       rx->bank0 + FUJISAN_SLVSEC_GLB_LANEPDN);
	writel((readl(rx->bank1 + FUJISAN_SLVSEC_GLB_LANEPDN) & ~0xFFU) | pdn,
	       rx->bank1 + FUJISAN_SLVSEC_GLB_LANEPDN);

	writel(active_lane_mask, rx->bank0 + FUJISAN_SLVSEC_CH_BASE(0) + FUJISAN_SLVSEC_CH_LANE_MASK);
	writel(FUJISAN_SLVSEC_NORM_IRQ_MASK,
	       rx->bank0 + FUJISAN_SLVSEC_CH_BASE(0) + FUJISAN_SLVSEC_CH_NORM_EN);
	writel(FUJISAN_SLVSEC_LINK_ERR_MASK,
	       rx->bank0 + FUJISAN_SLVSEC_CH_BASE(0) + FUJISAN_SLVSEC_CH_LINK_EN);

	/* Start CCDTG and SLVS-EC RX */
	writel(1U, rx->ccdtg + FUJISAN_CCDTG_ENABLE);
	writel(1U, rx->bank0 + FUJISAN_SLVSEC_CH_BASE(0) + FUJISAN_SLVSEC_CH_ENABLE);
	writel(0x00110011U, rx->bank0 + FUJISAN_SLVSEC_GLB_CTRL);
	return 0;
}
EXPORT_SYMBOL_GPL(fujisan_slvsec_configure);

static int fujisan_slvsec_probe(struct platform_device *pdev)
{
	struct fujisan_slvsec *rx;
	int irq_ch0, irq_ch1, irq_err, ret;

	rx = devm_kzalloc(&pdev->dev, sizeof(*rx), GFP_KERNEL);
	if (!rx)
		return -ENOMEM;

	rx->dev = &pdev->dev;
	rx->bank0 = devm_platform_ioremap_resource_byname(pdev, "bank0");
	if (IS_ERR(rx->bank0))
		return PTR_ERR(rx->bank0);
	rx->bank1 = devm_platform_ioremap_resource_byname(pdev, "bank1");
	if (IS_ERR(rx->bank1))
		return PTR_ERR(rx->bank1);
	rx->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(rx->phy))
		return PTR_ERR(rx->phy);
	rx->ccdtg = devm_platform_ioremap_resource_byname(pdev, "ccdtg");
	if (IS_ERR(rx->ccdtg))
		return PTR_ERR(rx->ccdtg);

	irq_ch0 = platform_get_irq_byname_optional(pdev, "ch0");
	if (irq_ch0 > 0) {
		ret = devm_request_irq(&pdev->dev, irq_ch0, slvsec_ch0_irq, 0,
				       "fujisan-slvsec-ch0", rx);
		if (ret)
			return ret;
	}
	irq_ch1 = platform_get_irq_byname_optional(pdev, "ch1");
	if (irq_ch1 > 0) {
		ret = devm_request_irq(&pdev->dev, irq_ch1, slvsec_ch1_irq, 0,
				       "fujisan-slvsec-ch1", rx);
		if (ret)
			return ret;
	}
	irq_err = platform_get_irq_byname_optional(pdev, "err");
	if (irq_err > 0) {
		ret = devm_request_irq(&pdev->dev, irq_err, slvsec_err_irq, 0,
				       "fujisan-slvsec-err", rx);
		if (ret)
			return ret;
	}

	pm_runtime_enable(&pdev->dev);
	platform_set_drvdata(pdev, rx);
	dev_info(&pdev->dev, "Fujisan Dual-Bank 8-Lane SLVS-EC RX + PHY + CCDTG probed\n");
	return 0;
}

static void fujisan_slvsec_remove(struct platform_device *pdev)
{
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id fujisan_slvsec_of_match[] = {
	{ .compatible = "fujisan,spx4-slvsec" },
	{ }
};
MODULE_DEVICE_TABLE(of, fujisan_slvsec_of_match);

static struct platform_driver fujisan_slvsec_driver = {
	.probe = fujisan_slvsec_probe,
	.remove_new = fujisan_slvsec_remove,
	.driver = {
		.name = "fujisan-slvsec",
		.of_match_table = fujisan_slvsec_of_match,
	},
};
module_platform_driver(fujisan_slvsec_driver);

MODULE_AUTHOR("Fujisan OS Contributors");
MODULE_DESCRIPTION("Fujisan OS Dual-Bank 8-Lane SLVS-EC RX, PHY & CCDTG Driver");
MODULE_LICENSE("GPL");
