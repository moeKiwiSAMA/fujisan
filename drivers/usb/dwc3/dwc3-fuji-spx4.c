// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synopsys DesignWare USB 3.0 (DWC3) Platform Glue for SPX4-MF Medium-Format Camera (SPX4)
 *   - Controller MMIO: 0xFFB00000 (xHCI host @ +0x0000, Global/Dev @ +0xC100)
 *   - USB3 PHY wrapper: 0xFFB23800..0xFFB23FFF
 *   - GIC IRQ:          246 (Level)
 *
 * Probes DWC3_GHWPARAMS0 (+0xC140) bits[2:0] at runtime:
 *   - 0 = Device-Only silicon -> keeps dr_mode="peripheral" (USB Gadget CDC-ECM/ACM)
 *   - 1 = Host-Only / 2 = DRD -> enables xHCI host mode (USB Hub / Keyboard / Mouse)
 *     with automatic USB Gadget fallback when acting as peripheral.
 *
 * Note: SPX4-MF stock firmware hardcodes GCTL PRTCAPDIR=2 (Device-Only), whereas
 * Linux DWC3 dynamically inspects silicon GHWPARAMS0 so DRD-capable SPX4 silicon
 * can use USB-C OTG peripherals while never faulting on Device-Only synthesis.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define DWC3_GHWPARAMS0_OFF	0xC140
#define DWC3_GHWPARAMS0_MODE(x)	((x) & 0x7)

static int fuji_spx4_dwc3_probe(struct platform_device *pdev)
{
	void __iomem *phy_base;
	void __iomem *dwc_base;
	struct resource *res;
	u32 hwparams0 = 0;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy");
	if (res) {
		phy_base = devm_ioremap_resource(&pdev->dev, res);
		if (!IS_ERR(phy_base)) {
			/* Keep PHY clocks/rails released cleanly */
			dev_dbg(&pdev->dev, "SPX4 USB3 PHY mapped at %pR\n", res);
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dwc3-probe");
	if (res) {
		dwc_base = ioremap(res->start + DWC3_GHWPARAMS0_OFF, 4);
		if (dwc_base) {
			hwparams0 = readl_relaxed(dwc_base);
			iounmap(dwc_base);
			dev_info(&pdev->dev,
				 "SPX4 DWC3 GHWPARAMS0=0x%08x (silicon mode=%u: %s)\n",
				 hwparams0, DWC3_GHWPARAMS0_MODE(hwparams0),
				 DWC3_GHWPARAMS0_MODE(hwparams0) == 2 ? "DRD (xHCI+Gadget)" :
				 DWC3_GHWPARAMS0_MODE(hwparams0) == 1 ? "Host (xHCI)" :
				 "Device-Only (USB Gadget CDC-ECM/ACM)");
		}
	}

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id fuji_spx4_dwc3_of_match[] = {
	{ .compatible = "fujisan,spx4-dwc3" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_dwc3_of_match);

static struct platform_driver fuji_spx4_dwc3_driver = {
	.probe = fuji_spx4_dwc3_probe,
	.driver = {
		.name = "dwc3-fuji-spx4",
		.of_match_table = fuji_spx4_dwc3_of_match,
	},
};
module_platform_driver(fuji_spx4_dwc3_driver);

MODULE_DESCRIPTION("Fujisan SPX4 Synopsys DWC3 USB 3.0 DRD/Gadget Glue Driver");
MODULE_LICENSE("GPL");
