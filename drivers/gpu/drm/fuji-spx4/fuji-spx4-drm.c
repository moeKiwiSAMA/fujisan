// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan SPX4 DRM/KMS Display Driver (fuji-spx4-drm)
 *   - Primary Output: Rear LCD LCDC1 @ 0xFFFA9000 (1024x768@60, GIC SPI 118 VSYNC)
 *   - Secondary Output: EVF LCDC0 @ 0xFFF7A000 (GIC SPI 112/113, strictly clock-gated)
 *   - Display Clock / Mode-Word Gate: 0xFFF49700 (DAT @ section4.bin 0x012556BC..0x012558D0)
 *
 * Wave-3 Hardware Safety Rules (DAT verified):
 *   1. Ungate 0xFFF49700 bits[9:8] (mode 1/2/3 => bit 9 = 0x200) BEFORE accessing LCDC1.
 *   2. NEVER read/write LCDC0 (0xFFF7A000) unless evf_active is explicitly enabled AND
 *      0xFFF49700 has gated EVF mode ON.
 *   3. LCDC1 +0x008 MUST RMW preserving bit 12 (0x00001000 active output gate!) + bit 16
 *      (0x00010000 OSD0); clearing bit 12 fades the retail TFT gray!
 *   4. ALWAYS write +0x040 = 0 alongside +0x044 = fb_pa.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#include "fuji_spx4_hw.h"

extern void fuji_spx4_timer_set_lcdc_heartbeat(bool enable);

#define LCDC_WIDTH		1024
#define LCDC_HEIGHT		768
#define LCDC_STRIDE		(LCDC_WIDTH * 4)
#define LCDC_FB_BYTES		(LCDC_STRIDE * LCDC_HEIGHT)

#define LCDC_REG_RUN		0x004
#define LCDC_REG_PLANE_EN	0x008
#define LCDC_REG_OSD0_CTRL	0x030
#define LCDC_REG_OSD0_ADDR_H	0x040
#define LCDC_REG_OSD0_ADDR_L	0x044
#define LCDC_REG_OSD0_PITCH	0x050
#define LCDC_REG_OSD0_SIZE	0x054
#define LCDC_REG_OSD0_POS	0x058
#define LCDC_REG_OSD0_WIN	0x05C
#define LCDC_REG_OSD0_SCALE	0x060
#define LCDC_REG_COMMIT		0x600

#define LCDC_OSD0_PITCH_VAL	0x00010400U
#define LCDC_OSD0_EXTENT	((LCDC_HEIGHT << 16) | LCDC_WIDTH)
#define LCDC_OSD0_UNIT_SCALE	0x10001000U

struct fuji_spx4_drm {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector conn;
	void __iomem *lcdc1_base;
	void __iomem *lcdc0_base;
	void __iomem *clk_gate_base;
	void *xrgb_shadow_vaddr;
	dma_addr_t xrgb_shadow_dma;
	atomic64_t vsync_count;
	int vsync_irq;
	bool evf_active;
	bool lcdc0_ungated;
	bool hw_committed;
};

static inline struct fuji_spx4_drm *to_spx4_drm(struct drm_device *drm)
{
	return container_of(drm, struct fuji_spx4_drm, drm);
}

static void spx4_lcdc_set_clk_gate(struct fuji_spx4_drm *spx, u32 mode)
{
	u32 cur, next;

	if (!spx->clk_gate_base)
		return;

	cur = readl_relaxed(spx->clk_gate_base);
	next = spx4_lcdc_clk_gate_rmw(cur, mode);
	writel_relaxed(next, spx->clk_gate_base);
	spx->lcdc0_ungated = (next & SPX4_LCDC_CLK_MODE_EVF) != 0;
}

static void spx4_lcdc_commit_addr(void __iomem *base, dma_addr_t pa, bool first_init)
{
	/*
	 * Always enforce bit 12 (0x1000 active output gate) + bit 16 (OSD0) via RMW
	 * so retail TFT never fades gray (DAT @ section4.bin 0x01269380..0x0126940C).
	 */
	u32 en = readl_relaxed(base + LCDC_REG_PLANE_EN);

	en = spx4_lcdc_plane_en_rmw(en, true);
	writel_relaxed(en, base + LCDC_REG_PLANE_EN);

	if (first_init) {
		writel_relaxed(SPX4_LCDC_OSD0_CTRL_OPAQUE, base + LCDC_REG_OSD0_CTRL);
		writel_relaxed(LCDC_OSD0_PITCH_VAL, base + LCDC_REG_OSD0_PITCH);
		writel_relaxed(LCDC_OSD0_EXTENT, base + LCDC_REG_OSD0_SIZE);
		writel_relaxed(0x00000000U, base + LCDC_REG_OSD0_POS);
		writel_relaxed(LCDC_OSD0_EXTENT, base + LCDC_REG_OSD0_WIN);
		writel_relaxed(LCDC_OSD0_UNIT_SCALE, base + LCDC_REG_OSD0_SCALE);
		writel_relaxed(1U, base + LCDC_REG_RUN);
	}
	writel_relaxed(0U, base + LCDC_REG_OSD0_ADDR_H);
	writel_relaxed((u32)pa, base + LCDC_REG_OSD0_ADDR_L);
	writel_relaxed(LCDC_OSD0_UNIT_SCALE, base + LCDC_REG_OSD0_SCALE);
	writel_relaxed(1U, base + LCDC_REG_COMMIT);
}

static void spx4_swizzle_xrgb_rows(u32 *dst, const u32 *src,
				   unsigned int y1, unsigned int y2)
{
	size_t start, end, i;

	if (y1 >= LCDC_HEIGHT)
		return;
	if (y2 > LCDC_HEIGHT)
		y2 = LCDC_HEIGHT;
	if (y2 <= y1)
		return;

	start = (size_t)y1 * LCDC_WIDTH;
	end = (size_t)y2 * LCDC_WIDTH;
	for (i = start; i < end; i++)
		dst[i] = spx4_xrgb8888_to_lcdc_rgba(src[i]);
}

static irqreturn_t spx4_drm_vsync_irq(int irq, void *dev_id)
{
	struct fuji_spx4_drm *spx = dev_id;

	atomic64_inc(&spx->vsync_count);
	return IRQ_HANDLED;
}

static int spx4_conn_get_modes(struct drm_connector *connector)
{
	static const struct drm_display_mode spx4_mf_lcd_mode = {
		DRM_MODE("1024x768", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
			 65000, 1024, 1048, 1184, 1344, 0,
			 768, 771, 777, 806, 0,
			 DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC),
	};
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &spx4_mf_lcd_mode);
	if (!mode)
		return 0;

	drm_mode_probed_add(connector, mode);
	/* 3.2" 4:3 rear LCD physical size: ~65mm x 49mm (~400 PPI) */
	connector->display_info.width_mm = 65;
	connector->display_info.height_mm = 49;
	return 1;
}

static const struct drm_connector_helper_funcs spx4_conn_helper_funcs = {
	.get_modes = spx4_conn_get_modes,
};

static const struct drm_connector_funcs spx4_conn_funcs = {
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static void spx4_pipe_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct fuji_spx4_drm *spx = to_spx4_drm(pipe->crtc.dev);

	spx4_lcdc_set_clk_gate(spx, spx->evf_active ? 5U : 1U);
	spx->hw_committed = false;
}

static void spx4_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct fuji_spx4_drm *spx = to_spx4_drm(pipe->crtc.dev);

	spx->hw_committed = false;
}

static void spx4_pipe_update(struct drm_simple_display_pipe *pipe,
			     struct drm_plane_state *old_state)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct fuji_spx4_drm *spx = to_spx4_drm(crtc->dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_pending_vblank_event *event = crtc->state->event;
	struct drm_rect clip;
	unsigned long flags;

	if (fb) {
		struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, 0);
		dma_addr_t scanout_pa = gem->dma_addr + fb->offsets[0];

		if (fb->format->format == DRM_FORMAT_XRGB8888 && spx->xrgb_shadow_vaddr) {
			if (!spx->hw_committed || !old_state || old_state->fb != fb) {
				spx4_swizzle_xrgb_rows(spx->xrgb_shadow_vaddr,
						       gem->vaddr + fb->offsets[0],
						       0, LCDC_HEIGHT);
			} else if (drm_atomic_helper_damage_merged(old_state, state, &clip)) {
				spx4_swizzle_xrgb_rows(spx->xrgb_shadow_vaddr,
						       gem->vaddr + fb->offsets[0],
						       clip.y1, clip.y2);
			}
			scanout_pa = spx->xrgb_shadow_dma;
		}

		spx4_lcdc_commit_addr(spx->lcdc1_base, scanout_pa, !spx->hw_committed);
		/* Strict clock-gate guard: NEVER touch LCDC0 unless gated ON via 0xFFF49700 */
		if (spx->evf_active && spx->lcdc0_base && spx->lcdc0_ungated)
			spx4_lcdc_commit_addr(spx->lcdc0_base, scanout_pa, !spx->hw_committed);
		spx->hw_committed = true;
	}

	if (event) {
		crtc->state->event = NULL;
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, event);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static const struct drm_simple_display_pipe_funcs spx4_pipe_funcs = {
	.enable		= spx4_pipe_enable,
	.disable	= spx4_pipe_disable,
	.update		= spx4_pipe_update,
};

static const u32 spx4_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGBA8888,
};

static const struct drm_mode_config_funcs spx4_mode_config_funcs = {
	.fb_create	= drm_gem_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(spx4_drm_fops);

static const struct drm_driver fuji_spx4_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &spx4_drm_fops,
	DRM_GEM_DMA_DRIVER_OPS_VMAP,
	.name			= "fuji-spx4-drm",
	.desc			= "SPX4-MF Medium-Format Camera SPX4 LCDC1/LCDC0 DRM/KMS",
	.date			= "20261122",
	.major			= 1,
	.minor			= 1,
};

static ssize_t evf_active_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fuji_spx4_drm *spx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", spx->evf_active ? 1 : 0);
}

static ssize_t evf_active_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fuji_spx4_drm *spx = dev_get_drvdata(dev);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;
	spx->evf_active = val;
	spx4_lcdc_set_clk_gate(spx, val ? 5U : 1U);
	return count;
}
static DEVICE_ATTR_RW(evf_active);

static struct attribute *spx4_drm_attrs[] = {
	&dev_attr_evf_active.attr,
	NULL,
};
ATTRIBUTE_GROUPS(spx4_drm);

static int fuji_spx4_drm_probe(struct platform_device *pdev)
{
	struct fuji_spx4_drm *spx;
	struct drm_device *drm;
	struct resource *res;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	spx = devm_drm_dev_alloc(&pdev->dev, &fuji_spx4_drm_driver,
				 struct fuji_spx4_drm, drm);
	if (IS_ERR(spx))
		return PTR_ERR(spx);

	drm = &spx->drm;
	atomic64_set(&spx->vsync_count, 0);

	/* Map display clock/mode-word gate (0xFFF49700) first & ungate LCDC1 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (res) {
		spx->clk_gate_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(spx->clk_gate_base))
			spx->clk_gate_base = NULL;
	}
	spx4_lcdc_set_clk_gate(spx, 1U);

	spx->lcdc1_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spx->lcdc1_base))
		return PTR_ERR(spx->lcdc1_base);

	/* Optional LCDC0 EVF MMIO window (mapped but NEVER accessed while clock-gated) */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res)
		spx->lcdc0_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(spx->lcdc0_base))
		spx->lcdc0_base = NULL;

	spx->xrgb_shadow_vaddr = dmam_alloc_coherent(&pdev->dev, LCDC_FB_BYTES,
						     &spx->xrgb_shadow_dma, GFP_KERNEL);
	if (!spx->xrgb_shadow_vaddr)
		dev_warn(&pdev->dev, "CMA shadow alloc failed; only native RGBA8888 scanout active\n");

	/* Take over cleanly from simplefb and timer LCDC heartbeat */
	ret = drm_aperture_remove_framebuffers(&fuji_spx4_drm_driver);
	if (ret)
		return ret;
	fuji_spx4_timer_set_lcdc_heartbeat(false);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 64;
	drm->mode_config.max_width = 1920;
	drm->mode_config.min_height = 64;
	drm->mode_config.max_height = 1200;
	drm->mode_config.funcs = &spx4_mode_config_funcs;

	drm_connector_helper_add(&spx->conn, &spx4_conn_helper_funcs);
	ret = drm_connector_init(drm, &spx->conn, &spx4_conn_funcs,
				 DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &spx->pipe, &spx4_pipe_funcs,
					   spx4_formats, ARRAY_SIZE(spx4_formats),
					   NULL, &spx->conn);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&spx->pipe.plane);

	spx->vsync_irq = platform_get_irq_optional(pdev, 0);
	if (spx->vsync_irq > 0) {
		ret = devm_request_irq(&pdev->dev, spx->vsync_irq, spx4_drm_vsync_irq,
				       IRQF_SHARED, "fuji-spx4-lcdc1-vsync", spx);
		if (ret)
			dev_warn(&pdev->dev, "VSYNC IRQ %d request failed (%d); continuing without IRQ\n",
				 spx->vsync_irq, ret);
	}

	drm_mode_config_reset(drm);
	platform_set_drvdata(pdev, spx);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_fbdev_generic_setup(drm, 32);

	dev_info(&pdev->dev,
		 "Fujisan SPX4 DRM/KMS registered (LCDC1 1024x768, clk_gate=%s, vsync_irq=%d, shadow_pa=%pad)\n",
		 spx->clk_gate_base ? "0xFFF49700" : "none",
		 spx->vsync_irq, &spx->xrgb_shadow_dma);
	return 0;
}

static int fuji_spx4_drm_remove(struct platform_device *pdev)
{
	struct fuji_spx4_drm *spx = platform_get_drvdata(pdev);

	drm_dev_unregister(&spx->drm);
	drm_atomic_helper_shutdown(&spx->drm);
	return 0;
}

static const struct of_device_id fuji_spx4_drm_of_match[] = {
	{ .compatible = "fujisan,spx4-drm" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_drm_of_match);

static struct platform_driver fuji_spx4_drm_driver_plat = {
	.probe	= fuji_spx4_drm_probe,
	.remove	= fuji_spx4_drm_remove,
	.driver	= {
		.name		= "fuji-spx4-drm",
		.of_match_table	= fuji_spx4_drm_of_match,
		.dev_groups	= spx4_drm_groups,
	},
};
module_platform_driver(fuji_spx4_drm_driver_plat);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera SPX4 LCDC1/LCDC0 DRM/KMS driver");
MODULE_LICENSE("GPL");
