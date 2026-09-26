#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.

set -eu

KSFT_SKIP=4
SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEVICE=${1:-}
RESULT_DIR=${2:-}

if [ -z "$DEVICE" ]; then
	for name in /sys/class/video4linux/video*/name; do
		[ -r "$name" ] || continue
		[ "$(cat "$name")" = "sun60i-di" ] || continue
		DEVICE=/dev/$(basename "$(dirname "$name")")
		break
	done
fi

if [ -z "$DEVICE" ] || [ ! -c "$DEVICE" ]; then
	echo "SKIP: sun60i-di video device not found"
	exit "$KSFT_SKIP"
fi

if [ -z "$RESULT_DIR" ]; then
	RESULT_DIR=$(mktemp -d /root/sun60i-di-test.XXXXXX)
else
	mkdir -p "$RESULT_DIR"
fi

echo "device=$DEVICE"
echo "results=$RESULT_DIR"
dmesg > "$RESULT_DIR/dmesg.before"
v4l2-ctl -d "$DEVICE" --all > "$RESULT_DIR/v4l2-info.txt"

if "$SELF_DIR/sun60i_di_test" "$DEVICE" "$RESULT_DIR" \
		> "$RESULT_DIR/test.log" 2>&1; then
	cat "$RESULT_DIR/test.log"
	dmesg > "$RESULT_DIR/dmesg.after"
	echo "PASS: sun60i-di functional test"
	exit 0
else
	status=$?
fi

cat "$RESULT_DIR/test.log"
dmesg > "$RESULT_DIR/dmesg.after"
echo "FAIL: sun60i-di functional test (status=$status)"
exit "$status"
