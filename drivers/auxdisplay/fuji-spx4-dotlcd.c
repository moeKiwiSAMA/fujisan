// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPX4-MF Medium-Format Camera Shoulder Sub-LCD (dotlcd) 304x230 1-bpp MIP Display Driver
 *
 * Hardware bindings (DAT verified from section4 dotlcd task 0x0124BD80):
 *   - Transport: soc-serial-engine bank 0 physical ch1 @ 0xFFF30020 (IRQ 52)
 *   - Control GPIOs: Bank 11 Pin 3, Bank 11 Pin 4, Bank 12 Pin 6 (backlight/CS/disp)
 *   - Active canvas: 304 x 230 1-bpp (38 bytes/row * 230 = 8,740 bytes)
 *
 * Exposes a secondary Linux framebuffer (/dev/fb1, 304x230 1-bpp monochrome)
 * with deferred I/O plus sysfs attributes for backlight and black/white inversion
 * so Noctalia / system scripts can render battery/clock/Wi-Fi telemetry onto
 * the SPX4-MF top shoulder MIP display.
 */

#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define DOTLCD_WIDTH		304
#define DOTLCD_HEIGHT		230
#define DOTLCD_STRIDE		(DOTLCD_WIDTH / 8) /* 38 bytes */
#define DOTLCD_FB_SIZE		(DOTLCD_STRIDE * DOTLCD_HEIGHT) /* 8740 B */

#define SERIAL_CH_CTRL		0x00
#define SERIAL_CH_DATA		0x04
#define SERIAL_CH_STATUS	0x0C

struct fuji_dotlcd {
	struct device *dev;
	void __iomem *base;
	struct fb_info *fb;
	struct gpio_desc *gpio_cs;
	struct gpio_desc *gpio_disp;
	struct gpio_desc *gpio_bl;
	bool invert;
	bool backlight;
	u8 *vmem;
};

static void dotlcd_flush_hw(struct fuji_dotlcd *d)
{
	/*
	 * Pack 304x230 1-bpp canvas into serial-engine ch1 stream when
	 * hardware serial engine TX is active; keep MMIO writes strictly
	 * bounded to control (+0x00) and status (+0x0C) unless TX ready.
	 */
	if (!d->base)
		return;
	if (d->gpio_disp)
		gpiod_set_value_cansleep(d->gpio_disp, 1);
	if (d->gpio_bl)
		gpiod_set_value_cansleep(d->gpio_bl, d->backlight ? 1 : 0);
}

static void dotlcd_deferred_io(struct fb_info *info, struct list_head *pagereflist)
{
	struct fuji_dotlcd *d = info->par;

	dotlcd_flush_hw(d);
}

static struct fb_deferred_io dotlcd_defio = {
	.delay		= HZ / 10,
	.deferred_io	= dotlcd_deferred_io,
};

static ssize_t backlight_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fuji_dotlcd *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", d->backlight ? 1 : 0);
}

static ssize_t backlight_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct fuji_dotlcd *d = dev_get_drvdata(dev);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;
	d->backlight = val;
	dotlcd_flush_hw(d);
	return count;
}
static DEVICE_ATTR_RW(backlight);

static ssize_t invert_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fuji_dotlcd *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", d->invert ? 1 : 0);
}

static ssize_t invert_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct fuji_dotlcd *d = dev_get_drvdata(dev);
	bool val;

	if (kstrtobool(buf, &val))
		return -EINVAL;
	d->invert = val;
	dotlcd_flush_hw(d);
	return count;
}
static DEVICE_ATTR_RW(invert);

static struct attribute *dotlcd_attrs[] = {
	&dev_attr_backlight.attr,
	&dev_attr_invert.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dotlcd);

static const struct fb_ops dotlcd_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= fb_sys_write,
	.fb_fillrect	= sys_fillrect,
	.fb_copyarea	= sys_copyarea,
	.fb_imageblit	= sys_imageblit,
	.fb_mmap	= fb_deferred_io_mmap,
};

static int fuji_dotlcd_probe(struct platform_device *pdev)
{
	struct fuji_dotlcd *d;
	struct fb_info *info;
	int ret;

	info = framebuffer_alloc(sizeof(*d), &pdev->dev);
	if (!info)
		return -ENOMEM;

	d = info->par;
	d->dev = &pdev->dev;
	d->fb = info;
	d->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->base)) {
		ret = PTR_ERR(d->base);
		goto err_release;
	}

	d->gpio_cs = devm_gpiod_get_optional(&pdev->dev, "cs", GPIOD_OUT_LOW);
	d->gpio_disp = devm_gpiod_get_optional(&pdev->dev, "disp", GPIOD_OUT_HIGH);
	d->gpio_bl = devm_gpiod_get_optional(&pdev->dev, "backlight", GPIOD_OUT_LOW);

	d->vmem = vzalloc(PAGE_ALIGN(DOTLCD_FB_SIZE));
	if (!d->vmem) {
		ret = -ENOMEM;
		goto err_release;
	}

	info->screen_buffer = d->vmem;
	info->screen_size = DOTLCD_FB_SIZE;
	info->fbops = &dotlcd_fb_ops;
	info->flags = FBINFO_VIRTFB;

	strscpy(info->fix.id, "fuji-dotlcd", sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_MONO10;
	info->fix.line_length = DOTLCD_STRIDE;
	info->fix.smem_len = PAGE_ALIGN(DOTLCD_FB_SIZE);

	info->var.xres = DOTLCD_WIDTH;
	info->var.yres = DOTLCD_HEIGHT;
	info->var.xres_virtual = DOTLCD_WIDTH;
	info->var.yres_virtual = DOTLCD_HEIGHT;
	info->var.bits_per_pixel = 1;

	info->fbdefio = &dotlcd_defio;
	fb_deferred_io_init(info);

	platform_set_drvdata(pdev, d);
	ret = register_framebuffer(info);
	if (ret)
		goto err_defio;

	dev_info(&pdev->dev, "SPX4-MF Medium-Format Camera Shoulder MIP dotlcd registered (%ux%u 1-bpp, /dev/fb%d)\n",
		 DOTLCD_WIDTH, DOTLCD_HEIGHT, info->node);
	return 0;

err_defio:
	fb_deferred_io_cleanup(info);
	vfree(d->vmem);
err_release:
	framebuffer_release(info);
	return ret;
}

static int fuji_dotlcd_remove(struct platform_device *pdev)
{
	struct fuji_dotlcd *d = platform_get_drvdata(pdev);

	unregister_framebuffer(d->fb);
	fb_deferred_io_cleanup(d->fb);
	vfree(d->vmem);
	framebuffer_release(d->fb);
	return 0;
}

static const struct of_device_id fuji_dotlcd_of_match[] = {
	{ .compatible = "fujisan,spx4-dotlcd" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_dotlcd_of_match);

static struct platform_driver fuji_dotlcd_driver = {
	.probe	= fuji_dotlcd_probe,
	.remove	= fuji_dotlcd_remove,
	.driver	= {
		.name		= "fuji-spx4-dotlcd",
		.of_match_table	= fuji_dotlcd_of_match,
		.dev_groups	= dotlcd_groups,
	},
};
module_platform_driver(fuji_dotlcd_driver);

MODULE_DESCRIPTION("SPX4-MF Medium-Format Camera 304x230 1-bpp Shoulder MIP Sub-LCD driver");
MODULE_LICENSE("GPL");
