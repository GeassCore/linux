// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Allwinner A733 G2D driver
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>

#include "sun60i-g2d-formats.h"
#include "sun60i-g2d.h"

#define SUN60I_G2D_JOB_TIMEOUT_MS	500

static inline u32 sun60i_g2d_read(struct sun60i_g2d_dev *dev, u32 reg)
{
	return readl(dev->rot_base + reg);
}

static inline void sun60i_g2d_write(struct sun60i_g2d_dev *dev, u32 reg,
				    u32 value)
{
	writel(value, dev->rot_base + reg);
}

static inline void sun60i_g2d_set_bits(struct sun60i_g2d_dev *dev, u32 reg, u32 bits)
{
	writel(readl(dev->rot_base + reg) | bits, dev->rot_base + reg);
}

static void sun60i_g2d_calc_addr_pitch(dma_addr_t buffer,
				   u32 bytesperline, u32 height,
				   const struct sun60i_g2d_format *fmt,
				   dma_addr_t *addr, u32 *pitch)
{
	u32 size;
	int i;

	memset(addr, 0, 3 * sizeof(*addr));
	memset(pitch, 0, 3 * sizeof(*pitch));

	for (i = 0; i < fmt->planes; i++) {
		pitch[i] = bytesperline;
		addr[i] = buffer;
		if (i > 0)
			pitch[i] /= fmt->hsub / fmt->bpp[i];
		size = pitch[i] * height;
		if (i > 0)
			size /= fmt->vsub;
		buffer += size;
	}
}

static void sun60i_g2d_finish_job(struct sun60i_g2d_ctx *ctx,
				  enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *buffer;

	buffer = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (buffer)
		v4l2_m2m_buf_done(buffer, state);

	buffer = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	if (buffer)
		v4l2_m2m_buf_done(buffer, state);

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

static void sun60i_g2d_start_watchdog(struct sun60i_g2d_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->irqlock, flags);
	dev->job_active = true;
	spin_unlock_irqrestore(&dev->irqlock, flags);
	mod_delayed_work(system_wq, &dev->watchdog,
			 msecs_to_jiffies(SUN60I_G2D_JOB_TIMEOUT_MS));
}

static bool sun60i_g2d_claim_job(struct sun60i_g2d_dev *dev)
{
	unsigned long flags;
	bool active;

	spin_lock_irqsave(&dev->irqlock, flags);
	active = dev->job_active;
	dev->job_active = false;
	spin_unlock_irqrestore(&dev->irqlock, flags);

	return active;
}

static void sun60i_g2d_watchdog(struct work_struct *work)
{
	struct sun60i_g2d_dev *dev =
		container_of(to_delayed_work(work), struct sun60i_g2d_dev,
			     watchdog);
	struct sun60i_g2d_ctx *ctx;
	enum sun60i_g2d_engine engine;

	if (!sun60i_g2d_claim_job(dev))
		return;

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx)
		return;

	engine = dev->engine;
	v4l2_err(&dev->v4l2_dev, "%s processing timeout\n",
		 engine == SUN60I_G2D_ENGINE_MIXER ? "mixer" : "rotate");
	writel(0, dev->base + SUN60I_G2D_TOP_RCQ_IRQ_CTL);
	sun60i_g2d_write(dev, SUN60I_G2D_INT, 0);
	sun60i_g2d_top_reset_engine(dev, engine);
	dev->engine = SUN60I_G2D_ENGINE_NONE;
	sun60i_g2d_finish_job(ctx, VB2_BUF_STATE_ERROR);
}

static void sun60i_g2d_device_run(void *priv)
{
	struct sun60i_g2d_ctx *ctx = priv;
	struct sun60i_g2d_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src, *dst;
	const struct sun60i_g2d_format *fmt;
	dma_addr_t addr[3];
	u32 val, pitch[3];

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	v4l2_m2m_buf_copy_metadata(src, dst, true);

	if (ctx->rotate == 0 && !ctx->hflip && !ctx->vflip &&
	    (ctx->src_fmt.width != ctx->dst_fmt.width ||
	     ctx->src_fmt.height != ctx->dst_fmt.height ||
	     ctx->src_fmt.pixelformat != ctx->dst_fmt.pixelformat)) {
		dev->engine = SUN60I_G2D_ENGINE_MIXER;
		sun60i_g2d_start_watchdog(dev);
		if (!sun60i_g2d_mixer_run(ctx, src, dst))
			return;

		cancel_delayed_work_sync(&dev->watchdog);
		if (sun60i_g2d_claim_job(dev))
			sun60i_g2d_finish_job(ctx, VB2_BUF_STATE_ERROR);
		dev->engine = SUN60I_G2D_ENGINE_NONE;
		return;
	}

	dev->engine = SUN60I_G2D_ENGINE_ROTATE;

	val = SUN60I_G2D_GLB_CTL_MODE(SUN60I_G2D_MODE_COPY_ROTATE);
	if (ctx->hflip)
		val |= SUN60I_G2D_GLB_CTL_HFLIP;
	if (ctx->vflip)
		val |= SUN60I_G2D_GLB_CTL_VFLIP;
	val |= SUN60I_G2D_GLB_CTL_ROTATION(ctx->rotate / 90);
	if (ctx->rotate != 90 && ctx->rotate != 270)
		val |= SUN60I_G2D_GLB_CTL_BURST_LEN(SUN60I_G2D_BURST_64);
	else
		val |= SUN60I_G2D_GLB_CTL_BURST_LEN(SUN60I_G2D_BURST_8);
	sun60i_g2d_write(dev, SUN60I_G2D_GLB_CTL, val);

	fmt = sun60i_g2d_find_format(ctx->src_fmt.pixelformat);
	if (!fmt) {
		sun60i_g2d_finish_job(ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	sun60i_g2d_write(dev, SUN60I_G2D_IN_FMT, SUN60I_G2D_IN_FMT_FORMAT(fmt->hw_format));

	sun60i_g2d_calc_addr_pitch(vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0),
			       ctx->src_fmt.bytesperline, ctx->src_fmt.height,
			       fmt, addr, pitch);

	sun60i_g2d_write(dev, SUN60I_G2D_IN_SIZE,
		     SUN60I_G2D_SIZE(ctx->src_fmt.width, ctx->src_fmt.height));

	sun60i_g2d_write(dev, SUN60I_G2D_IN_PITCH0, pitch[0]);
	sun60i_g2d_write(dev, SUN60I_G2D_IN_PITCH1, pitch[1]);
	sun60i_g2d_write(dev, SUN60I_G2D_IN_PITCH2, pitch[2]);

	sun60i_g2d_write(dev, SUN60I_G2D_IN_ADDRL0, addr[0]);
	sun60i_g2d_write(dev, SUN60I_G2D_IN_ADDRL1, addr[1]);
	sun60i_g2d_write(dev, SUN60I_G2D_IN_ADDRL2, addr[2]);

	sun60i_g2d_write(dev, SUN60I_G2D_IN_ADDRH0, upper_32_bits(addr[0]));
	sun60i_g2d_write(dev, SUN60I_G2D_IN_ADDRH1, upper_32_bits(addr[1]));
	sun60i_g2d_write(dev, SUN60I_G2D_IN_ADDRH2, upper_32_bits(addr[2]));

	fmt = sun60i_g2d_find_format(ctx->dst_fmt.pixelformat);
	if (!fmt) {
		sun60i_g2d_finish_job(ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	sun60i_g2d_calc_addr_pitch(vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0),
			       ctx->dst_fmt.bytesperline, ctx->dst_fmt.height,
			       fmt, addr, pitch);

	sun60i_g2d_write(dev, SUN60I_G2D_OUT_SIZE,
		     SUN60I_G2D_SIZE(ctx->dst_fmt.width, ctx->dst_fmt.height));

	sun60i_g2d_write(dev, SUN60I_G2D_OUT_PITCH0, pitch[0]);
	sun60i_g2d_write(dev, SUN60I_G2D_OUT_PITCH1, pitch[1]);
	sun60i_g2d_write(dev, SUN60I_G2D_OUT_PITCH2, pitch[2]);

	sun60i_g2d_write(dev, SUN60I_G2D_OUT_ADDRL0, addr[0]);
	sun60i_g2d_write(dev, SUN60I_G2D_OUT_ADDRL1, addr[1]);
	sun60i_g2d_write(dev, SUN60I_G2D_OUT_ADDRL2, addr[2]);

	sun60i_g2d_write(dev, SUN60I_G2D_OUT_ADDRH0, upper_32_bits(addr[0]));
	sun60i_g2d_write(dev, SUN60I_G2D_OUT_ADDRH1, upper_32_bits(addr[1]));
	sun60i_g2d_write(dev, SUN60I_G2D_OUT_ADDRH2, upper_32_bits(addr[2]));

	sun60i_g2d_start_watchdog(dev);
	sun60i_g2d_set_bits(dev, SUN60I_G2D_INT,
			     SUN60I_G2D_INT_FINISH_IRQ_EN);
	sun60i_g2d_set_bits(dev, SUN60I_G2D_GLB_CTL, SUN60I_G2D_GLB_CTL_START);
}

static irqreturn_t sun60i_g2d_irq(int irq, void *data)
{
	struct sun60i_g2d_dev *dev = data;
	struct sun60i_g2d_ctx *ctx;
	unsigned int val;

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		v4l2_err(&dev->v4l2_dev,
			 "Instance released before the end of transaction\n");
		return IRQ_NONE;
	}

	if (dev->engine == SUN60I_G2D_ENGINE_MIXER) {
		if (!sun60i_g2d_rcq_irq(dev))
			return IRQ_NONE;
	} else {
		val = sun60i_g2d_read(dev, SUN60I_G2D_INT);
		if (!(val & SUN60I_G2D_INT_FINISH_IRQ))
			return IRQ_NONE;

		/* clear flag and disable irq */
		sun60i_g2d_write(dev, SUN60I_G2D_INT,
				  SUN60I_G2D_INT_FINISH_IRQ);
	}
	if (!sun60i_g2d_claim_job(dev))
		return IRQ_HANDLED;
	cancel_delayed_work_sync(&dev->watchdog);
	dev->engine = SUN60I_G2D_ENGINE_NONE;
	sun60i_g2d_finish_job(ctx, VB2_BUF_STATE_DONE);

	return IRQ_HANDLED;
}

static void sun60i_g2d_job_abort(void *priv)
{
	struct sun60i_g2d_ctx *ctx = priv;

	mod_delayed_work(system_wq, &ctx->dev->watchdog, 0);
}

static inline struct sun60i_g2d_ctx *sun60i_g2d_file2ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct sun60i_g2d_ctx, fh);
}

static void sun60i_g2d_prepare_format(struct v4l2_pix_format *pix_fmt)
{
	unsigned int height, width, alignment, sizeimage, size, bpl;
	const struct sun60i_g2d_format *fmt;
	int i;

	fmt = sun60i_g2d_find_format(pix_fmt->pixelformat);
	if (!fmt)
		return;

	width = ALIGN(pix_fmt->width, fmt->hsub);
	height = ALIGN(pix_fmt->height, fmt->vsub);

	/* all pitches have to be 16 byte aligned */
	alignment = 16;
	if (fmt->planes > 1)
		alignment *= fmt->hsub / fmt->bpp[1];
	bpl = ALIGN(width * fmt->bpp[0], alignment);

	sizeimage = 0;
	for (i = 0; i < fmt->planes; i++) {
		size = bpl * height;
		if (i > 0) {
			size *= fmt->bpp[i];
			size /= fmt->hsub;
			size /= fmt->vsub;
		}
		sizeimage += size;
	}

	pix_fmt->width = width;
	pix_fmt->height = height;
	pix_fmt->bytesperline = bpl;
	pix_fmt->sizeimage = sizeimage;
}

static int sun60i_g2d_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, SUN60I_G2D_NAME, sizeof(cap->driver));
	strscpy(cap->card, SUN60I_G2D_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", SUN60I_G2D_NAME);

	return 0;
}

static int sun60i_g2d_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return sun60i_g2d_enum_fmt(f, true);
}

static int sun60i_g2d_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return sun60i_g2d_enum_fmt(f, false);
}

static int sun60i_g2d_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	const struct sun60i_g2d_format *fmt;

	if (fsize->index != 0)
		return -EINVAL;

	fmt = sun60i_g2d_find_format(fsize->pixel_format);
	if (!fmt)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = SUN60I_G2D_MIN_WIDTH;
	fsize->stepwise.min_height = SUN60I_G2D_MIN_HEIGHT;
	fsize->stepwise.max_width = SUN60I_G2D_MAX_WIDTH;
	fsize->stepwise.max_height = SUN60I_G2D_MAX_HEIGHT;
	fsize->stepwise.step_width = fmt->hsub;
	fsize->stepwise.step_height = fmt->vsub;

	return 0;
}

static int sun60i_g2d_set_cap_format(struct sun60i_g2d_ctx *ctx,
				 struct v4l2_pix_format *f,
				 u32 rotate)
{
	const struct sun60i_g2d_format *fmt;

	fmt = sun60i_g2d_find_format(ctx->src_fmt.pixelformat);
	if (!fmt)
		return -EINVAL;

	if (fmt->flags & SUN60I_G2D_FLAG_YUV)
		f->pixelformat = V4L2_PIX_FMT_YUV420;
	else
		f->pixelformat = ctx->src_fmt.pixelformat;

	f->field = V4L2_FIELD_NONE;

	if (rotate == 90 || rotate == 270) {
		f->width = ctx->src_fmt.height;
		f->height = ctx->src_fmt.width;
	} else {
		f->width = ctx->src_fmt.width;
		f->height = ctx->src_fmt.height;
	}

	sun60i_g2d_prepare_format(f);

	return 0;
}

static int sun60i_g2d_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct sun60i_g2d_ctx *ctx = sun60i_g2d_file2ctx(file);

	f->fmt.pix = ctx->dst_fmt;

	return 0;
}

static int sun60i_g2d_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct sun60i_g2d_ctx *ctx = sun60i_g2d_file2ctx(file);

	f->fmt.pix = ctx->src_fmt;

	return 0;
}

static int sun60i_g2d_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct sun60i_g2d_ctx *ctx = sun60i_g2d_file2ctx(file);
	const struct sun60i_g2d_format *dst_fmt;

	if (ctx->rotate || ctx->hflip || ctx->vflip)
		return sun60i_g2d_set_cap_format(ctx, &f->fmt.pix,
						 ctx->rotate);

	dst_fmt = sun60i_g2d_find_format(f->fmt.pix.pixelformat);
	if (!dst_fmt || !(dst_fmt->flags & SUN60I_G2D_FLAG_OUTPUT))
		f->fmt.pix.pixelformat = ctx->src_fmt.pixelformat;

	f->fmt.pix.width = clamp_t(u32, f->fmt.pix.width,
				   SUN60I_G2D_MIN_WIDTH, 2048);
	f->fmt.pix.height = clamp_t(u32, f->fmt.pix.height,
				    SUN60I_G2D_MIN_HEIGHT, 2048);
	f->fmt.pix.field = V4L2_FIELD_NONE;
	sun60i_g2d_prepare_format(&f->fmt.pix);

	return 0;
}

static int sun60i_g2d_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	if (!sun60i_g2d_find_format(f->fmt.pix.pixelformat))
		f->fmt.pix.pixelformat = V4L2_PIX_FMT_ARGB32;

	if (f->fmt.pix.width < SUN60I_G2D_MIN_WIDTH)
		f->fmt.pix.width = SUN60I_G2D_MIN_WIDTH;
	if (f->fmt.pix.height < SUN60I_G2D_MIN_HEIGHT)
		f->fmt.pix.height = SUN60I_G2D_MIN_HEIGHT;

	if (f->fmt.pix.width > SUN60I_G2D_MAX_WIDTH)
		f->fmt.pix.width = SUN60I_G2D_MAX_WIDTH;
	if (f->fmt.pix.height > SUN60I_G2D_MAX_HEIGHT)
		f->fmt.pix.height = SUN60I_G2D_MAX_HEIGHT;

	f->fmt.pix.field = V4L2_FIELD_NONE;

	sun60i_g2d_prepare_format(&f->fmt.pix);

	return 0;
}

static int sun60i_g2d_s_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct sun60i_g2d_ctx *ctx = sun60i_g2d_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	ret = sun60i_g2d_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ctx->dst_fmt = f->fmt.pix;

	return 0;
}

static int sun60i_g2d_s_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct sun60i_g2d_ctx *ctx = sun60i_g2d_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	ret = sun60i_g2d_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	/*
	 * Capture queue has to be also checked, because format and size
	 * depends on output format and size.
	 */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ctx->src_fmt = f->fmt.pix;

	/* Propagate colorspace information to capture. */
	ctx->dst_fmt.colorspace = f->fmt.pix.colorspace;
	ctx->dst_fmt.xfer_func = f->fmt.pix.xfer_func;
	ctx->dst_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
	ctx->dst_fmt.quantization = f->fmt.pix.quantization;

	return sun60i_g2d_set_cap_format(ctx, &ctx->dst_fmt, ctx->rotate);
}

static const struct v4l2_ioctl_ops sun60i_g2d_ioctl_ops = {
	.vidioc_querycap		= sun60i_g2d_querycap,

	.vidioc_enum_framesizes		= sun60i_g2d_enum_framesizes,

	.vidioc_enum_fmt_vid_cap	= sun60i_g2d_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= sun60i_g2d_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= sun60i_g2d_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= sun60i_g2d_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out	= sun60i_g2d_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out		= sun60i_g2d_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out		= sun60i_g2d_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out		= sun60i_g2d_s_fmt_vid_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_log_status		= v4l2_ctrl_log_status,
	.vidioc_subscribe_event		= v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int sun60i_g2d_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			      unsigned int *nplanes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct sun60i_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (*nplanes) {
		if (sizes[0] < pix_fmt->sizeimage)
			return -EINVAL;
	} else {
		sizes[0] = pix_fmt->sizeimage;
		*nplanes = 1;
	}

	return 0;
}

static int sun60i_g2d_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct sun60i_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < pix_fmt->sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, pix_fmt->sizeimage);

	return 0;
}

static void sun60i_g2d_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sun60i_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void sun60i_g2d_queue_cleanup(struct vb2_queue *vq,
				   enum vb2_buffer_state state)
{
	struct sun60i_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	do {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (vbuf)
			v4l2_m2m_buf_done(vbuf, state);
	} while (vbuf);
}

static int sun60i_g2d_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		struct sun60i_g2d_ctx *ctx = vb2_get_drv_priv(vq);
		struct device *dev = ctx->dev->dev;
		int ret;

		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err(dev, "Failed to enable module\n");
			sun60i_g2d_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
			return ret;
		}
	}

	return 0;
}

static void sun60i_g2d_stop_streaming(struct vb2_queue *vq)
{
	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		struct sun60i_g2d_ctx *ctx = vb2_get_drv_priv(vq);

		pm_runtime_put(ctx->dev->dev);
	}

	sun60i_g2d_queue_cleanup(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops sun60i_g2d_qops = {
	.queue_setup		= sun60i_g2d_queue_setup,
	.buf_prepare		= sun60i_g2d_buf_prepare,
	.buf_queue		= sun60i_g2d_buf_queue,
	.start_streaming	= sun60i_g2d_start_streaming,
	.stop_streaming		= sun60i_g2d_stop_streaming,
};

static int sun60i_g2d_queue_init(void *priv, struct vb2_queue *src_vq,
			     struct vb2_queue *dst_vq)
{
	struct sun60i_g2d_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->min_queued_buffers = 1;
	src_vq->ops = &sun60i_g2d_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->min_queued_buffers = 2;
	dst_vq->ops = &sun60i_g2d_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(dst_vq);
	if (ret)
		return ret;

	return 0;
}

static int sun60i_g2d_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sun60i_g2d_ctx *ctx = container_of(ctrl->handler,
					      struct sun60i_g2d_ctx,
					      ctrl_handler);
	struct v4l2_pix_format fmt;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		ctx->hflip = ctrl->val;
		break;
	case V4L2_CID_VFLIP:
		ctx->vflip = ctrl->val;
		break;
	case V4L2_CID_ROTATE:
		sun60i_g2d_set_cap_format(ctx, &fmt, ctrl->val);

		/* Check if capture format needs to be changed */
		if (fmt.width != ctx->dst_fmt.width ||
		    fmt.height != ctx->dst_fmt.height ||
		    fmt.bytesperline != ctx->dst_fmt.bytesperline ||
		    fmt.sizeimage != ctx->dst_fmt.sizeimage) {
			struct vb2_queue *vq;

			vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
					     V4L2_BUF_TYPE_VIDEO_CAPTURE);
			if (vb2_is_busy(vq))
				return -EBUSY;

			sun60i_g2d_set_cap_format(ctx, &ctx->dst_fmt, ctrl->val);
		}

		ctx->rotate = ctrl->val;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops sun60i_g2d_ctrl_ops = {
	.s_ctrl = sun60i_g2d_s_ctrl,
};

static int sun60i_g2d_setup_ctrls(struct sun60i_g2d_ctx *ctx)
{
	v4l2_ctrl_handler_init(&ctx->ctrl_handler, 3);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sun60i_g2d_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sun60i_g2d_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std(&ctx->ctrl_handler, &sun60i_g2d_ctrl_ops,
			  V4L2_CID_ROTATE, 0, 270, 90, 0);

	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;

		v4l2_err(&ctx->dev->v4l2_dev, "control setup failed!\n");
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);

		return err;
	}

	return 0;
}

static int sun60i_g2d_open(struct file *file)
{
	struct sun60i_g2d_dev *dev = video_drvdata(file);
	struct sun60i_g2d_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&dev->dev_mutex);
		return -ENOMEM;
	}

	/* default output format */
	ctx->src_fmt.pixelformat = V4L2_PIX_FMT_ARGB32;
	ctx->src_fmt.field = V4L2_FIELD_NONE;
	ctx->src_fmt.width = 640;
	ctx->src_fmt.height = 480;
	sun60i_g2d_prepare_format(&ctx->src_fmt);

	/* default capture format */
	sun60i_g2d_set_cap_format(ctx, &ctx->dst_fmt, ctx->rotate);

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->dev = dev;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &sun60i_g2d_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_fh;
	}

	ret = sun60i_g2d_setup_ctrls(ctx);
	if (ret)
		goto err_m2m;

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
	if (ret)
		goto err_ctrls;

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	v4l2_fh_add(&ctx->fh, file);

	mutex_unlock(&dev->dev_mutex);

	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
err_m2m:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static int sun60i_g2d_release(struct file *file)
{
	struct sun60i_g2d_dev *dev = video_drvdata(file);
	struct sun60i_g2d_ctx *ctx = sun60i_g2d_file2ctx(file);

	mutex_lock(&dev->dev_mutex);

	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);

	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations sun60i_g2d_fops = {
	.owner		= THIS_MODULE,
	.open		= sun60i_g2d_open,
	.release	= sun60i_g2d_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device sun60i_g2d_video_device = {
	.name		= SUN60I_G2D_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &sun60i_g2d_fops,
	.ioctl_ops	= &sun60i_g2d_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops sun60i_g2d_m2m_ops = {
	.device_run	= sun60i_g2d_device_run,
	.job_abort	= sun60i_g2d_job_abort,
};

static int sun60i_g2d_probe(struct platform_device *pdev)
{
	struct sun60i_g2d_dev *dev;
	struct video_device *vfd;
	int irq, ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->vfd = sun60i_g2d_video_device;
	dev->dev = &pdev->dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);
	dev->rot_base = dev->base + SUN60I_G2D_ROT_OFFSET;

	dev->clks[0].id = "bus";
	dev->clks[1].id = "g2d";
	dev->clks[2].id = "mbus";
	dev->clks[3].id = "ahb";
	ret = devm_clk_bulk_get(dev->dev, ARRAY_SIZE(dev->clks), dev->clks);
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to get clocks\n");

	dev->resets[0].id = "g2d";
	dev->resets[1].id = "de";
	ret = devm_reset_control_bulk_get_shared(dev->dev,
					       ARRAY_SIZE(dev->resets),
					       dev->resets);
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to get resets\n");

	ret = dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(40));
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to set DMA mask\n");

	ret = sun60i_g2d_rcq_init(dev);
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to allocate RCQ\n");

	mutex_init(&dev->dev_mutex);
	spin_lock_init(&dev->irqlock);
	INIT_DELAYED_WORK(&dev->watchdog, sun60i_g2d_watchdog);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(dev->dev, "Failed to register V4L2 device\n");

		goto err_rcq;
	}

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	snprintf(vfd->name, sizeof(vfd->name), "%s",
		 sun60i_g2d_video_device.name);
	video_set_drvdata(vfd, dev);

	dev->m2m_dev = v4l2_m2m_init(&sun60i_g2d_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(dev->m2m_dev);

		goto err_v4l2;
	}

	ret = devm_request_threaded_irq(dev->dev, irq, NULL, sun60i_g2d_irq,
					IRQF_ONESHOT, dev_name(dev->dev), dev);
	if (ret) {
		dev_err_probe(dev->dev, ret, "Failed to request IRQ\n");
		goto err_m2m;
	}

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto err_m2m;
	}

	v4l2_info(&dev->v4l2_dev,
		  "Device registered as /dev/video%d\n", vfd->num);

	platform_set_drvdata(pdev, dev);

	pm_runtime_enable(dev->dev);

	return 0;

err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_rcq:
	sun60i_g2d_rcq_cleanup(dev);

	return ret;
}

static void sun60i_g2d_remove(struct platform_device *pdev)
{
	struct sun60i_g2d_dev *dev = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&dev->watchdog);
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	sun60i_g2d_rcq_cleanup(dev);

	pm_runtime_force_suspend(&pdev->dev);
}

static int sun60i_g2d_runtime_resume(struct device *device)
{
	struct sun60i_g2d_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(dev->clks), dev->clks);
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to enable clocks\n");

	ret = reset_control_bulk_deassert(ARRAY_SIZE(dev->resets), dev->resets);
	if (ret)
		goto err_clks;

	sun60i_g2d_top_enable(dev);

	return 0;

err_clks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(dev->clks), dev->clks);

	return ret;
}

static int sun60i_g2d_runtime_suspend(struct device *device)
{
	struct sun60i_g2d_dev *dev = dev_get_drvdata(device);

	sun60i_g2d_top_disable(dev);
	reset_control_bulk_assert(ARRAY_SIZE(dev->resets), dev->resets);
	clk_bulk_disable_unprepare(ARRAY_SIZE(dev->clks), dev->clks);

	return 0;
}

static const struct of_device_id sun60i_g2d_dt_match[] = {
	{ .compatible = "allwinner,sun60i-a733-g2d" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun60i_g2d_dt_match);

static const struct dev_pm_ops sun60i_g2d_pm_ops = {
	.runtime_resume		= sun60i_g2d_runtime_resume,
	.runtime_suspend	= sun60i_g2d_runtime_suspend,
};

static struct platform_driver sun60i_g2d_driver = {
	.probe		= sun60i_g2d_probe,
	.remove		= sun60i_g2d_remove,
	.driver		= {
		.name		= SUN60I_G2D_NAME,
		.of_match_table	= sun60i_g2d_dt_match,
		.pm		= &sun60i_g2d_pm_ops,
	},
};
module_platform_driver(sun60i_g2d_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Allwinner A733 G2D V4L2 mem2mem driver");
