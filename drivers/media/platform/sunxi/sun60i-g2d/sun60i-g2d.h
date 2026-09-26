/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
#ifndef _SUN60I_G2D_H_
#define _SUN60I_G2D_H_

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "sun60i-g2d-regs.h"

#define SUN60I_G2D_NAME		"sun60i-g2d"
#define SUN60I_G2D_MIN_WIDTH	8U
#define SUN60I_G2D_MIN_HEIGHT	8U
#define SUN60I_G2D_MAX_WIDTH	4096U
#define SUN60I_G2D_MAX_HEIGHT	4096U

struct sun60i_g2d_ctx {
	struct v4l2_fh fh;
	struct sun60i_g2d_dev *dev;
	struct v4l2_pix_format src_fmt;
	struct v4l2_pix_format dst_fmt;
	struct v4l2_ctrl_handler ctrl_handler;
	u32 hflip;
	u32 vflip;
	u32 rotate;
};

struct sun60i_g2d_rcq {
	void *cpu;
	dma_addr_t dma;
	size_t size;
	size_t used;
	unsigned int blocks;
};

enum sun60i_g2d_engine {
	SUN60I_G2D_ENGINE_NONE,
	SUN60I_G2D_ENGINE_ROTATE,
	SUN60I_G2D_ENGINE_MIXER,
};

struct sun60i_g2d_dev {
	struct v4l2_device v4l2_dev;
	struct video_device vfd;
	struct device *dev;
	struct v4l2_m2m_dev *m2m_dev;
	/* Serializes file operations and both V4L2 queues. */
	struct mutex dev_mutex;
	void __iomem *base;
	void __iomem *rot_base;
	struct clk_bulk_data clks[4];
	struct reset_control_bulk_data resets[2];
	struct sun60i_g2d_rcq rcq;
	enum sun60i_g2d_engine engine;
	spinlock_t irqlock;
	struct delayed_work watchdog;
	bool job_active;
};

void sun60i_g2d_top_enable(struct sun60i_g2d_dev *dev);
void sun60i_g2d_top_disable(struct sun60i_g2d_dev *dev);
void sun60i_g2d_top_reset_engine(struct sun60i_g2d_dev *dev,
				 enum sun60i_g2d_engine engine);

int sun60i_g2d_rcq_init(struct sun60i_g2d_dev *dev);
void sun60i_g2d_rcq_cleanup(struct sun60i_g2d_dev *dev);
void sun60i_g2d_rcq_reset(struct sun60i_g2d_dev *dev);
void *sun60i_g2d_rcq_add(struct sun60i_g2d_dev *dev, u32 offset, size_t size);
void sun60i_g2d_rcq_submit(struct sun60i_g2d_dev *dev);
bool sun60i_g2d_rcq_irq(struct sun60i_g2d_dev *dev);

int sun60i_g2d_mixer_run(struct sun60i_g2d_ctx *ctx,
			struct vb2_v4l2_buffer *src,
			struct vb2_v4l2_buffer *dst);

#endif
