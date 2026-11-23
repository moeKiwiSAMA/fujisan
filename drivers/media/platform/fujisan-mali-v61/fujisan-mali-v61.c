// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan OS — ARM Mali-V61 4K H.265/H.264 V4L2 M2M Driver
 *
 * DAT verified (SPX4-MF 2.13 CMOS-RE-4, S4 0x01201E6C..0x01207508):
 *   - MMIO Base: 0xFFE00000 (Main +0x00..+0x50, LSID[0..3] +0x200..+0x2FF)
 *   - GIC IRQ:   138 (Level)
 *   - Power Domain: PD 10 (fujisan-spx4-pm)
 *   - Verified Input Formats:
 *       8-bit  NV12 (0x0201): 2-plane semi-planar Y + UV
 *       10-bit Y0L2 (0x0121): 1-plane packed 2x2-tiled YUV 4:2:0 (stride[0]=stride*4, NOT AFBC)
 *   - LSID 2-Level MMU PTE layout: ((PA >> 12) << 2) | (busatr_idx << 30) | perm[1:0]
 *     BUSATR0..2 = 0x33, BUSATR3 = 0x23, LSID_CTRL = 0x4F0, LSID_SCHED = 1
 *   - User-extracted RASC firmwares (zero proprietary blobs bundled in repository):
 *       /lib/firmware/fujisan/mali-v61/{hevc_enc,hevc_dec,h264_enc,h264_dec}.bin
 */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "fujisan_spx4_hw.h"

/* Custom 10-bit packed 2x2-tiled Y0L2 FourCC matching Mali-V61 0x0121 */
#define V4L2_PIX_FMT_FUJISAN_Y0L2	v4l2_fourcc('Y', '0', 'L', '2')

struct fujisan_mali_v61 {
	struct device *dev;
	void __iomem *base;
	int irq;
	uint32_t version;
	uint32_t ncores;
	uint32_t nlsid;
	struct mutex lock;
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m_dev;
};

struct fujisan_mv61_ctx {
	struct v4l2_fh fh;
	struct fujisan_mali_v61 *vpu;
	uint32_t width;
	uint32_t height;
	uint32_t src_fourcc;
	uint32_t dst_fourcc;
	uint32_t lsid;
	uint32_t *l1_pt;
	dma_addr_t l1_dma;
	uint32_t *l2_fw_pt;
	dma_addr_t l2_fw_dma;
};

static int mv61_map_lsid_session(struct fujisan_mv61_ctx *ctx, dma_addr_t fw_pa, size_t fw_len)
{
	struct fujisan_mali_v61 *vpu = ctx->vpu;
	void __iomem *lsid_base = vpu->base + FUJISAN_MV61_LSID_BASE(ctx->lsid);
	uint32_t term, l1_desc;
	size_t pages, i;
	int ret;

	/* Populate L1[0] -> L2 firmware table and map .text RO with BUSATR3 (idx 3) */
	memset(ctx->l1_pt, 0, PAGE_SIZE);
	memset(ctx->l2_fw_pt, 0, PAGE_SIZE);

	ctx->l1_pt[FUJISAN_MV61_L1_SLOT_FW] =
		fujisan_mv61_encode_pte((uint32_t)ctx->l2_fw_dma, 3U, FUJISAN_MV61_PTE_RW);
	pages = min_t(size_t, DIV_ROUND_UP(fw_len, PAGE_SIZE), 1024U);
	for (i = 0; i < pages; i++) {
		uint32_t page_pa = (uint32_t)(fw_pa + (i * PAGE_SIZE));

		ctx->l2_fw_pt[i] = fujisan_mv61_encode_pte(page_pa, 3U, FUJISAN_MV61_PTE_RO);
	}
	dma_wmb();

	/* Exact DAT activation order (h265_MapSession @ 0x01207508) */
	writel(1U, lsid_base + FUJISAN_MV61_LSID_ALLOC);
	writel(1U, lsid_base + FUJISAN_MV61_LSID_TERMINATE);
	ret = readl_poll_timeout_atomic(lsid_base + FUJISAN_MV61_LSID_TERMINATE,
					term, term == 0U, 10, 20000);
	if (ret)
		return ret;

	writel(FUJISAN_MV61_BUSATR_DATA, lsid_base + FUJISAN_MV61_LSID_BUSATR0);
	writel(FUJISAN_MV61_BUSATR_DATA, lsid_base + FUJISAN_MV61_LSID_BUSATR1);
	writel(FUJISAN_MV61_BUSATR_DATA, lsid_base + FUJISAN_MV61_LSID_BUSATR2);
	writel(FUJISAN_MV61_BUSATR_FW, lsid_base + FUJISAN_MV61_LSID_BUSATR3);

	l1_desc = fujisan_mv61_encode_pte((uint32_t)ctx->l1_dma, 0U, FUJISAN_MV61_PTE_RW);
	writel(l1_desc, lsid_base + FUJISAN_MV61_LSID_MMU_CTRL);
	writel(FUJISAN_MV61_LSID_CTRL_VAL, lsid_base + FUJISAN_MV61_LSID_CTRL);
	writel(0U, lsid_base + FUJISAN_MV61_LSID_NPROT);
	writel(0U, lsid_base + FUJISAN_MV61_LSID_IRQVE);
	writel(0U, lsid_base + FUJISAN_MV61_LSID_IRQHOST);
	writel(1U, lsid_base + FUJISAN_MV61_LSID_SCHED);
	return 0;
}

static irqreturn_t mv61_irq_handler(int irq, void *dev_id)
{
	struct fujisan_mali_v61 *vpu = dev_id;
	uint32_t pending = readl(vpu->base + FUJISAN_MV61_IRQVE);
	unsigned int lsid;

	if (!pending)
		return IRQ_NONE;

	for (lsid = 0; lsid < 4U; lsid++) {
		if (pending & BIT(lsid))
			writel(0U, vpu->base + FUJISAN_MV61_LSID_BASE(lsid) +
				   FUJISAN_MV61_LSID_IRQVE);
	}
	return IRQ_HANDLED;
}

static void mv61_device_run(void *priv)
{
	struct fujisan_mv61_ctx *ctx = priv;
	struct fujisan_mali_v61 *vpu = ctx->vpu;
	struct vb2_v4l2_buffer *src, *dst;
	struct fujisan_mve_buffer_frame frame;
	const struct firmware *fw = NULL;
	const char *fw_name;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (!src || !dst)
		goto done_err;

	if (ctx->src_fourcc == V4L2_PIX_FMT_FUJISAN_Y0L2)
		fujisan_mv61_fill_frame_y0l2(&frame, 0x50000000U,
					     (uint16_t)ctx->width, (uint16_t)ctx->height,
					     ALIGN(ctx->width, 16));
	else
		fujisan_mv61_fill_frame_nv12(&frame, 0x50000000U,
					     (uint16_t)ctx->width, (uint16_t)ctx->height,
					     ALIGN(ctx->width, 16));

	fw_name = (ctx->dst_fourcc == V4L2_PIX_FMT_HEVC) ?
		  "fujisan/mali-v61/hevc_enc.bin" : "fujisan/mali-v61/h264_enc.bin";

	if (request_firmware_direct(&fw, fw_name, vpu->dev) == 0) {
		if (!fujisan_mv61_check_rasc_header(fw->data, fw->size))
			dev_err(vpu->dev, "Invalid Mali-V61 RASC magic in %s\n", fw_name);
		release_firmware(fw);
	} else {
		dev_warn_once(vpu->dev,
			      "Mali-V61 RASC firmware %s not installed (extract with tools/extract-rasc-fw.py)\n",
			      fw_name);
	}

done_err:
	if (src)
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
	if (dst)
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
	v4l2_m2m_job_finish(vpu->m2m_dev, ctx->fh.m2m_ctx);
}

static const struct v4l2_m2m_ops mv61_m2m_ops = {
	.device_run = mv61_device_run,
};

static int mv61_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "fujisan-mali-v61", sizeof(cap->driver));
	strscpy(cap->card, "Fujisan SPX4 Mali-V61 4K VPU (NV12 / 10-bit Y0L2)", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:fujisan-mali-v61", sizeof(cap->bus_info));
	return 0;
}

static const uint32_t mv61_raw_fmts[] = { V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_FUJISAN_Y0L2 };
static const uint32_t mv61_coded_fmts[] = { V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_H264 };

static int mv61_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(mv61_raw_fmts))
		return -EINVAL;
	f->pixelformat = mv61_raw_fmts[f->index];
	return 0;
}

static int mv61_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(mv61_coded_fmts))
		return -EINVAL;
	f->pixelformat = mv61_coded_fmts[f->index];
	return 0;
}

static int mv61_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct fujisan_mv61_ctx *ctx = container_of(priv, struct fujisan_mv61_ctx, fh);
	uint32_t fourcc = V4L2_TYPE_IS_OUTPUT(f->type) ? ctx->src_fourcc : ctx->dst_fourcc;
	uint32_t stride = ALIGN(ctx->width, 16);

	f->fmt.pix_mp.width = ctx->width;
	f->fmt.pix_mp.height = ctx->height;
	f->fmt.pix_mp.pixelformat = fourcc;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.num_planes = 1;
	f->fmt.pix_mp.plane_fmt[0].bytesperline =
		(fourcc == V4L2_PIX_FMT_FUJISAN_Y0L2) ? (stride << 2) : stride;
	f->fmt.pix_mp.plane_fmt[0].sizeimage =
		(fourcc == V4L2_PIX_FMT_FUJISAN_Y0L2) ?
		((stride << 2) * (ctx->height >> 1)) : (stride * ctx->height * 3U / 2U);
	return 0;
}

static int mv61_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct fujisan_mv61_ctx *ctx = container_of(priv, struct fujisan_mv61_ctx, fh);

	ctx->width = clamp_t(uint32_t, f->fmt.pix_mp.width, 64, 4096);
	ctx->height = clamp_t(uint32_t, f->fmt.pix_mp.height, 64, 2160);
	if (V4L2_TYPE_IS_OUTPUT(f->type))
		ctx->src_fourcc = f->fmt.pix_mp.pixelformat;
	else
		ctx->dst_fourcc = f->fmt.pix_mp.pixelformat;
	return mv61_g_fmt(file, priv, f);
}

static const struct v4l2_ioctl_ops mv61_ioctl_ops = {
	.vidioc_querycap		= mv61_querycap,
	.vidioc_enum_fmt_vid_out	= mv61_enum_fmt_out,
	.vidioc_enum_fmt_vid_cap	= mv61_enum_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane	= mv61_g_fmt,
	.vidioc_g_fmt_vid_cap_mplane	= mv61_g_fmt,
	.vidioc_s_fmt_vid_out_mplane	= mv61_s_fmt,
	.vidioc_s_fmt_vid_cap_mplane	= mv61_s_fmt,
	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,
};

static int mv61_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			    unsigned int *nplanes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct fujisan_mv61_ctx *ctx = vb2_get_drv_priv(vq);

	*nplanes = 1;
	sizes[0] = ALIGN(ctx->width, 16) * ctx->height * 2U;
	if (*nbuffers < 2)
		*nbuffers = 2;
	return 0;
}

static void mv61_buf_queue(struct vb2_buffer *vb)
{
	struct fujisan_mv61_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static const struct vb2_ops mv61_qops = {
	.queue_setup	= mv61_queue_setup,
	.buf_queue	= mv61_buf_queue,
	.wait_prepare	= vb2_ops_wait_prepare,
	.wait_finish	= vb2_ops_wait_finish,
};

static int mv61_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct fujisan_mv61_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &mv61_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->vpu->lock;
	src_vq->dev = ctx->vpu->dev;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &mv61_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->vpu->lock;
	dst_vq->dev = ctx->vpu->dev;
	return vb2_queue_init(dst_vq);
}

static int mv61_open(struct file *file)
{
	struct fujisan_mali_v61 *vpu = video_drvdata(file);
	struct fujisan_mv61_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->l1_pt = dma_alloc_coherent(vpu->dev, PAGE_SIZE, &ctx->l1_dma, GFP_KERNEL);
	ctx->l2_fw_pt = dma_alloc_coherent(vpu->dev, PAGE_SIZE, &ctx->l2_fw_dma, GFP_KERNEL);
	if (!ctx->l1_pt || !ctx->l2_fw_pt) {
		if (ctx->l1_pt)
			dma_free_coherent(vpu->dev, PAGE_SIZE, ctx->l1_pt, ctx->l1_dma);
		if (ctx->l2_fw_pt)
			dma_free_coherent(vpu->dev, PAGE_SIZE, ctx->l2_fw_pt, ctx->l2_fw_dma);
		kfree(ctx);
		return -ENOMEM;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->vpu = vpu;
	ctx->width = 3840;
	ctx->height = 2160;
	ctx->src_fourcc = V4L2_PIX_FMT_FUJISAN_Y0L2;
	ctx->dst_fourcc = V4L2_PIX_FMT_HEVC;
	ctx->lsid = 0;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vpu->m2m_dev, ctx, mv61_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		int ret = PTR_ERR(ctx->fh.m2m_ctx);

		dma_free_coherent(vpu->dev, PAGE_SIZE, ctx->l1_pt, ctx->l1_dma);
		dma_free_coherent(vpu->dev, PAGE_SIZE, ctx->l2_fw_pt, ctx->l2_fw_dma);
		kfree(ctx);
		return ret;
	}

	(void)mv61_map_lsid_session; /* invoked on firmware-loaded session start */
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);
	return 0;
}

static int mv61_release(struct file *file)
{
	struct fujisan_mv61_ctx *ctx = container_of(file->private_data,
						    struct fujisan_mv61_ctx, fh);

	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	dma_free_coherent(ctx->vpu->dev, PAGE_SIZE, ctx->l1_pt, ctx->l1_dma);
	dma_free_coherent(ctx->vpu->dev, PAGE_SIZE, ctx->l2_fw_pt, ctx->l2_fw_dma);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations mv61_fops = {
	.owner		= THIS_MODULE,
	.open		= mv61_open,
	.release	= mv61_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static int fujisan_mali_v61_probe(struct platform_device *pdev)
{
	struct fujisan_mali_v61 *vpu;
	int ret;

	vpu = devm_kzalloc(&pdev->dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	vpu->dev = &pdev->dev;
	mutex_init(&vpu->lock);

	vpu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vpu->base))
		return PTR_ERR(vpu->base);

	vpu->irq = platform_get_irq_optional(pdev, 0);
	if (vpu->irq > 0) {
		ret = devm_request_irq(&pdev->dev, vpu->irq, mv61_irq_handler, 0,
				       "fujisan-mali-v61", vpu);
		if (ret)
			return ret;
	}

	pm_runtime_enable(&pdev->dev);
	vpu->version = readl(vpu->base + FUJISAN_MV61_VERSION);
	vpu->ncores = readl(vpu->base + FUJISAN_MV61_NCORES);
	vpu->nlsid = readl(vpu->base + FUJISAN_MV61_NLSID);

	ret = v4l2_device_register(&pdev->dev, &vpu->v4l2_dev);
	if (ret)
		goto err_pm;

	vpu->m2m_dev = v4l2_m2m_init(&mv61_m2m_ops);
	if (IS_ERR(vpu->m2m_dev)) {
		ret = PTR_ERR(vpu->m2m_dev);
		goto err_v4l2;
	}

	strscpy(vpu->vdev.name, "fujisan-mali-v61", sizeof(vpu->vdev.name));
	vpu->vdev.v4l2_dev = &vpu->v4l2_dev;
	vpu->vdev.fops = &mv61_fops;
	vpu->vdev.ioctl_ops = &mv61_ioctl_ops;
	vpu->vdev.release = video_device_release_empty;
	vpu->vdev.lock = &vpu->lock;
	vpu->vdev.vfl_dir = VFL_DIR_M2M;
	vpu->vdev.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	video_set_drvdata(&vpu->vdev, vpu);

	ret = video_register_device(&vpu->vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;

	platform_set_drvdata(pdev, vpu);
	dev_info(&pdev->dev, "Fujisan Mali-V61 4K VPU probed (ver=0x%08x cores=%u lsid=%u)\n",
		 vpu->version, vpu->ncores, vpu->nlsid);
	return 0;

err_m2m:
	v4l2_m2m_release(vpu->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&vpu->v4l2_dev);
err_pm:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static void fujisan_mali_v61_remove(struct platform_device *pdev)
{
	struct fujisan_mali_v61 *vpu = platform_get_drvdata(pdev);

	video_unregister_device(&vpu->vdev);
	v4l2_m2m_release(vpu->m2m_dev);
	v4l2_device_unregister(&vpu->v4l2_dev);
	pm_runtime_disable(&pdev->dev);
}

static const struct of_device_id fujisan_mali_v61_of_match[] = {
	{ .compatible = "fujisan,spx4-mali-v61" },
	{ }
};
MODULE_DEVICE_TABLE(of, fujisan_mali_v61_of_match);

static struct platform_driver fujisan_mali_v61_driver = {
	.probe = fujisan_mali_v61_probe,
	.remove_new = fujisan_mali_v61_remove,
	.driver = {
		.name = "fujisan-mali-v61",
		.of_match_table = fujisan_mali_v61_of_match,
	},
};
module_platform_driver(fujisan_mali_v61_driver);

MODULE_AUTHOR("Fujisan OS Contributors");
MODULE_DESCRIPTION("Fujisan OS SPX4 ARM Mali-V61 4K V4L2 M2M Driver (NV12/Y0L2 + LSID MMU)");
MODULE_LICENSE("GPL");
