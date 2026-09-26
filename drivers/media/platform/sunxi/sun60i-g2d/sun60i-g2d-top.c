// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */

#include <linux/io.h>
#include <linux/delay.h>

#include "sun60i-g2d.h"

void sun60i_g2d_top_enable(struct sun60i_g2d_dev *dev)
{
	u32 gates = SUN60I_G2D_TOP_MIXER_GATE | SUN60I_G2D_TOP_ROT_GATE;

	writel(gates, dev->base + SUN60I_G2D_TOP_SCLK_GATE);
	writel(gates, dev->base + SUN60I_G2D_TOP_HCLK_GATE);
	writel(gates, dev->base + SUN60I_G2D_TOP_AHB_RESET);
	writel(1, dev->base + SUN60I_G2D_TOP_MCLK_GATE);
}

void sun60i_g2d_top_disable(struct sun60i_g2d_dev *dev)
{
	writel(0, dev->base + SUN60I_G2D_TOP_MCLK_GATE);
	writel(0, dev->base + SUN60I_G2D_TOP_AHB_RESET);
	writel(0, dev->base + SUN60I_G2D_TOP_HCLK_GATE);
	writel(0, dev->base + SUN60I_G2D_TOP_SCLK_GATE);
}

void sun60i_g2d_top_reset_engine(struct sun60i_g2d_dev *dev,
				 enum sun60i_g2d_engine engine)
{
	u32 bit, value;

	if (engine == SUN60I_G2D_ENGINE_MIXER)
		bit = SUN60I_G2D_TOP_MIXER_GATE;
	else if (engine == SUN60I_G2D_ENGINE_ROTATE)
		bit = SUN60I_G2D_TOP_ROT_GATE;
	else
		return;

	value = readl(dev->base + SUN60I_G2D_TOP_AHB_RESET);
	writel(value & ~bit, dev->base + SUN60I_G2D_TOP_AHB_RESET);
	udelay(1);
	writel(value | bit, dev->base + SUN60I_G2D_TOP_AHB_RESET);
}
