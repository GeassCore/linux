#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
kernel_dir=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
module=sun60i-ce.ko
afalg_test=sun60i-ce-afalg-test
dut=${DUT:-root@192.168.10.137}
dut_dir=${DUT_DIR:-/root}

usage()
{
	cat <<EOF
Usage: $0 build [kernel-output]
       $0 build-test [compiler]
       $0 deploy [user@host]
       $0 reload | unload | status | test | stress [rounds]

Environment for build: ARCH, CROSS_COMPILE, JOBS
Environment for deploy: DUT (default $dut), DUT_DIR (default $dut_dir)
EOF
}

build_test()
{
	cc=${1:-${CC:-aarch64-linux-gnu-gcc}}

	"$cc" -O2 -Wall -Wextra -Werror \
		-o "$script_dir/$afalg_test" "$script_dir/$afalg_test.c"
	test -x "$script_dir/$afalg_test"
	sha256sum "$script_dir/$afalg_test"
}

build_module()
{
	out=${1:-$kernel_dir}
	config=$out/.config
	jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}
	arch=${ARCH:-arm64}
	cross_compile=${CROSS_COMPILE:-aarch64-linux-gnu-}

	if [ ! -f "$config" ]; then
		echo "missing kernel configuration: $config" >&2
		exit 1
	fi

	"$kernel_dir/scripts/config" --file "$config" \
		-e ARCH_SUNXI \
		-e CRYPTO \
		-e CRYPTO_ECB \
		-e CRYPTO_CBC \
		-e CRYPTO_AES \
		-e CRYPTO_DES \
		-e CRYPTO_MD5 \
		-e CRYPTO_SHA1 \
		-e CRYPTO_SHA256 \
		-e CRYPTO_SHA512 \
		-e CRYPTO_HW \
		-e CRYPTO_DEV_ALLWINNER \
		-m CRYPTO_DEV_SUN60I_CE \
		-e CRYPTO_DEV_SUN60I_CE_HASH \
		-e CRYPTO_DEV_SUN60I_CE_PRNG \
		-e CRYPTO_DEV_SUN60I_CE_TRNG

	make_args="O=$out ARCH=$arch CROSS_COMPILE=$cross_compile"

	# shellcheck disable=SC2086
	make -C "$kernel_dir" $make_args olddefconfig
	# Keep generated configuration headers in sync for an isolated M= build.
	# shellcheck disable=SC2086
	make -C "$kernel_dir" $make_args prepare modules_prepare
	# shellcheck disable=SC2086
	make -C "$kernel_dir" $make_args -j"$jobs" \
		M=drivers/crypto/allwinner/sun60i-ce modules

	test -s "$script_dir/$module"
	sha256sum "$script_dir/$module"
}

deploy_module()
{
	target=${1:-$dut}
	test -s "$script_dir/$module"
	set -- "$script_dir/$module" "$script_dir/$(basename -- "$0")"
	if [ -x "$script_dir/$afalg_test" ]; then
		set -- "$@" "$script_dir/$afalg_test"
	fi
	scp "$@" "$target:$dut_dir/"
}

unload_module()
{
	if grep -q '^sun60i_ce ' /proc/modules; then
		rmmod sun60i_ce
	fi
}

load_module()
{
	unload_module
	if command -v modprobe >/dev/null 2>&1; then
		modprobe crypto_engine
	fi
	insmod "${MODULE_PATH:-./$module}"
}

status_module()
{
	grep '^sun60i_ce ' /proc/modules || true
	awk 'BEGIN { RS=""; ORS="\n\n" } \
		/driver[[:space:]]*: .*sun60i-ce/ { print }' /proc/crypto
	test -d /sys/bus/platform/drivers/sun60i-ce
}

check_crypto_driver()
{
	driver=$1

	awk -v driver="$driver" '
		BEGIN { RS="" }
		$0 ~ ("driver[[:space:]]*: " driver) &&
		$0 ~ /selftest[[:space:]]*: passed/ { found=1 }
		END { exit !found }
	' /proc/crypto || {
		echo "missing or failed hardware driver: $driver" >&2
		exit 1
	}
}

test_module()
{
	irq_before=$(awk '/sun60i-ce/ { for (i = 2; i < NF; i++) sum += $i }
		END { print sum + 0 }' /proc/interrupts)

	status_module

	for driver in \
		cbc-aes-sun60i-ce ecb-aes-sun60i-ce \
		cbc-des3-sun60i-ce ecb-des3-sun60i-ce \
		md5-sun60i-ce sha1-sun60i-ce sha224-sun60i-ce \
		sha256-sun60i-ce sha384-sun60i-ce sha512-sun60i-ce \
		sun60i-ce-prng; do
		check_crypto_driver "$driver"
	done

	if [ ! -x "${AFALG_TEST_PATH:-./$afalg_test}" ]; then
		echo "missing AF_ALG test binary: ${AFALG_TEST_PATH:-./$afalg_test}" >&2
		exit 1
	fi
	"${AFALG_TEST_PATH:-./$afalg_test}"

	irq_after=$(awk '/sun60i-ce/ { for (i = 2; i < NF; i++) sum += $i }
		END { print sum + 0 }' /proc/interrupts)
	irq_delta=$((irq_after - irq_before))
	if [ "$irq_delta" -lt 10 ]; then
		echo "CE interrupt count increased by $irq_delta, expected at least 10" >&2
		exit 1
	fi
	echo "CE interrupts: $irq_before -> $irq_after (+$irq_delta)"

	if [ -c /dev/hwrng ]; then
		rng_a=$(dd if=/dev/hwrng bs=32 count=32 2>/dev/null | cksum)
		rng_b=$(dd if=/dev/hwrng bs=32 count=32 2>/dev/null | cksum)
		[ "$rng_a" != "$rng_b" ] || {
			echo "hwrng returned identical samples" >&2
			exit 1
		}
		echo "hwrng samples: $rng_a / $rng_b"
	else
		echo "warning: /dev/hwrng is unavailable" >&2
	fi


	if dmesg | tail -n 200 | grep -Ei \
		'sun60i-ce.*(error|failed|failure|timeout)'; then
		echo "sun60i-ce reported an error" >&2
		exit 1
	fi

	dmesg | tail -n 120
}

stress_module()
{
	rounds=${1:-100}
	case $rounds in
	''|*[!0-9]*)
		echo "invalid stress round count: $rounds" >&2
		exit 2
		;;
	esac
	if [ "$rounds" -eq 0 ]; then
		echo "stress round count must be greater than zero" >&2
		exit 2
	fi
	if [ ! -x "${AFALG_TEST_PATH:-./$afalg_test}" ]; then
		echo "missing AF_ALG test binary: ${AFALG_TEST_PATH:-./$afalg_test}" >&2
		exit 1
	fi

	irq_before=$(awk '/sun60i-ce/ { for (i = 2; i < NF; i++) sum += $i }
		END { print sum + 0 }' /proc/interrupts)
	i=0
	while [ "$i" -lt "$rounds" ]; do
		"${AFALG_TEST_PATH:-./$afalg_test}" >/dev/null
		i=$((i + 1))
	done
	irq_after=$(awk '/sun60i-ce/ { for (i = 2; i < NF; i++) sum += $i }
		END { print sum + 0 }' /proc/interrupts)
	echo "stress rounds: $rounds"
	echo "CE interrupts: $irq_before -> $irq_after (+$((irq_after - irq_before)))"

	if dmesg | tail -n 300 | grep -Ei \
		'sun60i-ce.*(error|failed|failure|timeout)'; then
		echo "sun60i-ce reported an error during stress test" >&2
		exit 1
	fi
}

case ${1:-} in
build)
	build_module "${2:-}"
	;;
build-test)
	build_test "${2:-}"
	;;
deploy)
	deploy_module "${2:-}"
	;;
reload)
	load_module
	status_module
	;;
unload)
	unload_module
	;;
status)
	status_module
	;;
test)
	test_module
	;;
stress)
	stress_module "${2:-100}"
	;;
*)
	usage >&2
	exit 2
	;;
esac
