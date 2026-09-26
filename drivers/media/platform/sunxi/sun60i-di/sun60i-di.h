/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Allwinner A733 DI301 deinterlacer
 *
 * Register definitions are derived from the A733 BSP DI301 driver.
 */

#ifndef _SUN60I_DEINTERLACE_H_
#define _SUN60I_DEINTERLACE_H_

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#define SUN60I_DI_NAME			"sun60i-di"

/* DI301 top registers. */
#define SUN60I_DI_RESET			0x0000
#define SUN60I_DI_RESET_ASSERT		BIT(31)
#define SUN60I_DI_FUNC_VERSION		0x000c
#define SUN60I_DI_START			0x0010
#define SUN60I_DI_START_RUN		BIT(0)
#define SUN60I_DI_INT_CTL		0x0014
#define SUN60I_DI_INT_FINISH_EN		BIT(0)
#define SUN60I_DI_STATUS			0x0018
#define SUN60I_DI_STATUS_FINISH		BIT(0)
#define SUN60I_DI_STATUS_BUSY		BIT(8)
#define SUN60I_DI_IP_VERSION		0x001c
#define SUN60I_DI_FUNC_EN		0x0020
#define SUN60I_DI_FUNC_DIT_EN		BIT(0)
#define SUN60I_DI_DMA_CTL		0x0024
#define SUN60I_DI_DMA_IN_F1_EN		BIT(4)
#define SUN60I_DI_DMA_OUT_DIT1_EN	BIT(20)
#define SUN60I_DI_DMA_MCLK_GATE		BIT(31)
#define SUN60I_DI_SIZE			0x0030
#define SUN60I_DI_FORMAT			0x0034
#define SUN60I_DI_FORMAT_IN1(v)		(((v) & 0x3) << 4)
#define SUN60I_DI_FORMAT_DIT(v)		(((v) & 0x3) << 16)
#define SUN60I_DI_FORMAT_UV_SEQ		BIT(28)
#define SUN60I_DI_FIELD_ORDER		0x0038
#define SUN60I_DI_FIELD_ORDER_BFF	BIT(0)

#define SUN60I_DI_IN_F01_PITCH_Y	0x0040
#define SUN60I_DI_IN_F01_PITCH_C	0x0044
#define SUN60I_DI_IN_F01_PITCH_F23(v)	(((v) & 0xffff) << 16)
#define SUN60I_DI_OUT_DIT_PITCH_Y	0x0070
#define SUN60I_DI_OUT_DIT_PITCH_C	0x0074

/* in_f1 top and bottom field addresses, each with packed 40-bit MSBs. */
#define SUN60I_DI_IN_F1_TOP_Y		0x00b0
#define SUN60I_DI_IN_F1_TOP_C		0x00b4
#define SUN60I_DI_IN_F1_TOP_HADDR	0x00bc
#define SUN60I_DI_IN_F1_BOT_Y		0x00c0
#define SUN60I_DI_IN_F1_BOT_C		0x00c4
#define SUN60I_DI_IN_F1_BOT_HADDR	0x00cc

#define SUN60I_DI_OUT_DIT1_Y		0x0110
#define SUN60I_DI_OUT_DIT1_C		0x0114
#define SUN60I_DI_OUT_DIT1_HADDR	0x011c

/* DIT block starts at 0x2000. */
#define SUN60I_DI_DIT_SETTING		0x2000
#define SUN60I_DI_DIT_MODE_LUMA		BIT(0)
#define SUN60I_DI_DIT_DIAG_INTP_EN	BIT(5)
#define SUN60I_DI_DIT_ELA_DEMO_EN	BIT(8)
#define SUN60I_DI_DIT_WEAVE_DEMO_EN	BIT(9)
#define SUN60I_DI_DIT_BLEND_DEMO_EN	BIT(10)
#define SUN60I_DI_DIT_MODE_CHROMA	BIT(16)
#define SUN60I_DI_DIT_ONE_FRAME		BIT(24)
#define SUN60I_DI_DIT_CHROMA_PARA0	0x2004
#define SUN60I_DI_DIT_CHROMA_PARA1	0x2008
#define SUN60I_DI_DIT_INTRA_PARA	0x200c
#define SUN60I_DI_DIT_INTER_PARA	0x2010
#define SUN60I_DI_DIT_CROP_H		0x2014
#define SUN60I_DI_DIT_CROP_V		0x2018
#define SUN60I_DI_DIT_DEMO_H		0x201c
#define SUN60I_DI_DIT_DEMO_V		0x2020

#define SUN60I_DI_MIN_WIDTH		32U
#define SUN60I_DI_MIN_HEIGHT		32U
#define SUN60I_DI_MAX_WIDTH		2048U
#define SUN60I_DI_MAX_HEIGHT		1280U
#define SUN60I_DI_MAX_STRIDE		32764U

#define SUN60I_DI_FORMAT_YUV420_SP	1
#define SUN60I_DI_JOB_TIMEOUT_MS	100

struct sun60i_di_ctx {
	struct v4l2_fh fh;
	struct sun60i_di_dev *dev;

	struct v4l2_pix_format src_fmt;
	struct v4l2_pix_format dst_fmt;

	unsigned int first_field;
	unsigned int field;
	bool aborting;
};

struct sun60i_di_dev {
	struct v4l2_device v4l2_dev;
	struct video_device vfd;
	struct device *dev;
	struct v4l2_m2m_dev *m2m_dev;

	/* Serializes device access and the vb2 queues. */
	struct mutex dev_mutex;
	/* Protects job_active against the IRQ and timeout worker. */
	spinlock_t irqlock;
	bool job_active;
	struct delayed_work watchdog;
	void __iomem *base;

	struct clk_bulk_data clocks[2];
	struct reset_control_bulk_data resets[2];
};

#endif
