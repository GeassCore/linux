// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * Allwinner A733 display Combo-PHY
 *
 * The block contains the analog transmitter and a dedicated display PLL.
 * It supports the mutually exclusive LVDS and MIPI D-PHY transmitter modes.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define SUN60I_DPHY_GCTL_REG		0x00
#define SUN60I_DPHY_GCTL_EN		BIT(0)
#define SUN60I_DPHY_GCTL_LANES		GENMASK(5, 4)
#define SUN60I_DPHY_TX_CTL_REG		0x04
#define SUN60I_DPHY_TX_CTL_CLK_CONT	BIT(28)
#define SUN60I_DPHY_TX_TIME0_REG	0x10
#define SUN60I_DPHY_TX_TIME1_REG	0x14
#define SUN60I_DPHY_TX_TIME2_REG	0x18
#define SUN60I_DPHY_TX_TIME3_REG	0x1c
#define SUN60I_DPHY_TX_TIME4_REG	0x20
#define SUN60I_DPHY_ANA0_REG		0x4c
#define SUN60I_DPHY_ANA1_REG		0x50
#define SUN60I_DPHY_ANA2_REG		0x54
#define SUN60I_DPHY_ANA3_REG		0x58
#define SUN60I_DPHY_ANA4_REG		0x5c
#define SUN60I_DPHY_DBG0_REG		0xe0
#define SUN60I_DPHY_DBG0_PLL_LOCK	BIT(28)

#define SUN60I_DISPLL_REG0		0x104
#define SUN60I_DISPLL_REG0_M1		GENMASK(3, 0)
#define SUN60I_DISPLL_REG0_M0		GENMASK(5, 4)
#define SUN60I_DISPLL_REG0_NDET		BIT(7)
#define SUN60I_DISPLL_REG0_N		GENMASK(15, 8)
#define SUN60I_DISPLL_REG0_P		GENMASK(19, 16)
#define SUN60I_DISPLL_REG0_PLL_EN	BIT(20)
#define SUN60I_DISPLL_REG0_EN_LVS	BIT(21)
#define SUN60I_DISPLL_REG0_LDO_EN	BIT(22)
#define SUN60I_DISPLL_REG0_CP36_EN	BIT(23)
#define SUN60I_DISPLL_REG0_M3		GENMASK(27, 24)
#define SUN60I_DISPLL_REG0_M2		GENMASK(29, 28)
#define SUN60I_DISPLL_REG0_UPDATE	BIT(31)

#define SUN60I_DISPLL_REG1		0x108
#define SUN60I_DISPLL_REG1_LOCKDET_EN	BIT(12)
#define SUN60I_DISPLL_REG1_LS_GATE	BIT(21)
#define SUN60I_DISPLL_REG1_HS_GATE	BIT(22)

#define SUN60I_DISPLL_REG2		0x10c

#define SUN60I_COMBO_PHY_REG0		0x110
#define SUN60I_COMBO_PHY_CP_EN		BIT(0)
#define SUN60I_COMBO_PHY_LDO_EN		BIT(1)
#define SUN60I_COMBO_PHY_LVDS_EN	BIT(2)
#define SUN60I_COMBO_PHY_MIPI_EN	BIT(3)
#define SUN60I_COMBO_PHY_REG1		0x114
#define SUN60I_COMBO_PHY_REG2		0x118

#define SUN60I_COMBO_PHY_VREF_A733	0x63
#define SUN60I_DISPLL_MIN_VCO		1260000000UL
#define SUN60I_DISPLL_MAX_VCO		2520000000UL
#define SUN60I_DISPLL_MAX_DIV		64

struct sun60i_a733_display_phy {
	struct device *dev;
	struct regmap *regs;
	struct clk *bus_clk;
	struct clk *ref_clk;
	struct reset_control *reset;
	struct phy *phy;
	struct phy_configure_opts_lvds lvds;
	struct phy_configure_opts_mipi_dphy mipi;
	enum phy_mode mode;
	bool configured;
};

static int sun60i_a733_display_phy_init(struct phy *phy)
{
	struct sun60i_a733_display_phy *dphy = phy_get_drvdata(phy);
	int ret;

	ret = reset_control_deassert(dphy->reset);
	if (ret)
		return ret;

	ret = clk_prepare_enable(dphy->bus_clk);
	if (ret)
		reset_control_assert(dphy->reset);

	return ret;
}

static int sun60i_a733_display_phy_exit(struct phy *phy)
{
	struct sun60i_a733_display_phy *dphy = phy_get_drvdata(phy);

	clk_disable_unprepare(dphy->bus_clk);
	reset_control_assert(dphy->reset);

	return 0;
}

static int sun60i_a733_display_phy_set_mode(struct phy *phy,
					    enum phy_mode mode, int submode)
{
	struct sun60i_a733_display_phy *dphy = phy_get_drvdata(phy);

	if (mode != PHY_MODE_LVDS && mode != PHY_MODE_MIPI_DPHY)
		return -EOPNOTSUPP;

	dphy->mode = mode;
	return 0;
}

static int sun60i_a733_display_phy_validate(struct phy *phy,
					     enum phy_mode mode, int submode,
					     union phy_configure_opts *opts)
{
	struct phy_configure_opts_lvds *lvds = &opts->lvds;
	unsigned long serial_rate;

	if (mode == PHY_MODE_MIPI_DPHY)
		return phy_mipi_dphy_config_validate(&opts->mipi_dphy);
	if (mode != PHY_MODE_LVDS)
		return -EOPNOTSUPP;

	if (lvds->bits_per_lane_and_dclk_cycle != 7 || lvds->lanes != 4 ||
	    lvds->is_slave || !lvds->differential_clk_rate)
		return -EINVAL;

	serial_rate = lvds->differential_clk_rate * 7;
	if (serial_rate > SUN60I_DISPLL_MAX_VCO ||
	    serial_rate * SUN60I_DISPLL_MAX_DIV < SUN60I_DISPLL_MIN_VCO)
		return -ERANGE;

	return 0;
}

static int sun60i_a733_display_phy_configure(struct phy *phy,
					      union phy_configure_opts *opts)
{
	struct sun60i_a733_display_phy *dphy = phy_get_drvdata(phy);
	int ret;

	ret = sun60i_a733_display_phy_validate(phy, phy->attrs.mode, 0, opts);
	if (ret)
		return ret;

	if (dphy->mode == PHY_MODE_MIPI_DPHY)
		dphy->mipi = opts->mipi_dphy;
	else
		dphy->lvds = opts->lvds;
	dphy->configured = true;

	return 0;
}

static int sun60i_a733_displl_enable(struct sun60i_a733_display_phy *dphy)
{
	unsigned long ref_rate = clk_get_rate(dphy->ref_clk);
	unsigned long serial_rate = dphy->lvds.differential_clk_rate * 7;
	unsigned long best_diff = ULONG_MAX;
	unsigned int best_div = 0, best_n = 0;
	unsigned int div, n, m2, m3;
	u32 val;
	int ret;

	if (!ref_rate)
		return -EINVAL;

	/*
	 * The LVDS serializer is fed by the DISPLL M2/M3 output.  Search the
	 * small integer divider space and keep the VCO in the BSP range.
	 */
	for (div = 1; div <= SUN60I_DISPLL_MAX_DIV; div++) {
		unsigned long actual, diff, vco = serial_rate * div;

		if (vco < SUN60I_DISPLL_MIN_VCO || vco > SUN60I_DISPLL_MAX_VCO)
			continue;

		n = DIV_ROUND_CLOSEST(vco, ref_rate);
		if (!n || n > FIELD_MAX(SUN60I_DISPLL_REG0_N))
			continue;

		actual = ref_rate * n / div;
		diff = actual > serial_rate ? actual - serial_rate :
						serial_rate - actual;
		if (diff < best_diff) {
			best_diff = diff;
			best_div = div;
			best_n = n;
		}
	}

	if (!best_div)
		return -ERANGE;

	/* M2 is two bits and M3 is four bits; both fields store divider - 1. */
	for (m2 = 1; m2 <= 4; m2++)
		if (!(best_div % m2) && best_div / m2 <= 16)
			break;
	if (m2 > 4)
		return -ERANGE;
	m3 = best_div / m2;

	val = FIELD_PREP(SUN60I_DISPLL_REG0_N, best_n) |
	      SUN60I_DISPLL_REG0_NDET |
	      SUN60I_DISPLL_REG0_CP36_EN |
	      FIELD_PREP(SUN60I_DISPLL_REG0_M2, m2 - 1) |
	      FIELD_PREP(SUN60I_DISPLL_REG0_M3, m3 - 1);
	regmap_write(dphy->regs, SUN60I_DISPLL_REG2, 0);
	regmap_write(dphy->regs, SUN60I_DISPLL_REG0, val);
	regmap_update_bits(dphy->regs, SUN60I_DISPLL_REG1,
			   SUN60I_DISPLL_REG1_LOCKDET_EN |
			   SUN60I_DISPLL_REG1_LS_GATE |
			   SUN60I_DISPLL_REG1_HS_GATE,
			   SUN60I_DISPLL_REG1_LOCKDET_EN |
			   SUN60I_DISPLL_REG1_LS_GATE |
			   SUN60I_DISPLL_REG1_HS_GATE);
	regmap_update_bits(dphy->regs, SUN60I_DISPLL_REG0,
			   SUN60I_DISPLL_REG0_PLL_EN |
			   SUN60I_DISPLL_REG0_EN_LVS |
			   SUN60I_DISPLL_REG0_LDO_EN |
			   SUN60I_DISPLL_REG0_UPDATE,
			   SUN60I_DISPLL_REG0_PLL_EN |
			   SUN60I_DISPLL_REG0_EN_LVS |
			   SUN60I_DISPLL_REG0_LDO_EN |
			   SUN60I_DISPLL_REG0_UPDATE);

	ret = regmap_read_poll_timeout(dphy->regs, SUN60I_DPHY_DBG0_REG, val,
				       val & SUN60I_DPHY_DBG0_PLL_LOCK,
				       5, 20000);
	if (ret)
		dev_err(dphy->dev, "DISPLL failed to lock at %lu Hz\n",
			serial_rate);

	return ret;
}

static void sun60i_a733_displl_disable(struct sun60i_a733_display_phy *dphy)
{
	regmap_update_bits(dphy->regs, SUN60I_DISPLL_REG0,
			   SUN60I_DISPLL_REG0_PLL_EN |
			   SUN60I_DISPLL_REG0_EN_LVS |
			   SUN60I_DISPLL_REG0_LDO_EN |
			   SUN60I_DISPLL_REG0_UPDATE, 0);
	regmap_update_bits(dphy->regs, SUN60I_DISPLL_REG1,
			   SUN60I_DISPLL_REG1_LOCKDET_EN |
			   SUN60I_DISPLL_REG1_LS_GATE |
			   SUN60I_DISPLL_REG1_HS_GATE, 0);
	udelay(20);
}

static int sun60i_a733_displl_enable_mipi(struct sun60i_a733_display_phy *dphy)
{
	unsigned long rate = dphy->mipi.hs_clk_rate;
	unsigned long ref = clk_get_rate(dphy->ref_clk);
	unsigned int mult, m1, n;
	u32 val;
	int ret;

	if (!ref || !rate || rate > 2144000000UL)
		return -ERANGE;

	if (rate <= 264000000) {
		mult = 8;
		m1 = 7;
	} else if (rate <= 536000000) {
		mult = 4;
		m1 = 3;
	} else if (rate <= 1072000000) {
		mult = 2;
		m1 = 1;
	} else {
		mult = 1;
		m1 = 0;
	}

	n = DIV_ROUND_CLOSEST_ULL((u64)rate * mult, ref);
	if (!n || n > FIELD_MAX(SUN60I_DISPLL_REG0_N))
		return -ERANGE;

	/* BSP sun60iw2: HS=M0/M1, LS=M2/M3 and LS is HS / 4. */
	val = FIELD_PREP(SUN60I_DISPLL_REG0_N, n) |
	      SUN60I_DISPLL_REG0_NDET | SUN60I_DISPLL_REG0_CP36_EN |
	      FIELD_PREP(SUN60I_DISPLL_REG0_M1, m1) |
	      FIELD_PREP(SUN60I_DISPLL_REG0_M2, 3) |
	      FIELD_PREP(SUN60I_DISPLL_REG0_M3, m1);
	regmap_write(dphy->regs, SUN60I_DISPLL_REG2, 0);
	regmap_write(dphy->regs, SUN60I_DISPLL_REG0, val);
	regmap_update_bits(dphy->regs, SUN60I_DISPLL_REG1,
			   SUN60I_DISPLL_REG1_LOCKDET_EN |
			   SUN60I_DISPLL_REG1_LS_GATE |
			   SUN60I_DISPLL_REG1_HS_GATE,
			   SUN60I_DISPLL_REG1_LOCKDET_EN |
			   SUN60I_DISPLL_REG1_LS_GATE |
			   SUN60I_DISPLL_REG1_HS_GATE);
	regmap_update_bits(dphy->regs, SUN60I_DISPLL_REG0,
			   SUN60I_DISPLL_REG0_PLL_EN |
			   SUN60I_DISPLL_REG0_EN_LVS |
			   SUN60I_DISPLL_REG0_LDO_EN |
			   SUN60I_DISPLL_REG0_UPDATE,
			   SUN60I_DISPLL_REG0_PLL_EN |
			   SUN60I_DISPLL_REG0_EN_LVS |
			   SUN60I_DISPLL_REG0_LDO_EN |
			   SUN60I_DISPLL_REG0_UPDATE);

	ret = regmap_read_poll_timeout(dphy->regs, SUN60I_DPHY_DBG0_REG, val,
				       val & SUN60I_DPHY_DBG0_PLL_LOCK, 5, 20000);
	if (ret)
		dev_err(dphy->dev, "MIPI DISPLL failed to lock at %lu Hz\n", rate);

	return ret;
}

static int sun60i_a733_mipi_power_on(struct sun60i_a733_display_phy *dphy)
{
	u32 lanes = GENMASK(dphy->mipi.lanes - 1, 0);
	int ret;

	regmap_write(dphy->regs, SUN60I_DPHY_TX_TIME0_REG,
		     (9 << 24) | (6 << 16) | 0x0e);
	regmap_write(dphy->regs, SUN60I_DPHY_TX_TIME1_REG,
		     (10 << 24) | (3 << 16) | (50 << 8) | 7);
	regmap_write(dphy->regs, SUN60I_DPHY_TX_TIME2_REG, 30);
	regmap_write(dphy->regs, SUN60I_DPHY_TX_TIME3_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_TX_TIME4_REG, 0x303);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA0_REG, 0x00777777);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA4_REG, 0xe4b4327f);
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG1, 0x63);
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG0,
		     SUN60I_COMBO_PHY_CP_EN | SUN60I_COMBO_PHY_LDO_EN |
		     SUN60I_COMBO_PHY_MIPI_EN);
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG2, 20);

	regmap_write(dphy->regs, SUN60I_DPHY_ANA2_REG,
		     BIT(1) | BIT(4) | FIELD_PREP(GENMASK(27, 24), lanes));
	regmap_write(dphy->regs, SUN60I_DPHY_ANA3_REG,
		     BIT(18) | BIT(24) | BIT(25) | BIT(26) | BIT(27) |
		     FIELD_PREP(GENMASK(31, 28), lanes));
	regmap_write(dphy->regs, SUN60I_DPHY_ANA1_REG, BIT(31));
	regmap_write(dphy->regs, SUN60I_DPHY_TX_CTL_REG,
		     SUN60I_DPHY_TX_CTL_CLK_CONT);
	regmap_write(dphy->regs, SUN60I_DPHY_GCTL_REG,
		     SUN60I_DPHY_GCTL_EN |
		     FIELD_PREP(SUN60I_DPHY_GCTL_LANES, dphy->mipi.lanes - 1));

	ret = sun60i_a733_displl_enable_mipi(dphy);
	if (ret)
		regmap_write(dphy->regs, SUN60I_DPHY_GCTL_REG, 0);

	return ret;
}

static int sun60i_a733_display_phy_power_on(struct phy *phy)
{
	struct sun60i_a733_display_phy *dphy = phy_get_drvdata(phy);
	int ret;

	if (!dphy->configured)
		return -EINVAL;
	if (dphy->mode == PHY_MODE_MIPI_DPHY)
		return sun60i_a733_mipi_power_on(dphy);

	/* A733 LVDS analog sequence from the vendor display PHY driver. */
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG1,
		     SUN60I_COMBO_PHY_VREF_A733);
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG0,
		     SUN60I_COMBO_PHY_CP_EN);
	udelay(5);
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG0,
		     SUN60I_COMBO_PHY_CP_EN | SUN60I_COMBO_PHY_LVDS_EN);
	udelay(5);
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG0,
		     SUN60I_COMBO_PHY_CP_EN | SUN60I_COMBO_PHY_LDO_EN |
		     SUN60I_COMBO_PHY_LVDS_EN);

	regmap_write(dphy->regs, SUN60I_DPHY_ANA4_REG, 0x84000000);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA3_REG, 0x01040000);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA2_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA1_REG, 0);

	ret = sun60i_a733_displl_enable(dphy);
	if (ret) {
		regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG1, 0);
		regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG0, 0);
		regmap_write(dphy->regs, SUN60I_DPHY_ANA4_REG, 0);
		regmap_write(dphy->regs, SUN60I_DPHY_ANA3_REG, 0);
	}

	return ret;
}

static int sun60i_a733_display_phy_power_off(struct phy *phy)
{
	struct sun60i_a733_display_phy *dphy = phy_get_drvdata(phy);

	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG1, 0);
	regmap_write(dphy->regs, SUN60I_COMBO_PHY_REG0, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_GCTL_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_TX_CTL_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_TX_TIME0_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA0_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA4_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA3_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA2_REG, 0);
	regmap_write(dphy->regs, SUN60I_DPHY_ANA1_REG, 0);
	sun60i_a733_displl_disable(dphy);

	return 0;
}

static const struct phy_ops sun60i_a733_display_phy_ops = {
	.init = sun60i_a733_display_phy_init,
	.exit = sun60i_a733_display_phy_exit,
	.set_mode = sun60i_a733_display_phy_set_mode,
	.configure = sun60i_a733_display_phy_configure,
	.validate = sun60i_a733_display_phy_validate,
	.power_on = sun60i_a733_display_phy_power_on,
	.power_off = sun60i_a733_display_phy_power_off,
	.owner = THIS_MODULE,
};

static const struct regmap_config sun60i_a733_display_phy_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0x138,
};

static int sun60i_a733_display_phy_probe(struct platform_device *pdev)
{
	struct sun60i_a733_display_phy *dphy;
	struct phy_provider *provider;
	void __iomem *base;

	dphy = devm_kzalloc(&pdev->dev, sizeof(*dphy), GFP_KERNEL);
	if (!dphy)
		return -ENOMEM;

	dphy->dev = &pdev->dev;
	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	dphy->regs = devm_regmap_init_mmio(&pdev->dev, base,
					   &sun60i_a733_display_phy_regmap_config);
	if (IS_ERR(dphy->regs))
		return PTR_ERR(dphy->regs);

	dphy->bus_clk = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(dphy->bus_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->bus_clk),
				     "failed to get bus clock\n");

	dphy->ref_clk = devm_clk_get(&pdev->dev, "ref");
	if (IS_ERR(dphy->ref_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->ref_clk),
				     "failed to get reference clock\n");

	dphy->reset = devm_reset_control_get_shared(&pdev->dev, NULL);
	if (IS_ERR(dphy->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->reset),
				     "failed to get reset\n");

	dphy->phy = devm_phy_create(&pdev->dev, NULL,
				    &sun60i_a733_display_phy_ops);
	if (IS_ERR(dphy->phy))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->phy),
				     "failed to create PHY\n");

	phy_set_drvdata(dphy->phy, dphy);
	provider = devm_of_phy_provider_register(&pdev->dev,
						 of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id sun60i_a733_display_phy_of_match[] = {
	{ .compatible = "allwinner,sun60i-a733-display-combo-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun60i_a733_display_phy_of_match);

static struct platform_driver sun60i_a733_display_phy_driver = {
	.probe = sun60i_a733_display_phy_probe,
	.driver = {
		.name = "sun60i-a733-display-combo-phy",
		.of_match_table = sun60i_a733_display_phy_of_match,
	},
};
module_platform_driver(sun60i_a733_display_phy_driver);

MODULE_AUTHOR("OpenAI Codex");
MODULE_DESCRIPTION("Allwinner A733 display Combo-PHY driver");
MODULE_LICENSE("GPL");
