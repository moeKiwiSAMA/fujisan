// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan SPX4 DMP PICA / M2D OpenVG (VGLIB) 2D Hardware Accelerator Driver
 *   MMIO: 0xFFFD0000..0xFFFD20FF (GIC SPI 92)
 *
 * Wave-3 Hardware Verification (DAT @ section4.bin 0x01373174..0x01402648):
 *   - Fill Engine (+0x0010..+0x001C, +0x00E0..+0x00EC): solid fill (0x0F100201)
 *     and extended/gradient fill (0x0F100211), completion IRQ bit 26.
 *   - Copy / Morton 8x8 Tile Engine (+0x0C00..+0x0C28): linear (mode=1) and
 *     Morton 8x8 tile swizzle (mode=2), completion IRQ bit 30.
 *   - 3-Stream DMP PICA OpenVG Command Processor:
 *       Stream 0 (Geometry/Path):  +0x10D4 addr, +0x10DC words>>1, +0x10D0 kick
 *       Stream 1 (Fragment/Paint): +0x10E4 addr, +0x10EC words>>1, +0x10E0 kick
 *       Stream 2 (3rd DMA stream): +0x20E0 arg0>>3, +0x20E8 arg1>>3, +0x20F0 kick
 *     Completion IRQ bit 31 (+0x0034).
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

#include "fuji_spx4_hw.h"

struct fuji_vglib_fill_req {
	__u32 dst_phys;
	__u32 byte_len;
	__u32 rgba_word;
	__u32 flags;
};

struct fuji_vglib_copy_req {
	__u32 src_phys;
	__u32 dst_phys;
	__u16 src_width;
	__u16 src_height;
	__u16 dst_width;
	__u16 dst_height;
	__u32 mode; /* 1 = linear, 2 = Morton 8x8 swizzle */
};

struct fuji_vglib_submit3_req {
	__u32 strm0_phys;
	__u32 strm0_words;
	__u32 strm1_phys;
	__u32 strm1_words;
	__u32 strm2_arg0;
	__u32 strm2_arg1;
	__u32 flags;
};

#define FUJI_VGLIB_IOC_MAGIC	'V'
#define FUJI_VGLIB_IOC_FILL	_IOW(FUJI_VGLIB_IOC_MAGIC, 1, struct fuji_vglib_fill_req)
#define FUJI_VGLIB_IOC_COPY	_IOW(FUJI_VGLIB_IOC_MAGIC, 2, struct fuji_vglib_copy_req)
#define FUJI_VGLIB_IOC_SUBMIT3	_IOW(FUJI_VGLIB_IOC_MAGIC, 3, struct fuji_vglib_submit3_req)

struct fuji_spx4_vglib {
	struct device *dev;
	void __iomem *base;
	resource_size_t reg_size;
	int irq;
	struct completion done;
	struct mutex lock;
	struct miscdevice misc;
	u32 last_irq_status;
	u64 irq_count;
};

static void vglib_hw_init(struct fuji_spx4_vglib *vg)
{
	/* Exact DAT init sequence (section4.bin 0x01401FF0..0x014022B8) */
	writel_relaxed(SPX4_VGLIB_IRQ_MASK_DEFAULT, vg->base + SPX4_VGLIB_IRQ_MASK2);
	writel_relaxed(0xFFFFFFFFU, vg->base + SPX4_VGLIB_IRQ_MASK0);
	writel_relaxed(0xFFFFFFFFU, vg->base + SPX4_VGLIB_IRQ_MASK1);
	writel_relaxed(0U, vg->base + SPX4_VGLIB_CMD_STAT_RST);
	writel_relaxed(0U, vg->base + SPX4_VGLIB_CMD_RST0);
	writel_relaxed(0U, vg->base + SPX4_VGLIB_CMD_ACK);
	writel_relaxed(SPX4_VGLIB_MAGIC_UNLOCK, vg->base + SPX4_VGLIB_UNLOCK_MAGIC1);
	writel_relaxed(SPX4_VGLIB_MAGIC_UNLOCK, vg->base + SPX4_VGLIB_UNLOCK_MAGIC0);
	writel_relaxed(SPX4_VGLIB_IRQ_MASK_DEFAULT, vg->base + SPX4_VGLIB_IRQ_MASK2);
	writel_relaxed(SPX4_VGLIB_IRQ_MASK_DEFAULT, vg->base + SPX4_VGLIB_IRQ_MASK1);
	writel_relaxed(1U, vg->base + SPX4_VGLIB_INIT_CTRL);
	writel_relaxed(0U, vg->base + SPX4_VGLIB_COPY_KICK);
	writel_relaxed(0U, vg->base + SPX4_VGLIB_FILL_KICK);
	writel_relaxed(0U, vg->base + SPX4_VGLIB_IRQ_STATUS);
	writel_relaxed(SPX4_VGLIB_COPY_INIT_VAL, vg->base + SPX4_VGLIB_COPY_INIT_CFG);
}

static void vglib_drain_pipeline(struct fuji_spx4_vglib *vg)
{
	int tries = 100;

	/* DAT @ 0x014025EC..0x01402648: poll +0x58 bit 26 while draining +0x1214/+0x1200 */
	while ((readl_relaxed(vg->base + SPX4_VGLIB_BUSY_STATUS) & SPX4_VGLIB_BUSY_PIPE_B26) &&
	       --tries > 0) {
		writel_relaxed(SPX4_VGLIB_PIPE_DRAIN_TRIG, vg->base + SPX4_VGLIB_PIPE_TRIG);
		(void)readl_relaxed(vg->base + SPX4_VGLIB_PIPE_DRAIN_RD);
		udelay(5);
	}
}

static irqreturn_t vglib_irq_handler(int irq, void *dev_id)
{
	struct fuji_spx4_vglib *vg = dev_id;
	u32 st = readl_relaxed(vg->base + SPX4_VGLIB_IRQ_STATUS);

	if (!st)
		return IRQ_NONE;

	/* DAT @ section4.bin 0x01401E30..0x01401EF0: acknowledge per sub-engine */
	if (st & SPX4_VGLIB_IRQ_CMD_DONE) {
		writel_relaxed(0U, vg->base + SPX4_VGLIB_CMD_STAT_RST);
		writel_relaxed(0U, vg->base + SPX4_VGLIB_CMD_ACK);
	}
	if (st & SPX4_VGLIB_IRQ_COPY_DONE)
		writel_relaxed(0U, vg->base + SPX4_VGLIB_COPY_KICK);
	if (st & SPX4_VGLIB_IRQ_FILL_DONE)
		writel_relaxed(0U, vg->base + SPX4_VGLIB_FILL_KICK);

	writel_relaxed(0U, vg->base + SPX4_VGLIB_IRQ_STATUS);
	vg->last_irq_status = st;
	vg->irq_count++;
	complete(&vg->done);
	return IRQ_HANDLED;
}

static long vglib_ioctl_fill(struct fuji_spx4_vglib *vg, void __user *uarg)
{
	struct fuji_vglib_fill_req req;
	unsigned long timeout;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;

	if (req.byte_len == 0 || (req.dst_phys & 3) || (req.byte_len & 3) ||
	    req.byte_len > 0x00400000U)
		return -EINVAL;

	mutex_lock(&vg->lock);
	reinit_completion(&vg->done);
	writel_relaxed(req.dst_phys, vg->base + SPX4_VGLIB_FILL_START);
	writel_relaxed(req.dst_phys + req.byte_len, vg->base + SPX4_VGLIB_FILL_END);
	writel_relaxed(req.rgba_word, vg->base + SPX4_VGLIB_FILL_COLOR);
	writel_relaxed((req.flags & 1U) ? SPX4_VGLIB_FILL_KICK_EXT : SPX4_VGLIB_FILL_KICK_SOLID,
		       vg->base + SPX4_VGLIB_FILL_KICK);

	timeout = wait_for_completion_timeout(&vg->done, msecs_to_jiffies(100));
	mutex_unlock(&vg->lock);
	return timeout ? 0 : -ETIMEDOUT;
}

static long vglib_ioctl_copy(struct fuji_spx4_vglib *vg, void __user *uarg)
{
	struct fuji_vglib_copy_req req;
	unsigned long timeout;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;

	if (!req.src_phys || !req.dst_phys || !req.src_width || !req.src_height ||
	    !req.dst_width || !req.dst_height)
		return -EINVAL;
	if (req.mode != SPX4_VGLIB_COPY_MODE_LINEAR &&
	    req.mode != SPX4_VGLIB_COPY_MODE_MORTON8)
		return -EINVAL;

	mutex_lock(&vg->lock);
	reinit_completion(&vg->done);
	writel_relaxed(req.src_phys, vg->base + SPX4_VGLIB_COPY_SRC);
	writel_relaxed(req.dst_phys, vg->base + SPX4_VGLIB_COPY_DST);
	writel_relaxed(((u32)req.src_height << 16) | req.src_width,
		       vg->base + SPX4_VGLIB_COPY_SRC_DIM);
	writel_relaxed(((u32)req.dst_height << 16) | req.dst_width,
		       vg->base + SPX4_VGLIB_COPY_DST_DIM);
	writel_relaxed(req.mode, vg->base + SPX4_VGLIB_COPY_MODE);
	writel_relaxed(1U, vg->base + SPX4_VGLIB_COPY_KICK);

	timeout = wait_for_completion_timeout(&vg->done, msecs_to_jiffies(100));
	mutex_unlock(&vg->lock);
	return timeout ? 0 : -ETIMEDOUT;
}

static long vglib_ioctl_submit3(struct fuji_spx4_vglib *vg, void __user *uarg)
{
	struct fuji_vglib_submit3_req req;
	unsigned long timeout;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;

	if (!req.strm0_phys || !req.strm0_words || (req.strm0_words & 1U))
		return -EINVAL;
	if (!req.strm1_phys || !req.strm1_words || (req.strm1_words & 1U))
		return -EINVAL;

	mutex_lock(&vg->lock);
	vglib_drain_pipeline(vg);
	reinit_completion(&vg->done);

	/* DAT @ section4.bin 0x014022EC..0x014023D0: trigger + Stream 1 + Stream 0 + Stream 2 */
	writel_relaxed((req.flags & 1U) ? SPX4_VGLIB_PIPE_TRIG_EXT : SPX4_VGLIB_PIPE_TRIG_NORM,
		       vg->base + SPX4_VGLIB_PIPE_TRIG);
	writel_relaxed(0U, vg->base + SPX4_VGLIB_SUBMIT_CLR);

	/* Stream 1 (Fragment / Material / Paint) */
	writel_relaxed(req.strm1_phys, vg->base + SPX4_VGLIB_STRM1_ADDR);
	writel_relaxed(spx4_vglib_stream_size8(req.strm1_words), vg->base + SPX4_VGLIB_STRM1_SIZE8);
	writel_relaxed(1U, vg->base + SPX4_VGLIB_STRM1_KICK);

	/* Stream 0 (Geometry / Vertex / Path) */
	writel_relaxed(req.strm0_phys, vg->base + SPX4_VGLIB_STRM0_ADDR);
	writel_relaxed(spx4_vglib_stream_size8(req.strm0_words), vg->base + SPX4_VGLIB_STRM0_SIZE8);
	writel_relaxed(1U, vg->base + SPX4_VGLIB_STRM0_KICK);

	/* Optional 3rd DMA / Command Stream (+0x20E0..+0x20F0) */
	if ((req.strm2_arg0 || req.strm2_arg1) && vg->reg_size >= SPX4_VGLIB_PHYS_SIZE) {
		writel_relaxed(spx4_vglib_stream2_units(req.strm2_arg0),
			       vg->base + SPX4_VGLIB_STRM2_ARG0);
		writel_relaxed(spx4_vglib_stream2_units(req.strm2_arg1),
			       vg->base + SPX4_VGLIB_STRM2_ARG1);
		writel_relaxed(1U, vg->base + SPX4_VGLIB_STRM2_KICK);
	}

	timeout = wait_for_completion_timeout(&vg->done, msecs_to_jiffies(200));
	mutex_unlock(&vg->lock);
	return timeout ? 0 : -ETIMEDOUT;
}

static long vglib_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct miscdevice *mdev = file->private_data;
	struct fuji_spx4_vglib *vg = container_of(mdev, struct fuji_spx4_vglib, misc);

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	switch (cmd) {
	case FUJI_VGLIB_IOC_FILL:
		return vglib_ioctl_fill(vg, (void __user *)arg);
	case FUJI_VGLIB_IOC_COPY:
		return vglib_ioctl_copy(vg, (void __user *)arg);
	case FUJI_VGLIB_IOC_SUBMIT3:
		return vglib_ioctl_submit3(vg, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vglib_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= vglib_ioctl,
	.llseek		= noop_llseek,
};

static int fuji_spx4_vglib_probe(struct platform_device *pdev)
{
	struct fuji_spx4_vglib *vg;
	struct resource *res;
	int ret;

	vg = devm_kzalloc(&pdev->dev, sizeof(*vg), GFP_KERNEL);
	if (!vg)
		return -ENOMEM;

	vg->dev = &pdev->dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	vg->reg_size = resource_size(res);
	vg->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(vg->base))
		return PTR_ERR(vg->base);

	init_completion(&vg->done);
	mutex_init(&vg->lock);
	vglib_hw_init(vg);

	vg->irq = platform_get_irq_optional(pdev, 0);
	if (vg->irq > 0) {
		ret = devm_request_irq(&pdev->dev, vg->irq, vglib_irq_handler,
				       IRQF_SHARED, "fuji-spx4-vglib", vg);
		if (ret)
			return ret;
	}

	vg->misc.minor = MISC_DYNAMIC_MINOR;
	vg->misc.name = "fuji-vglib";
	vg->misc.fops = &vglib_fops;
	vg->misc.parent = &pdev->dev;

	ret = misc_register(&vg->misc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, vg);
	dev_info(&pdev->dev,
		 "Fujisan SPX4 DMP PICA/M2D VGLIB registered (/dev/fuji-vglib, irq=%d, win=0x%zx)\n",
		 vg->irq, (size_t)vg->reg_size);
	return 0;
}

static int fuji_spx4_vglib_remove(struct platform_device *pdev)
{
	struct fuji_spx4_vglib *vg = platform_get_drvdata(pdev);

	misc_deregister(&vg->misc);
	return 0;
}

static const struct of_device_id fuji_spx4_vglib_of_match[] = {
	{ .compatible = "fujisan,spx4-vglib" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_vglib_of_match);

static struct platform_driver fuji_spx4_vglib_driver = {
	.probe	= fuji_spx4_vglib_probe,
	.remove	= fuji_spx4_vglib_remove,
	.driver	= {
		.name		= "fuji-spx4-vglib",
		.of_match_table	= fuji_spx4_vglib_of_match,
	},
};
module_platform_driver(fuji_spx4_vglib_driver);

MODULE_DESCRIPTION("Fujisan SPX4 DMP PICA/M2D OpenVG (VGLIB) 3-stream 2D accelerator driver");
MODULE_LICENSE("GPL");
