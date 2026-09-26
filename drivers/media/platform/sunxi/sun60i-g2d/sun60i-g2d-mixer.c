// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */

#include <linux/math64.h>

#include "sun60i-g2d-formats.h"
#include "sun60i-g2d.h"

#define SUN60I_G2D_OVL_SIZE	0x40
#define SUN60I_G2D_VSU_SIZE	0x480
#define SUN60I_G2D_BLD_SIZE	0x1a0
#define SUN60I_G2D_WB_SIZE	0x2c

static const u32 sun60i_g2d_linear_coeffs[32] = {
	0x00004000, 0x00023e00, 0x00043c00, 0x00063a00,
	0x00083800, 0x000a3600, 0x000c3400, 0x000e3200,
	0x00103000, 0x00122e00, 0x00142c00, 0x00162a00,
	0x00182800, 0x001a2600, 0x001c2400, 0x001e2200,
	0x00202000, 0x00221e00, 0x00241c00, 0x00261a00,
	0x00281800, 0x002a1600, 0x002c1400, 0x002e1200,
	0x00301000, 0x00320e00, 0x00340c00, 0x00360a00,
	0x00380800, 0x003a0600, 0x003c0400, 0x003e0200,
};

/* BT.601 matrices from the A733 BSP, ordered by source/destination range. */
static const u32 sun60i_g2d_rgb2yuv601[4][12] = {
	/* full to limited */
	{ 0x107, 0x204, 0x064, 0x4000,
	  0xffffff68, 0xfffffed6, 0x01c2, 0x20000,
	  0x01c2, 0xfffffe87, 0xffffffb7, 0x20000 },
	/* limited to limited */
	{ 0x132, 0x259, 0x075, 0,
	  0xffffff4f, 0xfffffea5, 0x020c, 0x20000,
	  0x020c, 0xfffffe49, 0xffffffab, 0x20000 },
	/* full to full */
	{ 0x132, 0x259, 0x075, 0,
	  0xffffff53, 0xfffffead, 0x0200, 0x20000,
	  0x0200, 0xfffffe53, 0xffffffad, 0x20000 },
	/* limited to full */
	{ 0x0165, 0x02bc, 0x088, 0xfffff570,
	  0xffffff37, 0xfffffe75, 0x0254, 0x20000,
	  0x0254, 0xfffffe0d, 0xffffff9f, 0x20000 },
};

static const u32 sun60i_g2d_yuv2rgb601[4][12] = {
	/* full to limited */
	{ 0x04a8, 0, 0x0662, 0xfffc8480,
	  0x04a8, 0xfffffe6f, 0xfffffcc0, 0x21e00,
	  0x04a8, 0x0812, 0, 0xfffbac80 },
	/* limited to limited */
	{ 0x0400, 0, 0x057c, 0xfffd0200,
	  0x0400, 0xfffffea7, 0xfffffd35, 0x1d200,
	  0x0400, 0x06ee, 0, 0xfffc4900 },
	/* full to full */
	{ 0x0400, 0, 0x059c, 0xfffd31ff,
	  0x0400, 0xfffffe9f, 0xfffffd24, 0x21d80,
	  0x0400, 0x0717, 0, 0xfffc747f },
	/* limited to full */
	{ 0x036f, 0, 0x04d1, 0xfffda090,
	  0x036f, 0xfffffed1, 0xfffffd8c, 0x1da90,
	  0x036f, 0x0616, 0, 0xfffcfe10 },
};

static void sun60i_g2d_block_write(void *block, u32 offset, u32 value)
{
	*((u32 *)((u8 *)block + offset)) = value;
}

static void sun60i_g2d_planes(dma_addr_t base, u32 bytesperline, u32 height,
			      const struct sun60i_g2d_format *fmt,
			      dma_addr_t addr[3], u32 pitch[3])
{
	unsigned int i;

	for (i = 0; i < fmt->planes; i++) {
		pitch[i] = bytesperline;
		addr[i] = base;
		if (i)
			pitch[i] /= fmt->hsub / fmt->bpp[i];
		base += pitch[i] * height / (i ? fmt->vsub : 1);
	}
}

static void sun60i_g2d_config_overlay(void *block,
				      const struct v4l2_pix_format *pix,
				      const struct sun60i_g2d_format *fmt,
				      dma_addr_t base)
{
	dma_addr_t addr[3] = {};
	u32 pitch[3] = {};
	u32 high;

	sun60i_g2d_planes(base, pix->bytesperline, pix->height, fmt, addr, pitch);
	high = (upper_32_bits(addr[0]) & 0xff) |
	       ((upper_32_bits(addr[1]) & 0xff) << 8) |
	       ((upper_32_bits(addr[2]) & 0xff) << 16);

	sun60i_g2d_block_write(block, 0x00,
				BIT(0) | (fmt->hw_format << 8) | (0xff << 24));
	sun60i_g2d_block_write(block, 0x04,
				SUN60I_G2D_SIZE(pix->width, pix->height));
	sun60i_g2d_block_write(block, 0x0c, pitch[0]);
	sun60i_g2d_block_write(block, 0x10, pitch[1]);
	sun60i_g2d_block_write(block, 0x14, pitch[2]);
	sun60i_g2d_block_write(block, 0x18, lower_32_bits(addr[0]));
	sun60i_g2d_block_write(block, 0x1c, lower_32_bits(addr[1]));
	sun60i_g2d_block_write(block, 0x20, lower_32_bits(addr[2]));
	sun60i_g2d_block_write(block, 0x28, high);
	sun60i_g2d_block_write(block, 0x2c,
				SUN60I_G2D_SIZE(pix->width, pix->height));
}

static void sun60i_g2d_config_scaler(void *block,
				     const struct v4l2_pix_format *src,
				     const struct v4l2_pix_format *dst,
				     const struct sun60i_g2d_format *fmt)
{
	u64 step;
	u32 hstep, vstep, cwidth, cheight;
	unsigned int i;

	step = (u64)src->width << 19;
	hstep = div_u64(step, dst->width);
	step = (u64)src->height << 19;
	vstep = div_u64(step, dst->height);

	sun60i_g2d_block_write(block, 0x00,
				BIT(0) | BIT(8) |
				((fmt->flags & SUN60I_G2D_FLAG_YUV) ? BIT(16) : 0));
	sun60i_g2d_block_write(block, 0x40,
				SUN60I_G2D_SIZE(dst->width, dst->height));
	sun60i_g2d_block_write(block, 0x44, 0xff);
	sun60i_g2d_block_write(block, 0x80,
				SUN60I_G2D_SIZE(src->width, src->height));
	sun60i_g2d_block_write(block, 0x88, hstep << 1);
	sun60i_g2d_block_write(block, 0x8c, vstep << 1);

	cwidth = DIV_ROUND_UP(src->width, fmt->hsub);
	cheight = DIV_ROUND_UP(src->height, fmt->vsub);
	sun60i_g2d_block_write(block, 0xc0,
				SUN60I_G2D_SIZE(cwidth, cheight));
	sun60i_g2d_block_write(block, 0xc8,
				(fmt->hsub == 1) ? hstep << 1 : hstep);
	sun60i_g2d_block_write(block, 0xcc,
				(fmt->vsub == 1) ? vstep << 1 : vstep);
	if (fmt->hsub == 2 || fmt->vsub == 2) {
		sun60i_g2d_block_write(block, 0xd0, 0xfffc0000);
		sun60i_g2d_block_write(block, 0xd8, 0xfffc0000);
	}

	for (i = 0; i < ARRAY_SIZE(sun60i_g2d_linear_coeffs); i++) {
		sun60i_g2d_block_write(block, 0x200 + i * 4,
					sun60i_g2d_linear_coeffs[i]);
		sun60i_g2d_block_write(block, 0x300 + i * 4,
					sun60i_g2d_linear_coeffs[i]);
		sun60i_g2d_block_write(block, 0x400 + i * 4,
					sun60i_g2d_linear_coeffs[i]);
	}
}

static bool sun60i_g2d_full_range(const struct v4l2_pix_format *pix, bool rgb)
{
	enum v4l2_quantization quantization = pix->quantization;

	if (quantization == V4L2_QUANTIZATION_DEFAULT)
		quantization = V4L2_MAP_QUANTIZATION_DEFAULT(rgb,
							 pix->colorspace,
							 pix->ycbcr_enc);

	return quantization == V4L2_QUANTIZATION_FULL_RANGE;
}

static unsigned int sun60i_g2d_csc_index(bool src_full, bool dst_full)
{
	if (src_full)
		return dst_full ? 2 : 0;

	return dst_full ? 3 : 1;
}

static void sun60i_g2d_config_blender(void *block,
				      const struct v4l2_pix_format *src,
				      const struct v4l2_pix_format *dst,
				      const struct sun60i_g2d_format *src_fmt,
				      const struct sun60i_g2d_format *dst_fmt)
{
	const u32 *matrix;
	bool src_yuv = src_fmt->flags & SUN60I_G2D_FLAG_YUV;
	bool dst_yuv = dst_fmt->flags & SUN60I_G2D_FLAG_YUV;
	u32 size = SUN60I_G2D_SIZE(dst->width, dst->height);
	unsigned int i, index;

	sun60i_g2d_block_write(block, 0x00, BIT(8) | BIT(0));
	sun60i_g2d_block_write(block, 0x20, size);
	sun60i_g2d_block_write(block, 0x48, size);
	sun60i_g2d_block_write(block, 0x4c, 0x03010301);
	sun60i_g2d_block_write(block, 0x60,
				src_yuv ? BIT(1) : 0);
	sun60i_g2d_block_write(block, 0x80, 0xf0);
	sun60i_g2d_block_write(block, 0x84, 0x41000);

	if (src_yuv == dst_yuv)
		return;

	if (src_yuv)
		index = sun60i_g2d_csc_index(
			sun60i_g2d_full_range(dst, true),
			sun60i_g2d_full_range(src, false));
	else
		index = sun60i_g2d_csc_index(
			sun60i_g2d_full_range(src, true),
			sun60i_g2d_full_range(dst, false));
	matrix = src_yuv ? sun60i_g2d_yuv2rgb601[index] :
			   sun60i_g2d_rgb2yuv601[index];
	sun60i_g2d_block_write(block, 0x10, 0x00108080);
	sun60i_g2d_block_write(block, 0x14, 0x00108080);
	sun60i_g2d_block_write(block, 0x100, BIT(2));
	for (i = 0; i < 12; i++)
		sun60i_g2d_block_write(block, 0x170 + i * 4, matrix[i]);
}

static void sun60i_g2d_config_writeback(void *block,
					const struct v4l2_pix_format *pix,
					const struct sun60i_g2d_format *fmt,
					dma_addr_t base)
{
	dma_addr_t addr[3] = {};
	u32 pitch[3] = {};

	sun60i_g2d_planes(base, pix->bytesperline, pix->height, fmt, addr, pitch);
	sun60i_g2d_block_write(block, 0x00, fmt->hw_format);
	sun60i_g2d_block_write(block, 0x04,
				SUN60I_G2D_SIZE(pix->width, pix->height));
	sun60i_g2d_block_write(block, 0x08, pitch[0]);
	sun60i_g2d_block_write(block, 0x0c, pitch[1]);
	sun60i_g2d_block_write(block, 0x10, pitch[2]);
	sun60i_g2d_block_write(block, 0x14, lower_32_bits(addr[0]));
	sun60i_g2d_block_write(block, 0x18, upper_32_bits(addr[0]) & 0xff);
	sun60i_g2d_block_write(block, 0x1c, lower_32_bits(addr[1]));
	sun60i_g2d_block_write(block, 0x20, upper_32_bits(addr[1]) & 0xff);
	sun60i_g2d_block_write(block, 0x24, lower_32_bits(addr[2]));
	sun60i_g2d_block_write(block, 0x28, upper_32_bits(addr[2]) & 0xff);
}

int sun60i_g2d_mixer_run(struct sun60i_g2d_ctx *ctx,
			struct vb2_v4l2_buffer *src,
			struct vb2_v4l2_buffer *dst)
{
	struct sun60i_g2d_dev *dev = ctx->dev;
	const struct sun60i_g2d_format *src_fmt, *dst_fmt;
	void *overlay, *scaler, *blender, *writeback;

	src_fmt = sun60i_g2d_find_format(ctx->src_fmt.pixelformat);
	dst_fmt = sun60i_g2d_find_format(ctx->dst_fmt.pixelformat);
	if (!src_fmt || !dst_fmt)
		return -EINVAL;
	sun60i_g2d_rcq_reset(dev);
	overlay = sun60i_g2d_rcq_add(dev, SUN60I_G2D_V0_OFFSET,
				      SUN60I_G2D_OVL_SIZE);
	scaler = sun60i_g2d_rcq_add(dev, SUN60I_G2D_VSU_OFFSET,
				     SUN60I_G2D_VSU_SIZE);
	blender = sun60i_g2d_rcq_add(dev, SUN60I_G2D_BLD_OFFSET,
				      SUN60I_G2D_BLD_SIZE);
	writeback = sun60i_g2d_rcq_add(dev, SUN60I_G2D_WB_OFFSET,
					SUN60I_G2D_WB_SIZE);
	if (!overlay || !scaler || !blender || !writeback)
		return -ENOMEM;

	sun60i_g2d_config_overlay(overlay, &ctx->src_fmt, src_fmt,
				   vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0));
	sun60i_g2d_config_scaler(scaler, &ctx->src_fmt, &ctx->dst_fmt,
				  src_fmt);
	sun60i_g2d_config_blender(blender, &ctx->src_fmt, &ctx->dst_fmt,
				   src_fmt, dst_fmt);
	sun60i_g2d_config_writeback(writeback, &ctx->dst_fmt, dst_fmt,
				     vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0));
	sun60i_g2d_rcq_submit(dev);

	return 0;
}
