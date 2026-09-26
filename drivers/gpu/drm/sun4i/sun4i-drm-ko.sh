#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
# Load/unload sun4i DRM modules stored next to this script.
#
# Copy alongside:
#   sun4i-drm.ko  sun60i-de.ko  sun8i_tcon_top.ko  sun4i-tcon.ko
#   sun8i-drm-hdmi.ko
# Usage:
#   ./sun4i-drm-ko.sh load | unload | reload | status
#
# Requires CONFIG_MODULE_UNLOAD=y and CONFIG_DRM_SUN4I=m (and related =m).
# Shared helpers (dw_hdmi, drm_kms_helper, …) come from /lib/modules via modprobe.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"

KO_DRM="$DIR/sun4i-drm.ko"
KO_DE="$DIR/sun60i-de.ko"
KO_TCON_TOP="$DIR/sun8i_tcon_top.ko"
KO_TCON="$DIR/sun4i-tcon.ko"
KO_HDMI="$DIR/sun8i-drm-hdmi.ko"
KO_MIXER="$DIR/sun8i-mixer.ko"

ensure_helpers() {
	# Core DRM may be built-in (=y) or modular (=m).
	if ! lsmod | grep -q '^drm '; then
		modprobe drm 2>/dev/null || true
	fi
	if ! lsmod | grep -q '^drm_kms_helper '; then
		modprobe drm_kms_helper 2>/dev/null || true
	fi
	if ! lsmod | grep -q '^drm_dma_helper '; then
		modprobe drm_dma_helper 2>/dev/null || true
	fi
	if ! lsmod | grep -q '^dw_hdmi '; then
		modprobe dw_hdmi 2>/dev/null || true
	fi
}

unload() {
	killall modetest 2>/dev/null || true

	# Master first, then components (reverse of load).
	# Module names follow .ko basenames (- → _).
	# Retry leftovers so a partial load cannot block the next reload.
	i=0
	while [ "$i" -lt 5 ]; do
		rmmod sun4i_drm 2>/dev/null || true
		rmmod sun8i_drm_hdmi 2>/dev/null || true
		rmmod sun4i_tcon 2>/dev/null || true
		rmmod sun60i_de 2>/dev/null || true
		rmmod sun8i_tcon_top 2>/dev/null || true
		rmmod sun8i_mixer 2>/dev/null || true
		if ! lsmod | grep -qE \
			'^(sun4i_drm|sun8i_drm_hdmi|sun4i_tcon|sun60i_de|sun8i_tcon_top|sun8i_mixer) '; then
			break
		fi
		i=$((i + 1))
		sleep 0.2
	done

	echo "unload: done"
}

load() {
	for f in "$KO_DRM" "$KO_DE" "$KO_TCON_TOP" "$KO_TCON" "$KO_HDMI"; do
		if [ ! -f "$f" ]; then
			echo "load: missing $f" >&2
			exit 1
		fi
	done

	ensure_helpers

	if [ -f "$KO_MIXER" ]; then
		insmod "$KO_MIXER"
	fi
	insmod "$KO_TCON_TOP"
	insmod "$KO_TCON"
	insmod "$KO_DE"
	insmod "$KO_HDMI"
	insmod "$KO_DRM"

	echo "load: done"
	status
}

status() {
	echo "=== sun4i / helpers ==="
	lsmod | grep -E 'sun4i|sun60i|sun8i|dw_hdmi|drm_kms|drm_dma|^drm ' || true
	echo "=== /dev/dri ==="
	ls -l /dev/dri/ 2>/dev/null || echo "(no /dev/dri)"
	echo "=== connector ==="
	ls /sys/class/drm/ 2>/dev/null || true
}

reload() {
	unload
	sleep 0.3
	load
}

usage() {
	echo "Usage: $0 load | unload | reload | status" >&2
	exit 1
}

case "${1:-}" in
load|unload|reload|status) "$1" ;;
*) usage ;;
esac
