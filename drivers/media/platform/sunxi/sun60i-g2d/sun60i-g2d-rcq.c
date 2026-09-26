// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/string.h>

#include "sun60i-g2d.h"

#define SUN60I_G2D_RCQ_SIZE	4096
#define SUN60I_G2D_RCQ_HEADERS	4

struct sun60i_g2d_rcq_header {
	u32 addr_lo;
	u32 len_addr_hi;
	u32 dirty;
	u32 offset;
};

int sun60i_g2d_rcq_init(struct sun60i_g2d_dev *dev)
{
	dev->rcq.size = SUN60I_G2D_RCQ_SIZE;
	dev->rcq.cpu = dma_alloc_coherent(dev->dev, dev->rcq.size,
					  &dev->rcq.dma, GFP_KERNEL);
	if (!dev->rcq.cpu)
		return -ENOMEM;

	return 0;
}

void sun60i_g2d_rcq_cleanup(struct sun60i_g2d_dev *dev)
{
	if (dev->rcq.cpu)
		dma_free_coherent(dev->dev, dev->rcq.size, dev->rcq.cpu,
				  dev->rcq.dma);
}

void sun60i_g2d_rcq_reset(struct sun60i_g2d_dev *dev)
{
	memset(dev->rcq.cpu, 0, dev->rcq.size);
	dev->rcq.used = ALIGN(sizeof(struct sun60i_g2d_rcq_header) *
			      SUN60I_G2D_RCQ_HEADERS, 32);
	dev->rcq.blocks = 0;
}

void *sun60i_g2d_rcq_add(struct sun60i_g2d_dev *dev, u32 offset, size_t size)
{
	struct sun60i_g2d_rcq_header *headers = dev->rcq.cpu;
	struct sun60i_g2d_rcq_header *header;
	dma_addr_t dma;
	void *cpu;

	if (dev->rcq.blocks >= SUN60I_G2D_RCQ_HEADERS ||
	    dev->rcq.used + ALIGN(size, 32) > dev->rcq.size)
		return NULL;

	header = &headers[dev->rcq.blocks++];
	cpu = (u8 *)dev->rcq.cpu + dev->rcq.used;
	dma = dev->rcq.dma + dev->rcq.used;
	header->addr_lo = lower_32_bits(dma);
	header->len_addr_hi = (size & GENMASK(23, 0)) |
			      ((upper_32_bits(dma) & 0xff) << 24);
	header->dirty = 1;
	header->offset = offset;
	dev->rcq.used += ALIGN(size, 32);

	return cpu;
}

void sun60i_g2d_rcq_submit(struct sun60i_g2d_dev *dev)
{
	dma_wmb();
	writel(0, dev->base + SUN60I_G2D_TOP_RCQ_CTL);
	writel(0, dev->base + SUN60I_G2D_TOP_RCQ_IRQ_CTL);
	writel(lower_32_bits(dev->rcq.dma),
	       dev->base + SUN60I_G2D_TOP_RCQ_ADDR_LO);
	writel(upper_32_bits(dev->rcq.dma) & 0xff,
	       dev->base + SUN60I_G2D_TOP_RCQ_ADDR_HI);
	writel(dev->rcq.blocks * sizeof(struct sun60i_g2d_rcq_header),
	       dev->base + SUN60I_G2D_TOP_RCQ_LEN);
	writel(SUN60I_G2D_TOP_RCQ_TASK_IRQ_EN,
	       dev->base + SUN60I_G2D_TOP_RCQ_IRQ_CTL);
	writel(SUN60I_G2D_TOP_RCQ_UPDATE,
	       dev->base + SUN60I_G2D_TOP_RCQ_CTL);
}

bool sun60i_g2d_rcq_irq(struct sun60i_g2d_dev *dev)
{
	u32 status = readl(dev->base + SUN60I_G2D_TOP_RCQ_STATUS);

	if (!(status & SUN60I_G2D_TOP_RCQ_TASK_IRQ))
		return false;

	writel(SUN60I_G2D_TOP_RCQ_TASK_IRQ,
	       dev->base + SUN60I_G2D_TOP_RCQ_STATUS);
	writel(0, dev->base + SUN60I_G2D_TOP_RCQ_IRQ_CTL);
	return true;
}
