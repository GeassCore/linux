#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.

set -eu

WIDTH=${WIDTH:-64}
HEIGHT=${HEIGHT:-48}
DEVICE=${DEVICE:-}
WORKDIR=${WORKDIR:-/tmp/sun60i-g2d-test}

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

command -v v4l2-ctl >/dev/null 2>&1 || fail "v4l2-ctl is required"

if [ -z "$DEVICE" ]; then
	for node in /dev/video*; do
		[ -e "$node" ] || continue
		if v4l2-ctl -d "$node" -D 2>/dev/null |
			grep -q 'Driver name.*sun60i-g2d'; then
			DEVICE=$node
			break
		fi
	done
fi

[ -n "$DEVICE" ] || fail "sun60i-g2d V4L2 node not found"
mkdir -p "$WORKDIR"
INPUT=$WORKDIR/input.argb

# A non-zero ARGB8888 frame is sufficient for DMA/data-path smoke coverage;
# geometric correctness is checked by output dimensions.
dd if=/dev/urandom of="$INPUT" bs=$((WIDTH * HEIGHT * 4)) count=1 2>/dev/null

echo "device: $DEVICE"
v4l2-ctl -d "$DEVICE" -D
v4l2-ctl -d "$DEVICE" --list-formats-out
v4l2-ctl -d "$DEVICE" --list-formats

run_transform()
{
	input=$1
	in_w=$2
	in_h=$3
	angle=$4
	output=$5
	hflip=${6:-0}
	vflip=${7:-0}
	if [ "$angle" = 90 ] || [ "$angle" = 270 ]; then
		out_w=$in_h
		out_h=$in_w
	else
		out_w=$in_w
		out_h=$in_h
	fi

	controls=rotate=$angle,horizontal_flip=$hflip,vertical_flip=$vflip
	v4l2-ctl -d "$DEVICE" \
		--set-ctrl="$controls" \
		--set-fmt-video-out=width="$in_w",height="$in_h",pixelformat=AR24 \
		--set-fmt-video=width="$out_w",height="$out_h",pixelformat=AR24 \
		--stream-out-mmap=1 --stream-mmap=2 \
		--stream-from="$input" --stream-to="$output" --stream-count=1

	[ "$(wc -c < "$output")" -eq $((out_w * out_h * 4)) ] ||
		fail "rotation $angle produced an unexpected output size"

	echo "$out_w $out_h"
}

run_mixer()
{
	input=$1
	in_w=$2
	in_h=$3
	in_fmt=$4
	output=$5
	out_w=$6
	out_h=$7
	out_fmt=$8

	v4l2-ctl -d "$DEVICE" \
		--set-ctrl=rotate=0,horizontal_flip=0,vertical_flip=0 \
		--set-fmt-video-out=width="$in_w",height="$in_h",pixelformat="$in_fmt" \
		--set-fmt-video=width="$out_w",height="$out_h",pixelformat="$out_fmt" \
		--stream-out-mmap=1 --stream-mmap=2 \
		--stream-from="$input" --stream-to="$output" --stream-count=1
}

run_case()
{
	angle=$1
	output=$WORKDIR/output-$angle.argb
	run_transform "$INPUT" "$WIDTH" "$HEIGHT" "$angle" "$output" >/dev/null
	cmp -s "$INPUT" "$output" || [ "$angle" -ne 0 ] ||
		fail "0-degree copy differs from input"
	echo "PASS: ARGB8888 rotation $angle"
}

run_case 0
run_case 90
run_case 180
run_case 270

run_transform "$WORKDIR/output-90.argb" "$HEIGHT" "$WIDTH" 270 \
	"$WORKDIR/roundtrip-90-270.argb" >/dev/null
cmp -s "$INPUT" "$WORKDIR/roundtrip-90-270.argb" ||
	fail "90+270 degree round trip differs from input"
echo "PASS: ARGB8888 90+270 degree round trip"

run_transform "$WORKDIR/output-180.argb" "$WIDTH" "$HEIGHT" 180 \
	"$WORKDIR/roundtrip-180.argb" >/dev/null
cmp -s "$INPUT" "$WORKDIR/roundtrip-180.argb" ||
	fail "180+180 degree round trip differs from input"
echo "PASS: ARGB8888 180+180 degree round trip"

run_transform "$INPUT" "$WIDTH" "$HEIGHT" 0 \
	"$WORKDIR/hflip.argb" 1 0 >/dev/null
run_transform "$WORKDIR/hflip.argb" "$WIDTH" "$HEIGHT" 0 \
	"$WORKDIR/hflip-roundtrip.argb" 1 0 >/dev/null
cmp -s "$INPUT" "$WORKDIR/hflip-roundtrip.argb" ||
	fail "horizontal flip round trip differs from input"
echo "PASS: ARGB8888 horizontal flip round trip"

run_transform "$INPUT" "$WIDTH" "$HEIGHT" 0 \
	"$WORKDIR/vflip.argb" 0 1 >/dev/null
run_transform "$WORKDIR/vflip.argb" "$WIDTH" "$HEIGHT" 0 \
	"$WORKDIR/vflip-roundtrip.argb" 0 1 >/dev/null
cmp -s "$INPUT" "$WORKDIR/vflip-roundtrip.argb" ||
	fail "vertical flip round trip differs from input"
echo "PASS: ARGB8888 vertical flip round trip"

SCALE_WIDTH=$((WIDTH / 2))
SCALE_HEIGHT=$((HEIGHT / 2))
[ "$SCALE_WIDTH" -ge 8 ] || SCALE_WIDTH=8
[ "$SCALE_HEIGHT" -ge 8 ] || SCALE_HEIGHT=8
SOLID=$WORKDIR/solid.argb
SCALED=$WORKDIR/scaled.argb
EXPECTED=$WORKDIR/scaled-expected.argb
dd if=/dev/zero bs=$((WIDTH * HEIGHT * 4)) count=1 2>/dev/null |
	tr '\000' '\377' > "$SOLID"
dd if=/dev/zero bs=$((SCALE_WIDTH * SCALE_HEIGHT * 4)) count=1 2>/dev/null |
	tr '\000' '\377' > "$EXPECTED"
run_mixer "$SOLID" "$WIDTH" "$HEIGHT" AR24 "$SCALED" \
	"$SCALE_WIDTH" "$SCALE_HEIGHT" AR24
[ "$(wc -c < "$SCALED")" -eq $((SCALE_WIDTH * SCALE_HEIGHT * 4)) ] ||
	fail "scaling produced an unexpected output size"
cmp -s "$EXPECTED" "$SCALED" ||
	fail "solid-color scaling changed pixel data"
echo "PASS: ARGB8888 scaling ${WIDTH}x${HEIGHT} -> ${SCALE_WIDTH}x${SCALE_HEIGHT}"

ABGR=$WORKDIR/converted.abgr
ARGB_ROUNDTRIP=$WORKDIR/converted-roundtrip.argb
OPAQUE=$WORKDIR/opaque.argb
pixel=0
while [ "$pixel" -lt $((WIDTH * HEIGHT)) ]; do
	printf '\021\042\063\377'
	pixel=$((pixel + 1))
done > "$OPAQUE"
run_mixer "$OPAQUE" "$WIDTH" "$HEIGHT" AR24 "$ABGR" \
	"$WIDTH" "$HEIGHT" AB24
cmp -s "$OPAQUE" "$ABGR" &&
	fail "ARGB8888 to ABGR8888 conversion did not change channel order"
run_mixer "$ABGR" "$WIDTH" "$HEIGHT" AB24 "$ARGB_ROUNDTRIP" \
	"$WIDTH" "$HEIGHT" AR24
cmp -s "$OPAQUE" "$ARGB_ROUNDTRIP" ||
	fail "ARGB8888/ABGR8888 round trip differs from input"
echo "PASS: ARGB8888 <-> ABGR8888 format conversion round trip"

NV12_SIZE=$((WIDTH * HEIGHT * 3 / 2))
NV12_SCALE_SIZE=$((SCALE_WIDTH * SCALE_HEIGHT * 3 / 2))
NV12_GRAY=$WORKDIR/gray.nv12
NV12_GRAY_SCALED=$WORKDIR/gray-scaled.nv12
NV12_GRAY_EXPECTED=$WORKDIR/gray-scaled-expected.nv12
dd if=/dev/zero bs="$NV12_SIZE" count=1 2>/dev/null |
	tr '\000' '\200' > "$NV12_GRAY"
dd if=/dev/zero bs="$NV12_SCALE_SIZE" count=1 2>/dev/null |
	tr '\000' '\200' > "$NV12_GRAY_EXPECTED"
run_mixer "$NV12_GRAY" "$WIDTH" "$HEIGHT" NV12 "$NV12_GRAY_SCALED" \
	"$SCALE_WIDTH" "$SCALE_HEIGHT" NV12
[ "$(wc -c < "$NV12_GRAY_SCALED")" -eq "$NV12_SCALE_SIZE" ] ||
	fail "NV12 scaling produced an unexpected output size"
cmp -s "$NV12_GRAY_EXPECTED" "$NV12_GRAY_SCALED" ||
	fail "neutral NV12 scaling changed pixel data"
echo "PASS: NV12 scaling ${WIDTH}x${HEIGHT} -> ${SCALE_WIDTH}x${SCALE_HEIGHT}"

WHITE_NV12=$WORKDIR/white.nv12
WHITE_NV12_EXPECTED=$WORKDIR/white-expected.nv12
WHITE_ARGB_ROUNDTRIP=$WORKDIR/white-roundtrip.argb
WHITE_ARGB_EXPECTED=$WORKDIR/white-roundtrip-expected.argb
dd if=/dev/zero bs=$((WIDTH * HEIGHT)) count=1 2>/dev/null |
	tr '\000' '\352' > "$WHITE_NV12_EXPECTED"
dd if=/dev/zero bs=$((WIDTH * HEIGHT / 2)) count=1 2>/dev/null |
	tr '\000' '\200' >> "$WHITE_NV12_EXPECTED"
run_mixer "$SOLID" "$WIDTH" "$HEIGHT" AR24 "$WHITE_NV12" \
	"$WIDTH" "$HEIGHT" NV12
[ "$(wc -c < "$WHITE_NV12")" -eq "$NV12_SIZE" ] ||
	fail "ARGB8888 to NV12 produced an unexpected output size"
cmp -s "$WHITE_NV12_EXPECTED" "$WHITE_NV12" ||
	fail "BT.601 full-range white did not convert to limited-range NV12"
echo "PASS: ARGB8888 full range -> NV12 limited range"

run_mixer "$WHITE_NV12" "$WIDTH" "$HEIGHT" NV12 \
	"$WHITE_ARGB_ROUNDTRIP" "$WIDTH" "$HEIGHT" AR24
pixel=0
while [ "$pixel" -lt $((WIDTH * HEIGHT)) ]; do
	printf '\375\375\375\377'
	pixel=$((pixel + 1))
done > "$WHITE_ARGB_EXPECTED"
cmp -s "$WHITE_ARGB_EXPECTED" "$WHITE_ARGB_ROUNDTRIP" ||
	fail "NV12 limited-range white did not convert to full-range ARGB8888"
echo "PASS: NV12 limited range -> ARGB8888 full range"

echo "PASS: sun60i-g2d functional smoke test"
