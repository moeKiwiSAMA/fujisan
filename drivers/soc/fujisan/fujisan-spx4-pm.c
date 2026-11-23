// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan OS — SPX4 4-Layer Imaging Power-Domain & Clock Controller
 *
 * DAT verified (SPX4-MF 2.13 S4 0x011D9280..0x011D9E50, 0x011F6038..0x011F81D0):
 *   Layer 1: Keyed Power Domains 0xFFF42000 (18 domains d=0..17, 0x0000AA01 ON / 0x0000AA02 OFF)
 *            PD 2 (CMOS/SLVS-EC/CCDIF), PD 4 (LCDC0/1), PD 6/7/8 (IIFP),
 *            PD 9 (UniResz/UniJpeg/Still), PD 10 (Mali-V61), PD 13 (RECC)
 *   Layer 2: PLL & Divider bank 0xFF701000..0xFF70C080 (+0xF0 busy poll)
 *   Layer 3: Peripheral reset/clock gates 0xFFF48300..0xFFF48334 (+ 0xFFF48608)
 *   Layer 4: AXI/Bus clock gates 0xFF70F000..0xFF70F03C
 *
 * Strict hardware bring-up invariant:
 *   - Power-ON:  PD ON -> PLL/Div -> Layer-3 (0xFFF483xx) FIRST -> Layer-4 (0xFF70F0xx)
 *   - Power-OFF: Layer-4 (0xFF70F0xx) FIRST -> Layer-3 (0xFFF483xx) -> PD OFF
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>

#include "fujisan_spx4_hw.h"

struct fujisan_spx4_pm;

struct fujisan_pm_genpd {
	struct generic_pm_domain genpd;
	struct fujisan_spx4_pm *pm;
	struct fujisan_pm_domain_desc desc;
};

struct fujisan_spx4_pm {
	struct device *dev;
	void __iomem *keyed_io;		/* 0xFFF42000 */
	void __iomem *rstclk;		/* 0xFFF48300..0xFFF486FF */
	void __iomem *axiclk;		/* 0xFF70F000..0xFF70F0FF */
	void __iomem *sysbridge;	/* 0xFFF49500..0xFFF497FF */
	struct mutex lock;
	struct genpd_onecell_data onecell;
	struct generic_pm_domain *domains[FUJISAN_PM_NUM_DOMAINS];
};

static int fujisan_pm_keyed_commit(struct fujisan_spx4_pm *pm, uint32_t pd_mask, bool power_on)
{
	uint32_t active, en, dis, stat;
	uint32_t cmd = power_on ? SPX4_KEYED_IO_CMD_KEY1 : SPX4_KEYED_IO_CMD_KEY2;
	uint32_t done_bit = power_on ? SPX4_KEYED_IO_STAT_ON_DONE : SPX4_KEYED_IO_STAT_OFF_DONE;
	int ret;

	active = readl(pm->keyed_io + SPX4_KEYED_IO_ACTIVE);
	if (power_on && (active & pd_mask) == pd_mask)
		return 0;
	if (!power_on && (active & pd_mask) == 0U)
		return 0;

	en = readl(pm->keyed_io + SPX4_KEYED_IO_ENABLE);
	dis = readl(pm->keyed_io + SPX4_KEYED_IO_DISABLE);
	if (power_on) {
		en |= pd_mask;
		dis &= ~pd_mask;
	} else {
		dis |= pd_mask;
		en &= ~pd_mask;
	}
	writel(en, pm->keyed_io + SPX4_KEYED_IO_ENABLE);
	writel(dis, pm->keyed_io + SPX4_KEYED_IO_DISABLE);
	writel(done_bit, pm->keyed_io + SPX4_KEYED_IO_STATUS_CLR);
	writel(cmd, pm->keyed_io + SPX4_KEYED_IO_COMMAND);

	ret = readl_poll_timeout_atomic(pm->keyed_io + SPX4_KEYED_IO_STATUS,
					stat, (stat & done_bit) != 0U,
					10, 50000);
	writel(done_bit, pm->keyed_io + SPX4_KEYED_IO_STATUS_CLR);
	return ret;
}

static void fujisan_pm_set_bits(void __iomem *reg, uint32_t mask, bool enable)
{
	uint32_t v;

	if (!mask)
		return;
	v = readl(reg);
	if (enable)
		v |= mask;
	else
		v &= ~mask;
	writel(v, reg);
}

static int fujisan_pm_domain_power_on(struct generic_pm_domain *genpd)
{
	struct fujisan_pm_genpd *fpd = container_of(genpd, struct fujisan_pm_genpd, genpd);
	struct fujisan_spx4_pm *pm = fpd->pm;
	const struct fujisan_pm_domain_desc *d = &fpd->desc;
	int ret;

	mutex_lock(&pm->lock);

	/* Layer 1: Keyed Power Domain ON (0xFFF42000 = 0x0000AA01) */
	ret = fujisan_pm_keyed_commit(pm, d->pd_mask, true);
	if (ret) {
		dev_err(pm->dev, "PD %u (%s) power-on timeout: %d\n", d->domain_id, d->name, ret);
		goto out;
	}

	/* Layer 3 BEFORE Layer 4: Parent gates */
	fujisan_pm_set_bits(pm->rstclk + d->rstclk_off, d->rstclk_parent, true);
	fujisan_pm_set_bits(pm->axiclk + d->axiclk_off, d->axiclk_parent, true);

	/* Layer 3 BEFORE Layer 4: Staged sub-engine gates */
	fujisan_pm_set_bits(pm->rstclk + d->rstclk_off, d->rstclk_sub, true);
	fujisan_pm_set_bits(pm->axiclk + d->axiclk_off, d->axiclk_sub, true);

	/* CMOS/SLVS-EC interconnect bridge (S5 0x02394E88..0x02394F04 DAT) */
	if (d->domain_id == FUJISAN_PD_CMOS_SLVSEC_CCDIF && pm->sysbridge) {
		fujisan_pm_set_bits(pm->rstclk + FUJISAN_RSTCLK_EXT_CMOS_OFF, 0x2U, true);
		writel(0x10000704U, pm->sysbridge + FUJISAN_SYSBRIDGE_CFG0_OFF);
		writel(0x00000070U, pm->sysbridge + FUJISAN_SYSBRIDGE_CFG1_OFF);
		writel(0x00000071U, pm->sysbridge + FUJISAN_SYSBRIDGE_CFG1_OFF);
		writel(0x00000003U, pm->sysbridge + FUJISAN_SYSBRIDGE_CTRL_OFF);
	}

out:
	mutex_unlock(&pm->lock);
	return ret;
}

static int fujisan_pm_domain_power_off(struct generic_pm_domain *genpd)
{
	struct fujisan_pm_genpd *fpd = container_of(genpd, struct fujisan_pm_genpd, genpd);
	struct fujisan_spx4_pm *pm = fpd->pm;
	const struct fujisan_pm_domain_desc *d = &fpd->desc;
	int ret;

	mutex_lock(&pm->lock);

	/* Strict reverse order: Layer 4 (0xFF70F0xx) BEFORE Layer 3 (0xFFF483xx) */
	fujisan_pm_set_bits(pm->axiclk + d->axiclk_off, d->axiclk_sub, false);
	fujisan_pm_set_bits(pm->rstclk + d->rstclk_off, d->rstclk_sub, false);

	fujisan_pm_set_bits(pm->axiclk + d->axiclk_off, d->axiclk_parent, false);
	fujisan_pm_set_bits(pm->rstclk + d->rstclk_off, d->rstclk_parent, false);

	/* Layer 1: Keyed Power Domain OFF (0xFFF42000 = 0x0000AA02) */
	ret = fujisan_pm_keyed_commit(pm, d->pd_mask, false);

	mutex_unlock(&pm->lock);
	return ret;
}

static int fujisan_spx4_pm_probe(struct platform_device *pdev)
{
	struct fujisan_spx4_pm *pm;
	unsigned int i;
	int ret;

	pm = devm_kzalloc(&pdev->dev, sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;

	pm->dev = &pdev->dev;
	mutex_init(&pm->lock);

	pm->keyed_io = devm_platform_ioremap_resource_byname(pdev, "keyed-io");
	if (IS_ERR(pm->keyed_io))
		return PTR_ERR(pm->keyed_io);

	pm->rstclk = devm_platform_ioremap_resource_byname(pdev, "rstclk");
	if (IS_ERR(pm->rstclk))
		return PTR_ERR(pm->rstclk);

	pm->axiclk = devm_platform_ioremap_resource_byname(pdev, "axiclk");
	if (IS_ERR(pm->axiclk))
		return PTR_ERR(pm->axiclk);

	pm->sysbridge = devm_platform_ioremap_resource_byname(pdev, "sysbridge");
	if (IS_ERR(pm->sysbridge))
		pm->sysbridge = NULL;

	for (i = 0; i < FUJISAN_PM_NUM_DOMAINS; i++) {
		struct fujisan_pm_domain_desc desc;
		struct fujisan_pm_genpd *fpd;

		if (!fujisan_pm_get_domain_desc((uint8_t)i, &desc))
			continue;

		fpd = devm_kzalloc(&pdev->dev, sizeof(*fpd), GFP_KERNEL);
		if (!fpd)
			return -ENOMEM;

		fpd->pm = pm;
		fpd->desc = desc;
		fpd->genpd.name = desc.name;
		fpd->genpd.power_on = fujisan_pm_domain_power_on;
		fpd->genpd.power_off = fujisan_pm_domain_power_off;

		pm_genpd_init(&fpd->genpd, NULL, true);
		pm->domains[i] = &fpd->genpd;
	}

	pm->onecell.domains = pm->domains;
	pm->onecell.num_domains = FUJISAN_PM_NUM_DOMAINS;

	ret = of_genpd_add_provider_onecell(pdev->dev.of_node, &pm->onecell);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, pm);
	dev_info(&pdev->dev, "Fujisan SPX4 4-layer PM/Clock controller registered (PD 2/4/6-8/9/10/13)\n");
	return 0;
}

static const struct of_device_id fujisan_spx4_pm_of_match[] = {
	{ .compatible = "fujisan,spx4-pm" },
	{ }
};
MODULE_DEVICE_TABLE(of, fujisan_spx4_pm_of_match);

static struct platform_driver fujisan_spx4_pm_driver = {
	.probe = fujisan_spx4_pm_probe,
	.driver = {
		.name = "fujisan-spx4-pm",
		.of_match_table = fujisan_spx4_pm_of_match,
	},
};
module_platform_driver(fujisan_spx4_pm_driver);

MODULE_AUTHOR("Fujisan OS Contributors");
MODULE_DESCRIPTION("Fujisan OS SPX4 4-Layer Imaging Power-Domain & Clock Controller");
MODULE_LICENSE("GPL");
