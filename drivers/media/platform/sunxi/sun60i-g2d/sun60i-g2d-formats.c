// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */

#include "sun60i-g2d-formats.h"
#include "sun60i-g2d.h"

/*
 * Formats not included in array:
 * SUN60I_G2D_FORMAT_BGR565
 * SUN60I_G2D_FORMAT_VYUV
 */

static const struct sun60i_g2d_format sun60i_g2d_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_ARGB32,
		.hw_format = SUN60I_G2D_FORMAT_ARGB32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_ABGR32,
		.hw_format = SUN60I_G2D_FORMAT_ABGR32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_RGBA32,
		.hw_format = SUN60I_G2D_FORMAT_RGBA32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_BGRA32,
		.hw_format = SUN60I_G2D_FORMAT_BGRA32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_XRGB32,
		.hw_format = SUN60I_G2D_FORMAT_XRGB32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_XBGR32,
		.hw_format = SUN60I_G2D_FORMAT_XBGR32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_RGB32,
		.hw_format = SUN60I_G2D_FORMAT_RGBX32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_BGR32,
		.hw_format = SUN60I_G2D_FORMAT_BGRX32,
		.planes = 1,
		.bpp = { 4, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_RGB24,
		.hw_format = SUN60I_G2D_FORMAT_RGB24,
		.planes = 1,
		.bpp = { 3, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_BGR24,
		.hw_format = SUN60I_G2D_FORMAT_BGR24,
		.planes = 1,
		.bpp = { 3, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_RGB565,
		.hw_format = SUN60I_G2D_FORMAT_RGB565,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_ARGB444,
		.hw_format = SUN60I_G2D_FORMAT_ARGB4444,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_ABGR444,
		.hw_format = SUN60I_G2D_FORMAT_ABGR4444,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_RGBA444,
		.hw_format = SUN60I_G2D_FORMAT_RGBA4444,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_BGRA444,
		.hw_format = SUN60I_G2D_FORMAT_BGRA4444,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_ARGB555,
		.hw_format = SUN60I_G2D_FORMAT_ARGB1555,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_ABGR555,
		.hw_format = SUN60I_G2D_FORMAT_ABGR1555,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_RGBA555,
		.hw_format = SUN60I_G2D_FORMAT_RGBA5551,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_BGRA555,
		.hw_format = SUN60I_G2D_FORMAT_BGRA5551,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 1,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_YVYU,
		.hw_format = SUN60I_G2D_FORMAT_YVYU,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 2,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_YUV
	}, {
		.fourcc = V4L2_PIX_FMT_UYVY,
		.hw_format = SUN60I_G2D_FORMAT_UYVY,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 2,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_YUV
	}, {
		.fourcc = V4L2_PIX_FMT_YUYV,
		.hw_format = SUN60I_G2D_FORMAT_YUYV,
		.planes = 1,
		.bpp = { 2, 0, 0 },
		.hsub = 2,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_YUV
	}, {
		.fourcc = V4L2_PIX_FMT_NV61,
		.hw_format = SUN60I_G2D_FORMAT_NV61,
		.planes = 2,
		.bpp = { 1, 2, 0 },
		.hsub = 2,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_YUV
	}, {
		.fourcc = V4L2_PIX_FMT_NV16,
		.hw_format = SUN60I_G2D_FORMAT_NV16,
		.planes = 2,
		.bpp = { 1, 2, 0 },
		.hsub = 2,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_YUV
	}, {
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.hw_format = SUN60I_G2D_FORMAT_YUV422P,
		.planes = 3,
		.bpp = { 1, 1, 1 },
		.hsub = 2,
		.vsub = 1,
		.flags = SUN60I_G2D_FLAG_YUV
	}, {
		.fourcc = V4L2_PIX_FMT_NV21,
		.hw_format = SUN60I_G2D_FORMAT_NV21,
		.planes = 2,
		.bpp = { 1, 2, 0 },
		.hsub = 2,
		.vsub = 2,
		.flags = SUN60I_G2D_FLAG_YUV
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.hw_format = SUN60I_G2D_FORMAT_NV12,
		.planes = 2,
		.bpp = { 1, 2, 0 },
		.hsub = 2,
		.vsub = 2,
		.flags = SUN60I_G2D_FLAG_YUV | SUN60I_G2D_FLAG_OUTPUT
	}, {
		.fourcc = V4L2_PIX_FMT_YUV420,
		.hw_format = SUN60I_G2D_FORMAT_YUV420P,
		.planes = 3,
		.bpp = { 1, 1, 1 },
		.hsub = 2,
		.vsub = 2,
		.flags = SUN60I_G2D_FLAG_YUV | SUN60I_G2D_FLAG_OUTPUT
	},
};

const struct sun60i_g2d_format *sun60i_g2d_find_format(u32 pixelformat)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sun60i_g2d_formats); i++)
		if (sun60i_g2d_formats[i].fourcc == pixelformat)
			return &sun60i_g2d_formats[i];

	return NULL;
}

int sun60i_g2d_enum_fmt(struct v4l2_fmtdesc *f, bool dst)
{
	int i, index;

	index = 0;

	for (i = 0; i < ARRAY_SIZE(sun60i_g2d_formats); i++) {
		/* not all formats can be used for capture buffers */
		if (dst && !(sun60i_g2d_formats[i].flags & SUN60I_G2D_FLAG_OUTPUT))
			continue;

		if (index == f->index) {
			f->pixelformat = sun60i_g2d_formats[i].fourcc;

			return 0;
		}

		index++;
	}

	return -EINVAL;
}
