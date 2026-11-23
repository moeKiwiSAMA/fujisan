// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan OS — SPX4 CCDIF V4L2 Capture, Semi-Planar Y+UV Live-View, RAW DMA &
 *              20-Channel 3A/PDAF Hardware Stats Engine
 *
 * DAT verified (SPX4-MF 2.13 CMOS-RE-2 & CMOS-RE-3):
 *   - 0xFFF6E000 (IRQ 105): Live RAW+OB Write-DMA (34-bit inline pri/OB bases)
 *   - 0xFFF72000 / 0xFFF73000 (IRQ 107/108): 2-Plane Semi-Planar Y+UV Live-View Ch0/Ch1
 *     (+0xC0/+0xC4 Plane 0 Y, +0xC8/+0xCC Plane 1 UV, +0xD0 stride, +0xD8 ring)
 *   - 0xFFF70000 (IRQ 106): 20-Ch 3A/AE/AWB/CAF/ROB/PDAF Stats DMA Engine
 *     (+0x108..+0x120 filter config, +0x2D4/+0x2D8 & +0x2DC/+0x2E0 PDAF 34-bit DDR bases)
 *   - 0xFFF10000..0xFFF1DFFF: 2 x 28 KiB ping-pong Stats SRAM (Bank 0 / Bank 1)
 *   - 0x2C4B0000..0x2C545FFF: 4-slot 600 KiB PDAF DDR ring (stride 0xD800)
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "fujisan_spx4_hw.h"

struct fujisan_ccdif_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct fujisan_ccdif {
	struct device *dev;
	void __iomem *raw_base;		/* 0xFFF6E000 */
	void __iomem *stats_base;	/* 0xFFF70000 */
	void __iomem *lv0_base;		/* 0xFFF72000 */
	void __iomem *lv1_base;		/* 0xFFF73000 */
	void __iomem *stats_sram;	/* 0xFFF10000 (56 KiB) */

	struct mutex lock;
	spinlock_t qlock;
	struct list_head buf_queue;
	struct fujisan_ccdif_buf *active_buf;

	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue vb2_q;

	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint32_t sequence;
	uint8_t stats_bank;
	uint8_t pdaf_slot;
};

static void ccdif_program_stats_pingpong(struct fujisan_ccdif *isp)
{
	uint32_t bank_pa = fujisan_stats_sram_bank_pa(isp->stats_bank);
	uint32_t pdaf_pri = fujisan_pdaf_slot_pri_pa(isp->pdaf_slot);
	uint32_t pdaf_sec = fujisan_pdaf_slot_sec_pa(isp->pdaf_slot);

	/* Program primary AE/AWB, Bayer 16x16, ROB, and PDAF 34-bit DDR bases */
	writel(bank_pa, isp->stats_base + FUJISAN_STATS_AE_PRI_LO);
	writel(0U, isp->stats_base + FUJISAN_STATS_AE_PRI_HI);
	writel(bank_pa + FUJISAN_STATS_BAYER_OFFSET, isp->stats_base + FUJISAN_STATS_BAYER_LO);
	writel(0U, isp->stats_base + FUJISAN_STATS_BAYER_HI);
	writel(bank_pa + FUJISAN_STATS_ROB_OFFSET, isp->stats_base + FUJISAN_STATS_ROB0_LO);
	writel(0U, isp->stats_base + FUJISAN_STATS_ROB0_HI);

	/* PDAF Primary & Secondary DDR 34-bit bases (+0x2D4..+0x2E0) */
	writel(pdaf_pri, isp->stats_base + FUJISAN_STATS_PDAF_PRI_LO);
	writel(0U, isp->stats_base + FUJISAN_STATS_PDAF_PRI_HI);
	writel(pdaf_sec, isp->stats_base + FUJISAN_STATS_PDAF_SEC_LO);
	writel(0U, isp->stats_base + FUJISAN_STATS_PDAF_SEC_HI);

	/* Enable 20-channel stats write masks & master run bit 8 */
	writel(fujisan_stats_ch_enable_word(0x3FFU, 0x3FFU), isp->stats_base + FUJISAN_STATS_EN_LO);
	writel(fujisan_stats_ch_enable_word(0x3FFU, 0x3FFU), isp->stats_base + FUJISAN_STATS_EN_HI);
	writel(1U << 8, isp->stats_base + FUJISAN_STATS_MASTER_CTRL);
}

static void ccdif_program_lv0_buffer(struct fujisan_ccdif *isp, dma_addr_t y_pa)
{
	uint32_t y_lo = 0, y_hi = 0;
	uint32_t uv_lo = 0, uv_hi = 0;
	uint32_t stride = ALIGN(isp->width, 16);
	dma_addr_t uv_pa = y_pa + ((dma_addr_t)stride * isp->height);

	spx4_dma34_split((uint64_t)y_pa, &y_lo, &y_hi);
	spx4_dma34_split((uint64_t)uv_pa, &uv_lo, &uv_hi);

	/* 2-plane semi-planar Y+UV on LV Ch0 (0xFFF72000) */
	writel(y_hi, isp->lv0_base + FUJISAN_LV_PLANE0_Y_HI);
	writel(y_lo, isp->lv0_base + FUJISAN_LV_PLANE0_Y_LO);
	writel(uv_hi, isp->lv0_base + FUJISAN_LV_PLANE1_UV_HI);
	writel(uv_lo, isp->lv0_base + FUJISAN_LV_PLANE1_UV_LO);
	writel(stride, isp->lv0_base + FUJISAN_LV_STRIDE);
	writel((1U << 28) | 1U, isp->lv0_base + FUJISAN_LV_RING_CTRL);
	writel(1U, isp->lv0_base + FUJISAN_LV_ENABLE);
}

static irqreturn_t ccdif_lv0_irq(int irq, void *dev_id)
{
	struct fujisan_ccdif *isp = dev_id;
	uint32_t st = readl(isp->lv0_base + FUJISAN_LV_IRQ_STATUS);
	unsigned long flags;

	writel(st, isp->lv0_base + FUJISAN_LV_IRQ_STATUS);

	spin_lock_irqsave(&isp->qlock, flags);
	if (isp->active_buf) {
		isp->active_buf->vb.vb2_buf.timestamp = ktime_get_ns();
		isp->active_buf->vb.sequence = isp->sequence++;
		vb2_buffer_done(&isp->active_buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
		isp->active_buf = NULL;
	}
	if (!list_empty(&isp->buf_queue)) {
		struct fujisan_ccdif_buf *next = list_first_entry(&isp->buf_queue,
								  struct fujisan_ccdif_buf, list);
		list_del(&next->list);
		isp->active_buf = next;
		ccdif_program_lv0_buffer(isp, vb2_dma_contig_plane_dma_addr(&next->vb.vb2_buf, 0));
	}
	spin_unlock_irqrestore(&isp->qlock, flags);
	return IRQ_HANDLED;
}

static irqreturn_t ccdif_stats_irq(int irq, void *dev_id)
{
	struct fujisan_ccdif *isp = dev_id;
	uint32_t st = readl(isp->stats_base + FUJISAN_STATS_IRQ_W1C);

	if (!(st & 1U))
		return IRQ_NONE;

	writel(1U, isp->stats_base + FUJISAN_STATS_IRQ_W1C);
	isp->stats_bank ^= 1U;
	isp->pdaf_slot = (isp->pdaf_slot + 1U) & 3U;
	ccdif_program_stats_pingpong(isp);
	return IRQ_HANDLED;
}

static irqreturn_t ccdif_raw_irq(int irq, void *dev_id)
{
	struct fujisan_ccdif *isp = dev_id;
	uint32_t st = readl(isp->raw_base + FUJISAN_RAW_IRQ_W1C);

	if (!(st & (1U << 8)))
		return IRQ_NONE;

	writel(1U << 8, isp->raw_base + FUJISAN_RAW_IRQ_W1C);
	return IRQ_HANDLED;
}

static int ccdif_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "fujisan-ccdif", sizeof(cap->driver));
	strscpy(cap->card, "Fujisan SPX4 CCDIF ISP (Semi-Planar LV + RAW + 3A/PDAF)",
		sizeof(cap->card));
	strscpy(cap->bus_info, "platform:fujisan-ccdif", sizeof(cap->bus_info));
	return 0;
}

static int ccdif_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	static const uint32_t fmts[] = { V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_SRGGB16 };

	if (f->index >= ARRAY_SIZE(fmts))
		return -EINVAL;
	f->pixelformat = fmts[f->index];
	return 0;
}

static int ccdif_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct fujisan_ccdif *isp = video_drvdata(file);
	uint32_t stride = ALIGN(isp->width, 16);

	f->fmt.pix.width = isp->width;
	f->fmt.pix.height = isp->height;
	f->fmt.pix.pixelformat = isp->fourcc;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = (isp->fourcc == V4L2_PIX_FMT_SRGGB16) ? (stride * 2U) : stride;
	f->fmt.pix.sizeimage = (isp->fourcc == V4L2_PIX_FMT_NV12) ?
			       (stride * isp->height * 3U / 2U) :
			       (stride * isp->height * 2U);
	f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
	return 0;
}

static int ccdif_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct fujisan_ccdif *isp = video_drvdata(file);

	isp->width = clamp_t(uint32_t, f->fmt.pix.width, 320, 11760);
	isp->height = clamp_t(uint32_t, f->fmt.pix.height, 240, 8754);
	if (f->fmt.pix.pixelformat == V4L2_PIX_FMT_NV12 ||
	    f->fmt.pix.pixelformat == V4L2_PIX_FMT_NV16 ||
	    f->fmt.pix.pixelformat == V4L2_PIX_FMT_SRGGB16)
		isp->fourcc = f->fmt.pix.pixelformat;
	return ccdif_g_fmt(file, priv, f);
}

static const struct v4l2_ioctl_ops ccdif_ioctl_ops = {
	.vidioc_querycap	= ccdif_querycap,
	.vidioc_enum_fmt_vid_cap = ccdif_enum_fmt,
	.vidioc_g_fmt_vid_cap	= ccdif_g_fmt,
	.vidioc_s_fmt_vid_cap	= ccdif_s_fmt,
	.vidioc_reqbufs		= vb2_ioctl_reqbufs,
	.vidioc_querybuf	= vb2_ioctl_querybuf,
	.vidioc_qbuf		= vb2_ioctl_qbuf,
	.vidioc_dqbuf		= vb2_ioctl_dqbuf,
	.vidioc_streamon	= vb2_ioctl_streamon,
	.vidioc_streamoff	= vb2_ioctl_streamoff,
};

static int ccdif_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			     unsigned int *nplanes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct fujisan_ccdif *isp = vb2_get_drv_priv(q);
	uint32_t stride = ALIGN(isp->width, 16);

	*nplanes = 1;
	sizes[0] = (isp->fourcc == V4L2_PIX_FMT_NV12) ?
		   (stride * isp->height * 3U / 2U) : (stride * isp->height * 2U);
	if (*nbuffers < 2)
		*nbuffers = 2;
	return 0;
}

static void ccdif_buf_queue(struct vb2_buffer *vb)
{
	struct fujisan_ccdif *isp = vb2_get_drv_priv(vb->vb2_queue);
	struct fujisan_ccdif_buf *buf = container_of(to_vb2_v4l2_buffer(vb),
						     struct fujisan_ccdif_buf, vb);
	unsigned long flags;

	spin_lock_irqsave(&isp->qlock, flags);
	list_add_tail(&buf->list, &isp->buf_queue);
	spin_unlock_irqrestore(&isp->qlock, flags);
}

static int ccdif_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct fujisan_ccdif *isp = vb2_get_drv_priv(q);
	unsigned long flags;
	int ret;

	ret = pm_runtime_resume_and_get(isp->dev);
	if (ret < 0)
		return ret;

	isp->sequence = 0;
	ccdif_program_stats_pingpong(isp);

	spin_lock_irqsave(&isp->qlock, flags);
	if (!list_empty(&isp->buf_queue)) {
		struct fujisan_ccdif_buf *first = list_first_entry(&isp->buf_queue,
								   struct fujisan_ccdif_buf, list);
		list_del(&first->list);
		isp->active_buf = first;
		ccdif_program_lv0_buffer(isp, vb2_dma_contig_plane_dma_addr(&first->vb.vb2_buf, 0));
	}
	spin_unlock_irqrestore(&isp->qlock, flags);
	return 0;
}

static void ccdif_stop_streaming(struct vb2_queue *q)
{
	struct fujisan_ccdif *isp = vb2_get_drv_priv(q);
	struct fujisan_ccdif_buf *buf, *tmp;
	unsigned long flags;

	writel(0U, isp->lv0_base + FUJISAN_LV_ENABLE);
	writel(0U, isp->stats_base + FUJISAN_STATS_MASTER_CTRL);

	spin_lock_irqsave(&isp->qlock, flags);
	if (isp->active_buf) {
		vb2_buffer_done(&isp->active_buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
		isp->active_buf = NULL;
	}
	list_for_each_entry_safe(buf, tmp, &isp->buf_queue, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&isp->qlock, flags);
	pm_runtime_put(isp->dev);
}

static const struct vb2_ops ccdif_vb2_ops = {
	.queue_setup		= ccdif_queue_setup,
	.buf_queue		= ccdif_buf_queue,
	.start_streaming	= ccdif_start_streaming,
	.stop_streaming		= ccdif_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static const struct v4l2_file_operations ccdif_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

static int fujisan_ccdif_probe(struct platform_device *pdev)
{
	struct fujisan_ccdif *isp;
	int irq, ret;

	isp = devm_kzalloc(&pdev->dev, sizeof(*isp), GFP_KERNEL);
	if (!isp)
		return -ENOMEM;

	isp->dev = &pdev->dev;
	mutex_init(&isp->lock);
	spin_lock_init(&isp->qlock);
	INIT_LIST_HEAD(&isp->buf_queue);
	isp->width = 1024;
	isp->height = 768;
	isp->fourcc = V4L2_PIX_FMT_NV12;

	isp->raw_base = devm_platform_ioremap_resource_byname(pdev, "raw-dma");
	if (IS_ERR(isp->raw_base))
		return PTR_ERR(isp->raw_base);
	isp->stats_base = devm_platform_ioremap_resource_byname(pdev, "stats");
	if (IS_ERR(isp->stats_base))
		return PTR_ERR(isp->stats_base);
	isp->lv0_base = devm_platform_ioremap_resource_byname(pdev, "lv-ch0");
	if (IS_ERR(isp->lv0_base))
		return PTR_ERR(isp->lv0_base);
	isp->lv1_base = devm_platform_ioremap_resource_byname(pdev, "lv-ch1");
	if (IS_ERR(isp->lv1_base))
		return PTR_ERR(isp->lv1_base);
	isp->stats_sram = devm_platform_ioremap_resource_byname(pdev, "stats-sram");
	if (IS_ERR(isp->stats_sram))
		return PTR_ERR(isp->stats_sram);

	irq = platform_get_irq_byname_optional(pdev, "lv0");
	if (irq > 0)
		devm_request_irq(&pdev->dev, irq, ccdif_lv0_irq, 0, "fujisan-ccdif-lv0", isp);
	irq = platform_get_irq_byname_optional(pdev, "stats");
	if (irq > 0)
		devm_request_irq(&pdev->dev, irq, ccdif_stats_irq, 0, "fujisan-ccdif-stats", isp);
	irq = platform_get_irq_byname_optional(pdev, "raw");
	if (irq > 0)
		devm_request_irq(&pdev->dev, irq, ccdif_raw_irq, 0, "fujisan-ccdif-raw", isp);

	ret = v4l2_device_register(&pdev->dev, &isp->v4l2_dev);
	if (ret)
		return ret;

	isp->vb2_q.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	isp->vb2_q.io_modes = VB2_MMAP | VB2_DMABUF;
	isp->vb2_q.drv_priv = isp;
	isp->vb2_q.buf_struct_size = sizeof(struct fujisan_ccdif_buf);
	isp->vb2_q.ops = &ccdif_vb2_ops;
	isp->vb2_q.mem_ops = &vb2_dma_contig_memops;
	isp->vb2_q.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	isp->vb2_q.lock = &isp->lock;
	isp->vb2_q.dev = &pdev->dev;
	ret = vb2_queue_init(&isp->vb2_q);
	if (ret)
		goto err_v4l2;

	strscpy(isp->vdev.name, "fujisan-ccdif", sizeof(isp->vdev.name));
	isp->vdev.v4l2_dev = &isp->v4l2_dev;
	isp->vdev.fops = &ccdif_fops;
	isp->vdev.ioctl_ops = &ccdif_ioctl_ops;
	isp->vdev.release = video_device_release_empty;
	isp->vdev.lock = &isp->lock;
	isp->vdev.queue = &isp->vb2_q;
	isp->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(&isp->vdev, isp);

	ret = video_register_device(&isp->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_v4l2;

	pm_runtime_enable(&pdev->dev);
	platform_set_drvdata(pdev, isp);
	dev_info(&pdev->dev, "Fujisan CCDIF V4L2 capture registered (%s)\n",
		 video_device_node_name(&isp->vdev));
	return 0;

err_v4l2:
	v4l2_device_unregister(&isp->v4l2_dev);
	return ret;
}

static void fujisan_ccdif_remove(struct platform_device *pdev)
{
	struct fujisan_ccdif *isp = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);
	video_unregister_device(&isp->vdev);
	v4l2_device_unregister(&isp->v4l2_dev);
}

static const struct of_device_id fujisan_ccdif_of_match[] = {
	{ .compatible = "fujisan,spx4-ccdif" },
	{ }
};
MODULE_DEVICE_TABLE(of, fujisan_ccdif_of_match);

static struct platform_driver fujisan_ccdif_driver = {
	.probe = fujisan_ccdif_probe,
	.remove_new = fujisan_ccdif_remove,
	.driver = {
		.name = "fujisan-ccdif",
		.of_match_table = fujisan_ccdif_of_match,
	},
};
module_platform_driver(fujisan_ccdif_driver);

MODULE_AUTHOR("Fujisan OS Contributors");
MODULE_DESCRIPTION("Fujisan OS SPX4 CCDIF Semi-Planar LV, RAW DMA & 20-Ch 3A/PDAF Stats V4L2 Driver");
MODULE_LICENSE("GPL");
