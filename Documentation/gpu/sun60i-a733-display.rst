.. SPDX-License-Identifier: GPL-2.0-only
.. Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.

=============================
Allwinner A733 display output
=============================

The A733 display engine can route mixer 0 to one of five timing-controller
devices.  The driver implements HDMI (device 4) and the
common TCON-LCD timing path (devices 0, 1 and 2).

TCON-LCD capabilities
=====================

The three TCON-LCD instances share a register layout but not their output
wiring:

================  ================================================
Controller        Physical outputs
================  ================================================
TCON-LCD0         RGB0, LVDS0, DSI0, DSI1 (including dual DSI)
TCON-LCD1         DSI1
TCON-LCD2         RGB1, LVDS1
================  ================================================

Parallel RGB uses the TCON channel-0 timing and dot-clock implementation.
LVDS and DSI are not integrated into the TCON on this SoC: both depend on the
A733 Combo-PHY, while DSI additionally uses the A733 DSI 4.0 host controller.
Do not describe either block as a legacy ``sun6i`` DSI or TCON-integrated LVDS
device. Combo-PHY LVDS mode is supported for TCON-LCD0. The A733-specific
DSI host configuration is implemented in the ``sun6i_mipi_dsi`` driver.

Only one mixer 0 output is currently supported at a time.  A board switching
from HDMI to an LCD output must disable the HDMI path and set
``allwinner,tcon-id`` on ``mixer0`` to the selected LCD controller number.

Parallel RGB template
=====================

The following example is a template, not a description of a real panel.  Copy
it to a board DTS and replace the timing, supply, physical dimensions and
optional GPIO/backlight properties with values from the panel and board
schematics.  TCON-LCD0 uses ``rgb0_24pins``; TCON-LCD2 uses ``rgb1_24pins`` and
requires ``allwinner,tcon-id = <2>``.

.. code-block:: dts

   / {
           panel_rgb: panel-rgb {
                   compatible = "vendor,panel-model", "panel-dpi";
                   power-supply = <&panel_supply>;
                   width-mm = <154>;     /* replace */
                   height-mm = <86>;     /* replace */

                   panel-timing {
                           clock-frequency = <33000000>; /* replace */
                           hactive = <800>;
                           vactive = <480>;
                           hfront-porch = <40>;
                           hback-porch = <40>;
                           hsync-len = <48>;
                           vfront-porch = <13>;
                           vback-porch = <29>;
                           vsync-len = <3>;
                           hsync-active = <0>;
                           vsync-active = <0>;
                           de-active = <1>;
                           pixelclk-active = <0>;
                   };

                   port {
                           panel_rgb_in: endpoint {
                                   remote-endpoint = <&tcon_lcd0_out_rgb>;
                           };
                   };
           };
   };

   &mixer0 {
           allwinner,tcon-id = <0>;
   };

   &tcon_lcd0_out {
           tcon_lcd0_out_rgb: endpoint {
                   remote-endpoint = <&panel_rgb_in>;
           };
   };

   &tcon_lcd_top {
           status = "okay";
   };

   &tcon_lcd0 {
           pinctrl-names = "default";
           pinctrl-0 = <&rgb0_24pins>;
           status = "okay";
   };

   &tcon_top { status = "disabled"; };
   &tcon_tv { status = "disabled"; };
   &hdmi { status = "disabled"; };

LVDS template
=============

An LVDS board description uses a ``panel-lvds`` node, TCON-LCD0, Combo-PHY0
and the ``lvds0_pins`` group.  The panel must specify its actual
``data-mapping``, ``data-width`` and timing.  Enable ``dsi0_combophy`` together
with ``tcon_lcd0``; despite its name the block is shared by DSI0 and LVDS0.
The legacy TCON LVDS analog registers at offsets 0x220 and 0x224 do not control
the A733 transmitter.

.. code-block:: dts

   &dsi0_combophy {
           status = "okay";
   };

   &tcon_lcd0 {
           phys = <&dsi0_combophy>;
           phy-names = "lvds";
           pinctrl-names = "default";
           pinctrl-0 = <&lvds0_pins>;
           status = "okay";
   };

The BSP describes TCON-LCD2's LVDS1 output on the PJ ``lvds2``/``lvds3`` pin
banks, but does not associate it with either PD DSI Combo-PHY register block.
Its PHY topology therefore remains deliberately unspecified until that
hardware mapping is known.

DSI template
============

A DSI panel is a child of the selected DSI host and specifies its virtual
channel, lane count, pixel format and mode flags through its panel driver.
TCON-LCD0 can feed DSI0 or DSI1, while TCON-LCD1 can only feed DSI1.  Keep this
pipeline disabled in the SoC description and enable it in the board DTS
only after describing the panel, its supplies and graph endpoints. Use the
A733 compatible to select the DSI 4.0 configuration; the A31/A64 configuration
does not describe this hardware.
