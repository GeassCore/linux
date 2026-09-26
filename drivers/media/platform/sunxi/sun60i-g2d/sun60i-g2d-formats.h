/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#ifndef _SUN60I_G2D_FORMATS_H_
#define _SUN60I_G2D_FORMATS_H_

#include <linux/videodev2.h>

#define SUN60I_G2D_FLAG_YUV	BIT(0)
#define SUN60I_G2D_FLAG_OUTPUT	BIT(1)

struct sun60i_g2d_format {
	u32 fourcc;
	u32 hw_format;
	unsigned int planes;
	unsigned int bpp[3];
	unsigned int hsub;
	unsigned int vsub;
	unsigned int flags;
};

const struct sun60i_g2d_format *sun60i_g2d_find_format(u32 pixelformat);
int sun60i_g2d_enum_fmt(struct v4l2_fmtdesc *f, bool dst);

#endif
