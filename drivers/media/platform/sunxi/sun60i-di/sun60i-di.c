// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner A733 DI301 deinterlacer driver
 *
 * The V4L2 mem2mem plumbing is based on the sun8i-di driver. The DI301
 * register programming is derived from the Allwinner A733 BSP.
 *
 * Copyright (C) 2019 Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>

#include "sun60i-di.h"

static const u32 sun60i_di_formats[] = {
	V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_NV21,
};

static inline u32 sun60i_di_read(struct sun60i_di_dev *dev, u32 reg)
{
	return readl(dev->base + reg);
}

static inline void sun60i_di_write(struct sun60i_di_dev *dev,
				  u32 reg, u32 value)
{
	writel(value, dev->base + reg);
}

static void sun60i_di_write_addr(struct sun60i_di_dev *dev, u32 y_reg,
				u32 c_reg, u32 high_reg,
				dma_addr_t y_addr, dma_addr_t c_addr)
{
	u32 high = upper_32_bits(y_addr) | (upper_32_bits(c_addr) << 8);

	sun60i_di_write(dev, y_reg, lower_32_bits(y_addr));
	sun60i_di_write(dev, c_reg, lower_32_bits(c_addr));
	sun60i_di_write(dev, high_reg, high);
}

static void sun60i_di_hw_reset(struct sun60i_di_dev *dev)
{
	sun60i_di_write(dev, SUN60I_DI_RESET, SUN60I_DI_RESET_ASSERT);
	udelay(1);
	sun60i_di_write(dev, SUN60I_DI_RESET, 0);
}

static void sun60i_di_finish_job(struct sun60i_di_ctx *ctx,
				enum vb2_buffer_state state)
{
	struct vb2_v4l2_buffer *src, *dst;

	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (dst)
		v4l2_m2m_buf_done(dst, state);

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	if (src)
		v4l2_m2m_buf_done(src, state);

	ctx->field = ctx->first_field;
	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

static void sun60i_di_watchdog(struct work_struct *work)
{
	struct sun60i_di_dev *dev =
		container_of(to_delayed_work(work), struct sun60i_di_dev,
			     watchdog);
	struct sun60i_di_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&dev->irqlock, flags);
	if (!dev->job_active) {
		spin_unlock_irqrestore(&dev->irqlock, flags);
		return;
	}
	dev->job_active = false;
	spin_unlock_irqrestore(&dev->irqlock, flags);

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx)
		return;

	v4l2_err(&dev->v4l2_dev, "processing timeout\n");
	sun60i_di_write(dev, SUN60I_DI_INT_CTL, 0);
	sun60i_di_hw_reset(dev);
	sun60i_di_finish_job(ctx, VB2_BUF_STATE_ERROR);
}

static void sun60i_di_device_run(void *priv)
{
	struct sun60i_di_ctx *ctx = priv;
	struct sun60i_di_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src, *dst;
	dma_addr_t src_y, src_c, dst_y, dst_c;
	u32 crop, format, height, stride, width;
	unsigned long flags;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	v4l2_m2m_buf_copy_metadata(src, dst, true);
	dst->field = V4L2_FIELD_NONE;

	width = ctx->src_fmt.width;
	height = ctx->src_fmt.height;
	stride = ctx->src_fmt.bytesperline;

	src_y = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	src_c = src_y + stride * height;
	dst_y = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	dst_c = dst_y + ctx->dst_fmt.bytesperline * height;

	/* DI301 reads the two fields through separate addresses. */
	sun60i_di_write_addr(dev, SUN60I_DI_IN_F1_TOP_Y,
			     SUN60I_DI_IN_F1_TOP_C,
			      SUN60I_DI_IN_F1_TOP_HADDR, src_y, src_c);
	sun60i_di_write_addr(dev, SUN60I_DI_IN_F1_BOT_Y,
			     SUN60I_DI_IN_F1_BOT_C,
			      SUN60I_DI_IN_F1_BOT_HADDR,
			      src_y + stride, src_c + stride);
	sun60i_di_write_addr(dev, SUN60I_DI_OUT_DIT1_Y,
			     SUN60I_DI_OUT_DIT1_C,
			      SUN60I_DI_OUT_DIT1_HADDR, dst_y, dst_c);

	sun60i_di_write(dev, SUN60I_DI_IN_F01_PITCH_Y,
			SUN60I_DI_IN_F01_PITCH_F23(stride * 2));
	sun60i_di_write(dev, SUN60I_DI_IN_F01_PITCH_C,
			SUN60I_DI_IN_F01_PITCH_F23(stride * 2));
	sun60i_di_write(dev, SUN60I_DI_OUT_DIT_PITCH_Y,
			ctx->dst_fmt.bytesperline);
	sun60i_di_write(dev, SUN60I_DI_OUT_DIT_PITCH_C,
			ctx->dst_fmt.bytesperline);

	format = SUN60I_DI_FORMAT_IN1(SUN60I_DI_FORMAT_YUV420_SP) |
		 SUN60I_DI_FORMAT_DIT(SUN60I_DI_FORMAT_YUV420_SP);
	if (ctx->src_fmt.pixelformat == V4L2_PIX_FMT_NV12)
		format |= SUN60I_DI_FORMAT_UV_SEQ;
	sun60i_di_write(dev, SUN60I_DI_FORMAT, format);

	sun60i_di_write(dev, SUN60I_DI_SIZE,
			(width - 1) | ((height - 1) << 16));
	sun60i_di_write(dev, SUN60I_DI_FIELD_ORDER,
			ctx->field ? SUN60I_DI_FIELD_ORDER_BFF : 0);

	crop = (width - 1) << 16;
	sun60i_di_write(dev, SUN60I_DI_DIT_CROP_H, crop);
	sun60i_di_write(dev, SUN60I_DI_DIT_DEMO_H, crop);
	crop = (height - 1) << 16;
	sun60i_di_write(dev, SUN60I_DI_DIT_CROP_V, crop);
	sun60i_di_write(dev, SUN60I_DI_DIT_DEMO_V, crop);

	sun60i_di_write(dev, SUN60I_DI_STATUS, SUN60I_DI_STATUS_FINISH);
	spin_lock_irqsave(&dev->irqlock, flags);
	dev->job_active = true;
	spin_unlock_irqrestore(&dev->irqlock, flags);
	mod_delayed_work(system_wq, &dev->watchdog,
			 msecs_to_jiffies(SUN60I_DI_JOB_TIMEOUT_MS));
	sun60i_di_write(dev, SUN60I_DI_INT_CTL, SUN60I_DI_INT_FINISH_EN);
	sun60i_di_write(dev, SUN60I_DI_START, SUN60I_DI_START_RUN);
}

static int sun60i_di_job_ready(void *priv)
{
	struct sun60i_di_ctx *ctx = priv;

	return v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) >= 1 &&
	       v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) >= 2;
}

static void sun60i_di_job_abort(void *priv)
{
	struct sun60i_di_ctx *ctx = priv;

	/* Run the watchdog immediately to abort and return the buffers. */
	ctx->aborting = 1;
	mod_delayed_work(system_wq, &ctx->dev->watchdog, 0);
}

static irqreturn_t sun60i_di_irq(int irq, void *data)
{
	struct sun60i_di_dev *dev = data;
	struct vb2_v4l2_buffer *dst;
	enum vb2_buffer_state state;
	struct sun60i_di_ctx *ctx;
	unsigned long flags;
	unsigned int val;

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		v4l2_err(&dev->v4l2_dev,
			 "Instance released before the end of transaction\n");
		return IRQ_NONE;
	}

	val = sun60i_di_read(dev, SUN60I_DI_STATUS);
	if (!(val & SUN60I_DI_STATUS_FINISH))
		return IRQ_NONE;

	sun60i_di_write(dev, SUN60I_DI_INT_CTL, 0);
	sun60i_di_write(dev, SUN60I_DI_STATUS, SUN60I_DI_STATUS_FINISH);
	cancel_delayed_work_sync(&dev->watchdog);
	spin_lock_irqsave(&dev->irqlock, flags);
	if (!dev->job_active) {
		spin_unlock_irqrestore(&dev->irqlock, flags);
		return IRQ_HANDLED;
	}
	dev->job_active = false;
	spin_unlock_irqrestore(&dev->irqlock, flags);
	state = ctx->aborting ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;

	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	if (dst)
		v4l2_m2m_buf_done(dst, state);

	if (ctx->field != ctx->first_field || ctx->aborting) {
		struct vb2_v4l2_buffer *src;

		ctx->field = ctx->first_field;
		src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		if (src)
			v4l2_m2m_buf_done(src, state);
		v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
	} else {
		ctx->field = !ctx->first_field;
		sun60i_di_device_run(ctx);
	}

	return IRQ_HANDLED;
}

static void sun60i_di_init(struct sun60i_di_dev *dev)
{
	u32 setting = SUN60I_DI_DIT_MODE_LUMA |
		      SUN60I_DI_DIT_DIAG_INTP_EN |
		      SUN60I_DI_DIT_ELA_DEMO_EN |
		      SUN60I_DI_DIT_WEAVE_DEMO_EN |
		      SUN60I_DI_DIT_BLEND_DEMO_EN |
		      SUN60I_DI_DIT_MODE_CHROMA |
		      SUN60I_DI_DIT_ONE_FRAME;

	sun60i_di_hw_reset(dev);
	sun60i_di_write(dev, SUN60I_DI_START, 0);
	sun60i_di_write(dev, SUN60I_DI_INT_CTL, 0);
	sun60i_di_write(dev, SUN60I_DI_STATUS, SUN60I_DI_STATUS_FINISH);
	sun60i_di_write(dev, SUN60I_DI_FUNC_EN, SUN60I_DI_FUNC_DIT_EN);
	sun60i_di_write(dev, SUN60I_DI_DMA_CTL,
			SUN60I_DI_DMA_IN_F1_EN |
			  SUN60I_DI_DMA_OUT_DIT1_EN |
			  SUN60I_DI_DMA_MCLK_GATE);

	sun60i_di_write(dev, SUN60I_DI_DIT_SETTING, setting);
	sun60i_di_write(dev, SUN60I_DI_DIT_CHROMA_PARA0, 0x30058000);
	sun60i_di_write(dev, SUN60I_DI_DIT_CHROMA_PARA1, 0x04300000);
	sun60i_di_write(dev, SUN60I_DI_DIT_INTRA_PARA, 0x514240ac);
	sun60i_di_write(dev, SUN60I_DI_DIT_INTER_PARA, 0x22000000);
}

static inline struct sun60i_di_ctx *sun60i_di_file2ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct sun60i_di_ctx, fh);
}

static bool sun60i_di_check_format(u32 pixelformat)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sun60i_di_formats); i++)
		if (sun60i_di_formats[i] == pixelformat)
			return true;

	return false;
}

static void sun60i_di_prepare_format(struct v4l2_pix_format *pix_fmt)
{
	unsigned int height = pix_fmt->height;
	unsigned int width = pix_fmt->width;
	unsigned int bytesperline;
	unsigned int sizeimage;

	width = round_down(clamp(width, SUN60I_DI_MIN_WIDTH,
				 SUN60I_DI_MAX_WIDTH), 4);
	height = round_down(clamp(height, SUN60I_DI_MIN_HEIGHT,
				  SUN60I_DI_MAX_HEIGHT), 4);

	bytesperline = min(ALIGN(max(pix_fmt->bytesperline, width), 4),
			   SUN60I_DI_MAX_STRIDE);
	/* luma */
	sizeimage = bytesperline * height;
	/* chroma */
	sizeimage += bytesperline * height / 2;

	pix_fmt->width = width;
	pix_fmt->height = height;
	pix_fmt->bytesperline = bytesperline;
	pix_fmt->sizeimage = sizeimage;
}

static int sun60i_di_querycap(struct file *file, void *priv,
			      struct v4l2_capability *cap)
{
	strscpy(cap->driver, SUN60I_DI_NAME, sizeof(cap->driver));
	strscpy(cap->card, SUN60I_DI_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", SUN60I_DI_NAME);

	return 0;
}

static int sun60i_di_enum_fmt(struct file *file, void *priv,
			      struct v4l2_fmtdesc *f)
{
	if (f->index < ARRAY_SIZE(sun60i_di_formats)) {
		f->pixelformat = sun60i_di_formats[f->index];

		return 0;
	}

	return -EINVAL;
}

static int sun60i_di_enum_framesizes(struct file *file, void *priv,
				     struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index != 0)
		return -EINVAL;

	if (!sun60i_di_check_format(fsize->pixel_format))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = SUN60I_DI_MIN_WIDTH;
	fsize->stepwise.min_height = SUN60I_DI_MIN_HEIGHT;
	fsize->stepwise.max_width = SUN60I_DI_MAX_WIDTH;
	fsize->stepwise.max_height = SUN60I_DI_MAX_HEIGHT;
	fsize->stepwise.step_width = 4;
	fsize->stepwise.step_height = 4;

	return 0;
}

static int sun60i_di_g_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct sun60i_di_ctx *ctx = sun60i_di_file2ctx(file);

	f->fmt.pix = ctx->dst_fmt;

	return 0;
}

static int sun60i_di_g_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct sun60i_di_ctx *ctx = sun60i_di_file2ctx(file);

	f->fmt.pix = ctx->src_fmt;

	return 0;
}

static int sun60i_di_try_fmt_vid_cap(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	if (!sun60i_di_check_format(f->fmt.pix.pixelformat))
		f->fmt.pix.pixelformat = sun60i_di_formats[0];

	if (f->fmt.pix.field != V4L2_FIELD_NONE)
		f->fmt.pix.field = V4L2_FIELD_NONE;

	sun60i_di_prepare_format(&f->fmt.pix);

	return 0;
}

static int sun60i_di_try_fmt_vid_out(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	if (!sun60i_di_check_format(f->fmt.pix.pixelformat))
		f->fmt.pix.pixelformat = sun60i_di_formats[0];

	if (f->fmt.pix.field != V4L2_FIELD_INTERLACED_TB &&
	    f->fmt.pix.field != V4L2_FIELD_INTERLACED_BT &&
	    f->fmt.pix.field != V4L2_FIELD_INTERLACED)
		f->fmt.pix.field = V4L2_FIELD_INTERLACED;

	sun60i_di_prepare_format(&f->fmt.pix);

	return 0;
}

static int sun60i_di_s_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct sun60i_di_ctx *ctx = sun60i_di_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	ret = sun60i_di_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	/* DI301 does not scale; capture always matches the output queue. */
	f->fmt.pix.width = ctx->src_fmt.width;
	f->fmt.pix.height = ctx->src_fmt.height;
	f->fmt.pix.pixelformat = ctx->src_fmt.pixelformat;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	sun60i_di_prepare_format(&f->fmt.pix);
	ctx->dst_fmt = f->fmt.pix;

	return 0;
}

static int sun60i_di_s_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct sun60i_di_ctx *ctx = sun60i_di_file2ctx(file);
	struct vb2_queue *dst_vq, *src_vq;
	int ret;

	ret = sun60i_di_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	src_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	dst_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
				 V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (vb2_is_busy(src_vq))
		return -EBUSY;
	if (vb2_is_busy(dst_vq) &&
	    (ctx->dst_fmt.width != f->fmt.pix.width ||
	     ctx->dst_fmt.height != f->fmt.pix.height ||
	     ctx->dst_fmt.pixelformat != f->fmt.pix.pixelformat))
		return -EBUSY;

	ctx->src_fmt = f->fmt.pix;
	if (!vb2_is_busy(dst_vq)) {
		ctx->dst_fmt.width = f->fmt.pix.width;
		ctx->dst_fmt.height = f->fmt.pix.height;
		ctx->dst_fmt.pixelformat = f->fmt.pix.pixelformat;
		ctx->dst_fmt.field = V4L2_FIELD_NONE;
		ctx->dst_fmt.bytesperline = f->fmt.pix.bytesperline;
		sun60i_di_prepare_format(&ctx->dst_fmt);
	}

	/* Propagate colorspace information to capture. */
	ctx->dst_fmt.colorspace = f->fmt.pix.colorspace;
	ctx->dst_fmt.xfer_func = f->fmt.pix.xfer_func;
	ctx->dst_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
	ctx->dst_fmt.quantization = f->fmt.pix.quantization;

	return 0;
}

static const struct v4l2_ioctl_ops sun60i_di_ioctl_ops = {
	.vidioc_querycap		= sun60i_di_querycap,

	.vidioc_enum_framesizes		= sun60i_di_enum_framesizes,

	.vidioc_enum_fmt_vid_cap	= sun60i_di_enum_fmt,
	.vidioc_g_fmt_vid_cap		= sun60i_di_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= sun60i_di_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= sun60i_di_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out	= sun60i_di_enum_fmt,
	.vidioc_g_fmt_vid_out		= sun60i_di_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out		= sun60i_di_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out		= sun60i_di_s_fmt_vid_out,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,
};

static int sun60i_di_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				 unsigned int *nplanes, unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct sun60i_di_ctx *ctx = vb2_get_drv_priv(vq);
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

static int sun60i_di_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct sun60i_di_ctx *ctx = vb2_get_drv_priv(vq);
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

static void sun60i_di_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sun60i_di_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void sun60i_di_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct sun60i_di_ctx *ctx = vb2_get_drv_priv(vq);
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

static int sun60i_di_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct sun60i_di_ctx *ctx = vb2_get_drv_priv(vq);
	struct device *dev = ctx->dev->dev;
	int ret;

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err(dev, "Failed to enable module\n");

			goto err_runtime_get;
		}

		ctx->first_field =
			ctx->src_fmt.field == V4L2_FIELD_INTERLACED_BT;
		ctx->field = ctx->first_field;

		ctx->aborting = 0;
	}

	return 0;

err_runtime_get:
	sun60i_di_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);

	return ret;
}

static void sun60i_di_stop_streaming(struct vb2_queue *vq)
{
	struct sun60i_di_ctx *ctx = vb2_get_drv_priv(vq);

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		struct device *dev = ctx->dev->dev;

		pm_runtime_put(dev);
	}

	sun60i_di_queue_cleanup(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops sun60i_di_qops = {
	.queue_setup		= sun60i_di_queue_setup,
	.buf_prepare		= sun60i_di_buf_prepare,
	.buf_queue		= sun60i_di_buf_queue,
	.start_streaming	= sun60i_di_start_streaming,
	.stop_streaming		= sun60i_di_stop_streaming,
};

static int sun60i_di_queue_init(void *priv, struct vb2_queue *src_vq,
				struct vb2_queue *dst_vq)
{
	struct sun60i_di_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->min_queued_buffers = 1;
	src_vq->ops = &sun60i_di_qops;
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
	dst_vq->ops = &sun60i_di_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(dst_vq);
	if (ret)
		return ret;

	return 0;
}

static int sun60i_di_open(struct file *file)
{
	struct sun60i_di_dev *dev = video_drvdata(file);
	struct sun60i_di_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&dev->dev_mutex);
		return -ENOMEM;
	}

	/* default output format */
	ctx->src_fmt.pixelformat = sun60i_di_formats[0];
	ctx->src_fmt.field = V4L2_FIELD_INTERLACED;
	ctx->src_fmt.width = 640;
	ctx->src_fmt.height = 480;
	sun60i_di_prepare_format(&ctx->src_fmt);

	/* default capture format */
	ctx->dst_fmt.pixelformat = sun60i_di_formats[0];
	ctx->dst_fmt.field = V4L2_FIELD_NONE;
	ctx->dst_fmt.width = 640;
	ctx->dst_fmt.height = 480;
	sun60i_di_prepare_format(&ctx->dst_fmt);

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	ctx->dev = dev;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &sun60i_di_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free;
	}

	v4l2_fh_add(&ctx->fh, file);

	mutex_unlock(&dev->dev_mutex);

	return 0;

err_free:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static int sun60i_di_release(struct file *file)
{
	struct sun60i_di_dev *dev = video_drvdata(file);
	struct sun60i_di_ctx *ctx = sun60i_di_file2ctx(file);

	mutex_lock(&dev->dev_mutex);

	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations sun60i_di_fops = {
	.owner		= THIS_MODULE,
	.open		= sun60i_di_open,
	.release	= sun60i_di_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device sun60i_di_video_device = {
	.name		= SUN60I_DI_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &sun60i_di_fops,
	.ioctl_ops	= &sun60i_di_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops sun60i_di_m2m_ops = {
	.device_run	= sun60i_di_device_run,
	.job_ready	= sun60i_di_job_ready,
	.job_abort	= sun60i_di_job_abort,
};

static int sun60i_di_probe(struct platform_device *pdev)
{
	struct sun60i_di_dev *dev;
	struct video_device *vfd;
	int irq, ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->vfd = sun60i_di_video_device;
	dev->dev = &pdev->dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	dev->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dev->base))
		return PTR_ERR(dev->base);

	dev->clocks[0].id = "mod";
	dev->clocks[1].id = "bus";
	ret = devm_clk_bulk_get(dev->dev, ARRAY_SIZE(dev->clocks), dev->clocks);
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to get clocks\n");

	dev->resets[0].id = "bus";
	dev->resets[1].id = "sys";
	ret = devm_reset_control_bulk_get_shared(dev->dev,
					       ARRAY_SIZE(dev->resets),
					       dev->resets);
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to get resets\n");

	ret = dma_set_mask_and_coherent(dev->dev, DMA_BIT_MASK(40));
	if (ret)
		return dev_err_probe(dev->dev, ret, "failed to set DMA mask\n");

	mutex_init(&dev->dev_mutex);
	spin_lock_init(&dev->irqlock);
	INIT_DELAYED_WORK(&dev->watchdog, sun60i_di_watchdog);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(dev->dev, "Failed to register V4L2 device\n");

		return ret;
	}

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;

	snprintf(vfd->name, sizeof(vfd->name), "%s",
		 sun60i_di_video_device.name);
	video_set_drvdata(vfd, dev);

	dev->m2m_dev = v4l2_m2m_init(&sun60i_di_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(dev->m2m_dev);

		goto err_v4l2;
	}

	ret = devm_request_threaded_irq(dev->dev, irq, NULL, sun60i_di_irq,
					IRQF_ONESHOT, dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request IRQ\n");
		goto err_m2m;
	}

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto err_m2m;
	}

	v4l2_info(&dev->v4l2_dev,
		  "DI301 registered as /dev/video%d\n", vfd->num);

	platform_set_drvdata(pdev, dev);

	pm_runtime_enable(dev->dev);

	return 0;

err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);

	return ret;
}

static void sun60i_di_remove(struct platform_device *pdev)
{
	struct sun60i_di_dev *dev = platform_get_drvdata(pdev);

	video_unregister_device(&dev->vfd);
	cancel_delayed_work_sync(&dev->watchdog);
	v4l2_m2m_release(dev->m2m_dev);
	v4l2_device_unregister(&dev->v4l2_dev);

	pm_runtime_force_suspend(&pdev->dev);
}

static int sun60i_di_runtime_resume(struct device *device)
{
	struct sun60i_di_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = clk_set_rate_exclusive(dev->clocks[0].clk, 300000000);
	if (ret) {
		dev_err(dev->dev, "Failed to set exclusive mod clock rate\n");

		return ret;
	}

	ret = reset_control_bulk_deassert(ARRAY_SIZE(dev->resets), dev->resets);
	if (ret) {
		dev_err(dev->dev, "Failed to apply reset\n");

		goto err_exclusive_rate;
	}

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(dev->clocks), dev->clocks);
	if (ret) {
		dev_err(dev->dev, "Failed to enable clocks\n");
		goto err_rst;
	}

	sun60i_di_init(dev);

	return 0;

err_rst:
	reset_control_bulk_assert(ARRAY_SIZE(dev->resets), dev->resets);
err_exclusive_rate:
	clk_rate_exclusive_put(dev->clocks[0].clk);

	return ret;
}

static int sun60i_di_runtime_suspend(struct device *device)
{
	struct sun60i_di_dev *dev = dev_get_drvdata(device);

	clk_bulk_disable_unprepare(ARRAY_SIZE(dev->clocks), dev->clocks);
	reset_control_bulk_assert(ARRAY_SIZE(dev->resets), dev->resets);
	clk_rate_exclusive_put(dev->clocks[0].clk);

	return 0;
}

static const struct of_device_id sun60i_di_dt_match[] = {
	{ .compatible = "allwinner,sun60i-a733-deinterlace" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sun60i_di_dt_match);

static const struct dev_pm_ops sun60i_di_pm_ops = {
	.runtime_resume		= sun60i_di_runtime_resume,
	.runtime_suspend	= sun60i_di_runtime_suspend,
};

static struct platform_driver sun60i_di_driver = {
	.probe		= sun60i_di_probe,
	.remove		= sun60i_di_remove,
	.driver		= {
		.name		= SUN60I_DI_NAME,
		.of_match_table	= sun60i_di_dt_match,
		.pm		= &sun60i_di_pm_ops,
	},
};
module_platform_driver(sun60i_di_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Allwinner A733 DI301 deinterlacer driver");
