// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fujisan SPX4 Hardware JPEG Encoder & Decoder V4L2 M2M Driver
 *
 * Wave-3 Hardware Verification (DAT @ section4.bin 0x011FE788..0x011FF878,
 * section5.bin 0x021E0F0C..0x021E1AF0):
 *   - Hardware JPEG Encoder:
 *       MMIO: 0xFFFA0000..0xFFFA0133 (22 DAT registers)
 *       34-bit DMA Upper Bits: 0xFFF4A740 (src PA[33:32]), 0xFFF4A744 (dst PA[33:32])
 *       GIC IRQ: 135 (DT SPI <0 103 4>)
 *       V4L2 M2M node: UYVY (OUTPUT) -> JPEG (CAPTURE)
 *   - Hardware JPEG Decoder:
 *       MMIO: 0xFFFA1000..0xFFFA112B (19 DAT registers, OFF_MODE +0x11C = 0)
 *       GIC IRQ: 136 (DT SPI <0 104 4>)
 *       V4L2 M2M node: JPEG (OUTPUT) -> UYVY (CAPTURE)
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "fuji_spx4_hw.h"

#define SPX4_JPEG_MIN_W		16U
#define SPX4_JPEG_MAX_W		11648U
#define SPX4_JPEG_MIN_H		16U
#define SPX4_JPEG_MAX_H		8736U

/* Standard Annex K luminance / chrominance quantization tables (64 bytes each) */
static const u8 spx4_default_luma_qtbl[64] = {
	16, 11, 10, 16, 24,  40,  51,  61,
	12, 12, 14, 19, 26,  58,  60,  55,
	14, 13, 16, 24, 40,  57,  69,  56,
	14, 17, 22, 29, 51,  87,  80,  62,
	18, 22, 37, 56, 68, 109, 103,  77,
	24, 35, 55, 64, 81, 104, 113,  92,
	49, 64, 78, 87, 103, 121, 120, 101,
	72, 92, 95, 98, 112, 100, 103,  99,
};

static const u8 spx4_default_chroma_qtbl[64] = {
	17, 18, 24, 47, 99, 99, 99, 99,
	18, 21, 26, 66, 99, 99, 99, 99,
	24, 26, 56, 99, 99, 99, 99, 99,
	47, 66, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99,
};

struct fuji_spx4_jpeg {
	struct device *dev;
	void __iomem *enc_base;
	void __iomem *dec_base;
	void __iomem *dma_hi_base;
	int enc_irq;
	int dec_irq;
	struct mutex lock;
	struct v4l2_device v4l2_dev;
	struct video_device enc_vdev;
	struct video_device dec_vdev;
	struct v4l2_m2m_dev *enc_m2m;
	struct v4l2_m2m_dev *dec_m2m;
	u32 last_enc_max_bytes;
};

struct spx4_jpeg_ctx {
	struct v4l2_fh fh;
	struct fuji_spx4_jpeg *jpeg;
	struct v4l2_ctrl_handler ctrl_hdl;
	bool is_encoder;
	u32 width;
	u32 height;
	u32 quality;
	u32 restart_interval;
	u32 jpeg_buf_size;
};

static void spx4_jenc_soft_reset(struct fuji_spx4_jpeg *jpeg)
{
	int tries = 200;

	/* DAT @ section5.bin 0x021E1564..0x021E15EC */
	writel_relaxed(0x3U, jpeg->enc_base + SPX4_JENC_DMA_CTRL);
	writel_relaxed(0x0U, jpeg->enc_base + SPX4_JENC_DMA_CTRL);
	writel_relaxed(SPX4_JENC_CTRL_RESET, jpeg->enc_base + SPX4_JENC_CTRL);
	while ((readl_relaxed(jpeg->enc_base + SPX4_JENC_CTRL) & SPX4_JENC_CTRL_RESET) &&
	       --tries > 0)
		udelay(5);
}

static void spx4_jenc_load_qtbl(struct fuji_spx4_jpeg *jpeg, u32 table_sel, const u8 q64[64])
{
	unsigned int i;

	writel_relaxed(SPX4_JENC_QTBL_LOAD_EN | (table_sel & 1U),
		       jpeg->enc_base + SPX4_JENC_QTBL_CTRL);
	for (i = 0; i < 64; i += 4)
		writel_relaxed(spx4_jpeg_pack_qtbl_word(&q64[i]),
			       jpeg->enc_base + SPX4_JENC_QTBL_DATA);
	writel_relaxed(0U, jpeg->enc_base + SPX4_JENC_QTBL_CTRL);
}

static void spx4_jenc_device_run(void *priv)
{
	struct spx4_jpeg_ctx *ctx = priv;
	struct fuji_spx4_jpeg *jpeg = ctx->jpeg;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	dma_addr_t src_pa, dst_pa;
	u32 src_lo, src_hi, dst_lo, dst_hi;
	u32 dst_max, pitch, dim;
	u8 qscale;

	if (!src || !dst)
		return;

	src_pa = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dst_pa = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	dst_max = (u32)(vb2_plane_size(&dst->vb2_buf, 0) & ~7UL);
	pitch = ctx->width * 2U; /* UYVY packed 4:2:2 */
	dim = spx4_jpeg_pack_dim((u16)ctx->width, (u16)ctx->height);
	qscale = (u8)clamp_t(u32, (100U - ctx->quality) / 6U, 1U, 15U);

	spx4_dma34_split((u64)src_pa, &src_lo, &src_hi);
	spx4_dma34_split((u64)dst_pa, &dst_lo, &dst_hi);

	spx4_jenc_soft_reset(jpeg);

	/* DAT @ section5.bin 0x021E173C..0x021E1AB0 */
	writel_relaxed(SPX4_JENC_FORMAT_DEFAULT, jpeg->enc_base + SPX4_JENC_FORMAT_CTRL);
	writel_relaxed(SPX4_JENC_SUBSAMPLE_422, jpeg->enc_base + SPX4_JENC_SUBSAMPLE);
	writel_relaxed(dim, jpeg->enc_base + SPX4_JENC_SRC_SIZE);
	writel_relaxed(dim, jpeg->enc_base + SPX4_JENC_WINDOW_DIM);
	writel_relaxed(pitch, jpeg->enc_base + SPX4_JENC_LINE_PITCH);
	writel_relaxed(spx4_jpeg_pack_qscale(qscale, qscale), jpeg->enc_base + SPX4_JENC_QSCALE);
	writel_relaxed(ctx->restart_interval & 0xFFFFU, jpeg->enc_base + SPX4_JENC_DRI);
	writel_relaxed(0U, jpeg->enc_base + SPX4_JENC_SWAP_CTRL);
	writel_relaxed(0U, jpeg->enc_base + SPX4_JENC_MARKER_CTRL);
	writel_relaxed(0U, jpeg->enc_base + SPX4_JENC_MISC);

	spx4_jenc_load_qtbl(jpeg, SPX4_JENC_QTBL_SEL_LUMA, spx4_default_luma_qtbl);
	spx4_jenc_load_qtbl(jpeg, SPX4_JENC_QTBL_SEL_CHROMA, spx4_default_chroma_qtbl);

	/* 34-bit DMA address programming: low 32b + 0xFFF4A740/44 bits[1:0] */
	if (jpeg->dma_hi_base) {
		writel_relaxed(src_hi, jpeg->dma_hi_base + SPX4_JPEG_DMA_HI_SRC_OFF);
		writel_relaxed(dst_hi, jpeg->dma_hi_base + SPX4_JPEG_DMA_HI_DST_OFF);
	}
	writel_relaxed(src_lo, jpeg->enc_base + SPX4_JENC_SRC_YC_ADDR);
	writel_relaxed(dst_lo, jpeg->enc_base + SPX4_JENC_DST_JPEG_ADDR);
	writel_relaxed(dst_max, jpeg->enc_base + SPX4_JENC_DST_JPEG_MAX);
	jpeg->last_enc_max_bytes = dst_max;

	writel_relaxed(SPX4_JENC_MODE_ENCODE, jpeg->enc_base + SPX4_JENC_MODE);
	/* W1C pending status, unmask ENC_DONE (bit 4) + error (bit 17) */
	writel_relaxed(0xFFFFFFFFU, jpeg->enc_base + SPX4_JENC_INT_STATUS);
	writel_relaxed(~(SPX4_JENC_INT_DONE | SPX4_JENC_INT_ERR),
		       jpeg->enc_base + SPX4_JENC_INT_MASK);

	/* Kick hardware JPEG encode (IRQ 135 signals completion) */
	writel_relaxed(SPX4_JENC_CTRL_START, jpeg->enc_base + SPX4_JENC_CTRL);
}

static void spx4_jdec_device_run(void *priv)
{
	struct spx4_jpeg_ctx *ctx = priv;
	struct fuji_spx4_jpeg *jpeg = ctx->jpeg;
	struct vb2_v4l2_buffer *src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	struct vb2_v4l2_buffer *dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	dma_addr_t src_pa, dst_pa;

	if (!src || !dst || !jpeg->dec_base)
		return;

	src_pa = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dst_pa = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);

	/* DAT @ section5.bin 0x021E11E8: decoder OFF_MODE (+0x11C) is always 0 */
	writel_relaxed(0U, jpeg->dec_base + SPX4_JDEC_OFF_MODE);
	writel_relaxed(spx4_jpeg_pack_dim((u16)ctx->width, (u16)ctx->height),
		       jpeg->dec_base + SPX4_JDEC_IMG_SIZE);
	writel_relaxed(ctx->width * 2U, jpeg->dec_base + SPX4_JDEC_LINE_PITCH);
	writel_relaxed((u32)src_pa, jpeg->dec_base + SPX4_JDEC_SRC_JPEG_ADDR);
	writel_relaxed((u32)dst_pa, jpeg->dec_base + SPX4_JDEC_DST_YC_ADDR);
	writel_relaxed(0xFFFFFFFFU, jpeg->dec_base + SPX4_JDEC_INT_STATUS);
	writel_relaxed(~SPX4_JENC_INT_DONE, jpeg->dec_base + SPX4_JDEC_INT_MASK);
	writel_relaxed(SPX4_JENC_CTRL_START, jpeg->dec_base + SPX4_JDEC_CTRL);
}

static irqreturn_t spx4_jenc_irq_handler(int irq, void *dev_id)
{
	struct fuji_spx4_jpeg *jpeg = dev_id;
	struct spx4_jpeg_ctx *ctx;
	struct vb2_v4l2_buffer *src, *dst;
	u32 status, out_bytes;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;

	status = readl_relaxed(jpeg->enc_base + SPX4_JENC_INT_STATUS);
	if (!status)
		return IRQ_NONE;

	/* W1C interrupt status (DAT @ section5.bin 0x021E1434) */
	writel_relaxed(status, jpeg->enc_base + SPX4_JENC_INT_STATUS);

	ctx = v4l2_m2m_get_curr_priv(jpeg->enc_m2m);
	if (!ctx)
		return IRQ_HANDLED;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	out_bytes = readl_relaxed(jpeg->enc_base + SPX4_JENC_OUT_BYTES);

	if (!(status & SPX4_JENC_INT_DONE) || (status & SPX4_JENC_INT_ERR_MASK) ||
	    out_bytes == 0 || out_bytes > jpeg->last_enc_max_bytes)
		state = VB2_BUF_STATE_ERROR;

	if (dst) {
		if (state == VB2_BUF_STATE_DONE)
			vb2_set_plane_payload(&dst->vb2_buf, 0, out_bytes);
		if (src)
			v4l2_m2m_buf_copy_metadata(src, dst, true);
		v4l2_m2m_buf_done(dst, state);
	}
	if (src)
		v4l2_m2m_buf_done(src, state);

	v4l2_m2m_job_finish(jpeg->enc_m2m, ctx->fh.m2m_ctx);
	return IRQ_HANDLED;
}

static irqreturn_t spx4_jdec_irq_handler(int irq, void *dev_id)
{
	struct fuji_spx4_jpeg *jpeg = dev_id;
	struct spx4_jpeg_ctx *ctx;
	struct vb2_v4l2_buffer *src, *dst;
	u32 status;
	enum vb2_buffer_state state = VB2_BUF_STATE_DONE;

	status = readl_relaxed(jpeg->dec_base + SPX4_JDEC_INT_STATUS);
	if (!status)
		return IRQ_NONE;

	writel_relaxed(status, jpeg->dec_base + SPX4_JDEC_INT_STATUS);

	ctx = v4l2_m2m_get_curr_priv(jpeg->dec_m2m);
	if (!ctx)
		return IRQ_HANDLED;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	if (!(status & SPX4_JENC_INT_DONE) || (status & SPX4_JENC_INT_ERR_MASK))
		state = VB2_BUF_STATE_ERROR;

	if (dst) {
		if (state == VB2_BUF_STATE_DONE)
			vb2_set_plane_payload(&dst->vb2_buf, 0, ctx->width * ctx->height * 2U);
		if (src)
			v4l2_m2m_buf_copy_metadata(src, dst, true);
		v4l2_m2m_buf_done(dst, state);
	}
	if (src)
		v4l2_m2m_buf_done(src, state);

	v4l2_m2m_job_finish(jpeg->dec_m2m, ctx->fh.m2m_ctx);
	return IRQ_HANDLED;
}

static const struct v4l2_m2m_ops spx4_jenc_m2m_ops = {
	.device_run = spx4_jenc_device_run,
};

static const struct v4l2_m2m_ops spx4_jdec_m2m_ops = {
	.device_run = spx4_jdec_device_run,
};

static int spx4_jpeg_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct spx4_jpeg_ctx *ctx = container_of(ctrl->handler, struct spx4_jpeg_ctx, ctrl_hdl);

	switch (ctrl->id) {
	case V4L2_CID_JPEG_COMPRESSION_QUALITY:
		ctx->quality = ctrl->val;
		return 0;
	case V4L2_CID_JPEG_RESTART_INTERVAL:
		ctx->restart_interval = ctrl->val;
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops spx4_jpeg_ctrl_ops = {
	.s_ctrl = spx4_jpeg_s_ctrl,
};

static int spx4_jpeg_querycap(struct file *file, void *priv, struct v4l2_capability *cap)
{
	struct spx4_jpeg_ctx *ctx = container_of(priv, struct spx4_jpeg_ctx, fh);

	strscpy(cap->driver, "fuji-spx4-jpeg", sizeof(cap->driver));
	strscpy(cap->card,
		ctx->is_encoder ? "Fujisan SPX4 HW JPEG Encoder" :
				  "Fujisan SPX4 HW JPEG Decoder",
		sizeof(cap->card));
	strscpy(cap->bus_info, "platform:fuji-spx4-jpeg", sizeof(cap->bus_info));
	return 0;
}

static u32 spx4_jpeg_fourcc_for_type(const struct spx4_jpeg_ctx *ctx, u32 type)
{
	bool is_out = V4L2_TYPE_IS_OUTPUT(type);

	if (ctx->is_encoder)
		return is_out ? V4L2_PIX_FMT_UYVY : V4L2_PIX_FMT_JPEG;
	return is_out ? V4L2_PIX_FMT_JPEG : V4L2_PIX_FMT_UYVY;
}

static int spx4_jpeg_enum_fmt(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	struct spx4_jpeg_ctx *ctx = container_of(priv, struct spx4_jpeg_ctx, fh);

	if (f->index > 0)
		return -EINVAL;
	f->pixelformat = spx4_jpeg_fourcc_for_type(ctx, f->type);
	return 0;
}

static int spx4_jpeg_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct spx4_jpeg_ctx *ctx = container_of(priv, struct spx4_jpeg_ctx, fh);
	u32 fourcc = spx4_jpeg_fourcc_for_type(ctx, f->type);

	f->fmt.pix.width = ctx->width;
	f->fmt.pix.height = ctx->height;
	f->fmt.pix.pixelformat = fourcc;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
	if (fourcc == V4L2_PIX_FMT_UYVY) {
		f->fmt.pix.bytesperline = ctx->width * 2U;
		f->fmt.pix.sizeimage = ctx->width * ctx->height * 2U;
	} else {
		f->fmt.pix.bytesperline = 0;
		f->fmt.pix.sizeimage = ctx->jpeg_buf_size;
	}
	return 0;
}

static int spx4_jpeg_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct spx4_jpeg_ctx *ctx = container_of(priv, struct spx4_jpeg_ctx, fh);
	u32 fourcc = spx4_jpeg_fourcc_for_type(ctx, f->type);

	ctx->width = round_up(clamp_t(u32, f->fmt.pix.width, SPX4_JPEG_MIN_W, SPX4_JPEG_MAX_W), 16U);
	ctx->height = round_up(clamp_t(u32, f->fmt.pix.height, SPX4_JPEG_MIN_H, SPX4_JPEG_MAX_H), 8U);
	if (fourcc == V4L2_PIX_FMT_JPEG && f->fmt.pix.sizeimage >= 4096U)
		ctx->jpeg_buf_size = round_up(f->fmt.pix.sizeimage, 8U);
	else
		ctx->jpeg_buf_size = round_up(ctx->width * ctx->height * 2U, 8U);

	return spx4_jpeg_g_fmt(file, priv, f);
}

static const struct v4l2_ioctl_ops spx4_jpeg_ioctl_ops = {
	.vidioc_querycap		= spx4_jpeg_querycap,
	.vidioc_enum_fmt_vid_out	= spx4_jpeg_enum_fmt,
	.vidioc_enum_fmt_vid_cap	= spx4_jpeg_enum_fmt,
	.vidioc_g_fmt_vid_out		= spx4_jpeg_g_fmt,
	.vidioc_g_fmt_vid_cap		= spx4_jpeg_g_fmt,
	.vidioc_try_fmt_vid_out		= spx4_jpeg_s_fmt,
	.vidioc_try_fmt_vid_cap		= spx4_jpeg_s_fmt,
	.vidioc_s_fmt_vid_out		= spx4_jpeg_s_fmt,
	.vidioc_s_fmt_vid_cap		= spx4_jpeg_s_fmt,
	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,
};

static int spx4_jpeg_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				 unsigned int *nplanes, unsigned int sizes[],
				 struct device *alloc_devs[])
{
	struct spx4_jpeg_ctx *ctx = vb2_get_drv_priv(vq);
	u32 fourcc = spx4_jpeg_fourcc_for_type(ctx, vq->type);
	u32 sz = (fourcc == V4L2_PIX_FMT_UYVY) ? (ctx->width * ctx->height * 2U) :
						 ctx->jpeg_buf_size;

	if (*nplanes)
		return sizes[0] < sz ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = sz;
	if (*nbuffers < 1)
		*nbuffers = 1;
	return 0;
}

static void spx4_jpeg_buf_queue(struct vb2_buffer *vb)
{
	struct spx4_jpeg_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static const struct vb2_ops spx4_jpeg_qops = {
	.queue_setup	= spx4_jpeg_queue_setup,
	.buf_queue	= spx4_jpeg_buf_queue,
	.wait_prepare	= vb2_ops_wait_prepare,
	.wait_finish	= vb2_ops_wait_finish,
};

static int spx4_jpeg_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct spx4_jpeg_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &spx4_jpeg_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->jpeg->lock;
	src_vq->dev = ctx->jpeg->dev;
	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &spx4_jpeg_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->jpeg->lock;
	dst_vq->dev = ctx->jpeg->dev;
	return vb2_queue_init(dst_vq);
}

static int spx4_jpeg_open(struct file *file)
{
	struct video_device *vdev = video_devdata(file);
	struct fuji_spx4_jpeg *jpeg = video_drvdata(file);
	struct spx4_jpeg_ctx *ctx;
	struct v4l2_m2m_dev *m2m;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	v4l2_fh_init(&ctx->fh, vdev);
	ctx->jpeg = jpeg;
	ctx->is_encoder = (vdev == &jpeg->enc_vdev);
	ctx->width = 1024U;
	ctx->height = 768U;
	ctx->quality = 90U;
	ctx->restart_interval = 0U;
	ctx->jpeg_buf_size = ctx->width * ctx->height * 2U;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, 2);
	if (ctx->is_encoder) {
		v4l2_ctrl_new_std(&ctx->ctrl_hdl, &spx4_jpeg_ctrl_ops,
				  V4L2_CID_JPEG_COMPRESSION_QUALITY, 1, 100, 1, 90);
		v4l2_ctrl_new_std(&ctx->ctrl_hdl, &spx4_jpeg_ctrl_ops,
				  V4L2_CID_JPEG_RESTART_INTERVAL, 0, 0xFFFF, 1, 0);
	}
	if (ctx->ctrl_hdl.error) {
		ret = ctx->ctrl_hdl.error;
		goto err_ctrl;
	}
	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;

	m2m = ctx->is_encoder ? jpeg->enc_m2m : jpeg->dec_m2m;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(m2m, ctx, spx4_jpeg_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrl;
	}

	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);
	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int spx4_jpeg_release(struct file *file)
{
	struct spx4_jpeg_ctx *ctx = container_of(file->private_data, struct spx4_jpeg_ctx, fh);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations spx4_jpeg_fops = {
	.owner		= THIS_MODULE,
	.open		= spx4_jpeg_open,
	.release	= spx4_jpeg_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static int fuji_spx4_jpeg_probe(struct platform_device *pdev)
{
	struct fuji_spx4_jpeg *jpeg;
	struct resource *res;
	int ret;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(34));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret)
			return ret;
	}

	jpeg = devm_kzalloc(&pdev->dev, sizeof(*jpeg), GFP_KERNEL);
	if (!jpeg)
		return -ENOMEM;

	jpeg->dev = &pdev->dev;
	mutex_init(&jpeg->lock);

	/* Reg 0: Hardware JPEG Encoder (0xFFFA0000..0xFFFA0133) */
	jpeg->enc_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(jpeg->enc_base))
		return PTR_ERR(jpeg->enc_base);

	/* Reg 1: Hardware JPEG Decoder (0xFFFA1000..0xFFFA112B) */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		jpeg->dec_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(jpeg->dec_base))
			return PTR_ERR(jpeg->dec_base);
	}

	/* Reg 2: 34-bit DMA Upper-Address Bank (0xFFF4A740..0xFFF4A747) */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (res) {
		jpeg->dma_hi_base = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(jpeg->dma_hi_base))
			jpeg->dma_hi_base = NULL;
	}

	jpeg->enc_irq = platform_get_irq_optional(pdev, 0);
	if (jpeg->enc_irq > 0) {
		ret = devm_request_irq(&pdev->dev, jpeg->enc_irq, spx4_jenc_irq_handler,
				       IRQF_SHARED, "fuji-spx4-jpeg-enc", jpeg);
		if (ret)
			return ret;
	}

	jpeg->dec_irq = platform_get_irq_optional(pdev, 1);
	if (jpeg->dec_irq > 0 && jpeg->dec_base) {
		ret = devm_request_irq(&pdev->dev, jpeg->dec_irq, spx4_jdec_irq_handler,
				       IRQF_SHARED, "fuji-spx4-jpeg-dec", jpeg);
		if (ret)
			return ret;
	}

	ret = v4l2_device_register(&pdev->dev, &jpeg->v4l2_dev);
	if (ret)
		return ret;

	jpeg->enc_m2m = v4l2_m2m_init(&spx4_jenc_m2m_ops);
	if (IS_ERR(jpeg->enc_m2m)) {
		ret = PTR_ERR(jpeg->enc_m2m);
		goto err_v4l2;
	}

	strscpy(jpeg->enc_vdev.name, "fuji-spx4-jpeg-enc", sizeof(jpeg->enc_vdev.name));
	jpeg->enc_vdev.fops = &spx4_jpeg_fops;
	jpeg->enc_vdev.ioctl_ops = &spx4_jpeg_ioctl_ops;
	jpeg->enc_vdev.minor = -1;
	jpeg->enc_vdev.release = video_device_release_empty;
	jpeg->enc_vdev.lock = &jpeg->lock;
	jpeg->enc_vdev.v4l2_dev = &jpeg->v4l2_dev;
	jpeg->enc_vdev.vfl_dir = VFL_DIR_M2M;
	jpeg->enc_vdev.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	video_set_drvdata(&jpeg->enc_vdev, jpeg);

	ret = video_register_device(&jpeg->enc_vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_enc_m2m;

	if (jpeg->dec_base) {
		jpeg->dec_m2m = v4l2_m2m_init(&spx4_jdec_m2m_ops);
		if (IS_ERR(jpeg->dec_m2m)) {
			ret = PTR_ERR(jpeg->dec_m2m);
			goto err_enc_vdev;
		}

		strscpy(jpeg->dec_vdev.name, "fuji-spx4-jpeg-dec", sizeof(jpeg->dec_vdev.name));
		jpeg->dec_vdev.fops = &spx4_jpeg_fops;
		jpeg->dec_vdev.ioctl_ops = &spx4_jpeg_ioctl_ops;
		jpeg->dec_vdev.minor = -1;
		jpeg->dec_vdev.release = video_device_release_empty;
		jpeg->dec_vdev.lock = &jpeg->lock;
		jpeg->dec_vdev.v4l2_dev = &jpeg->v4l2_dev;
		jpeg->dec_vdev.vfl_dir = VFL_DIR_M2M;
		jpeg->dec_vdev.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
		video_set_drvdata(&jpeg->dec_vdev, jpeg);

		ret = video_register_device(&jpeg->dec_vdev, VFL_TYPE_VIDEO, -1);
		if (ret)
			goto err_dec_m2m;
	}

	platform_set_drvdata(pdev, jpeg);
	dev_info(&pdev->dev,
		 "Fujisan SPX4 HW JPEG V4L2 M2M registered (enc=/dev/video%d irq=%d, dec=/dev/video%d irq=%d)\n",
		 jpeg->enc_vdev.num, jpeg->enc_irq,
		 jpeg->dec_base ? jpeg->dec_vdev.num : -1, jpeg->dec_irq);
	return 0;

err_dec_m2m:
	v4l2_m2m_release(jpeg->dec_m2m);
err_enc_vdev:
	video_unregister_device(&jpeg->enc_vdev);
err_enc_m2m:
	v4l2_m2m_release(jpeg->enc_m2m);
err_v4l2:
	v4l2_device_unregister(&jpeg->v4l2_dev);
	return ret;
}

static int fuji_spx4_jpeg_remove(struct platform_device *pdev)
{
	struct fuji_spx4_jpeg *jpeg = platform_get_drvdata(pdev);

	if (jpeg->dec_base) {
		video_unregister_device(&jpeg->dec_vdev);
		v4l2_m2m_release(jpeg->dec_m2m);
	}
	video_unregister_device(&jpeg->enc_vdev);
	v4l2_m2m_release(jpeg->enc_m2m);
	v4l2_device_unregister(&jpeg->v4l2_dev);
	return 0;
}

static const struct of_device_id fuji_spx4_jpeg_of_match[] = {
	{ .compatible = "fujisan,spx4-jpeg" },
	{ }
};
MODULE_DEVICE_TABLE(of, fuji_spx4_jpeg_of_match);

static struct platform_driver fuji_spx4_jpeg_driver = {
	.probe	= fuji_spx4_jpeg_probe,
	.remove	= fuji_spx4_jpeg_remove,
	.driver	= {
		.name		= "fuji-spx4-jpeg",
		.of_match_table	= fuji_spx4_jpeg_of_match,
	},
};
module_platform_driver(fuji_spx4_jpeg_driver);

MODULE_DESCRIPTION("Fujisan SPX4 Hardware JPEG Encoder (0xFFFA0000) & Decoder (0xFFFA1000) V4L2 M2M driver");
MODULE_LICENSE("GPL");
