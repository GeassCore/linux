#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SMOKE_BIN=${PVR_SMOKE_BIN:-"$SCRIPT_DIR/pvr-drm-smoke"}
FAILURES=0
SKIPS=0

pass()
{
	printf 'PASS: %s\n' "$*"
}

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	FAILURES=$((FAILURES + 1))
}

skip()
{
	printf 'SKIP: %s\n' "$*"
	SKIPS=$((SKIPS + 1))
}

find_powervr_render_node()
{
	for node in /sys/class/drm/renderD*; do
		[ -e "$node/dev" ] || continue
		driver=$(basename "$(readlink -f "$node/device/driver" 2>/dev/null)" 2>/dev/null)
		[ "$driver" = powervr ] || continue
		printf '/dev/dri/%s\n' "$(basename "$node")"
		return 0
	done
	return 1
}

printf 'INFO: PowerVR smoke and functional test (no stress)\n'
printf 'INFO: kernel: '
uname -a

if [ -r /proc/config.gz ] && command -v zcat >/dev/null 2>&1; then
	if zcat /proc/config.gz | grep -q '^CONFIG_DRM_POWERVR=y$'; then
		pass 'CONFIG_DRM_POWERVR=y'
	else
		fail 'CONFIG_DRM_POWERVR is not built in'
	fi
else
	skip '/proc/config.gz is unavailable; kernel config not checked'
fi

RENDER_NODE=$(find_powervr_render_node || true)
if [ -n "$RENDER_NODE" ] && [ -c "$RENDER_NODE" ]; then
	pass "PowerVR render node found: $RENDER_NODE"
else
	fail 'PowerVR render node not found'
fi

printf '%s\n' '--- PowerVR-related kernel log ---'
dmesg | grep -Ei 'powervr|pvr|rogue|(^|[[:space:]])gpu([[:space:]:]|$)' | tail -n 120 || true

PVR_ERRORS=$(dmesg | grep -Ei 'powervr|pvr|rogue' | \
	grep -Ei 'fail|error|timeout|timed out|not found|unsupported|fault' || true)
if [ -n "$PVR_ERRORS" ]; then
	printf '%s\n' "$PVR_ERRORS" >&2
	fail 'PowerVR error messages found in dmesg'
else
	pass 'no PowerVR failure signature in dmesg'
fi

if [ -n "$RENDER_NODE" ] && [ -x "$SMOKE_BIN" ]; then
	if "$SMOKE_BIN" "$RENDER_NODE"; then
		pass 'PowerVR DRM query/BO/VM functional test'
	else
		fail 'PowerVR DRM query/BO/VM functional test'
	fi
elif [ ! -x "$SMOKE_BIN" ]; then
	fail "missing executable smoke binary: $SMOKE_BIN"
fi

if command -v vulkaninfo >/dev/null 2>&1; then
	if command -v timeout >/dev/null 2>&1; then
		if timeout 30 vulkaninfo --summary; then
			pass 'Vulkan userspace initialization'
		else
			fail 'vulkaninfo --summary'
		fi
	else
		skip 'timeout is unavailable; refusing to run unbounded vulkaninfo'
	fi
else
	skip 'vulkaninfo is unavailable; GPU job submission not tested'
fi

if [ "$FAILURES" -ne 0 ]; then
	printf 'FAIL: PowerVR test completed with %u failure(s), %u skip(s)\n' \
		"$FAILURES" "$SKIPS" >&2
	exit 1
fi

printf 'PASS: PowerVR test completed with %u skip(s)\n' "$SKIPS"
