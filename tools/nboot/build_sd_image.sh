#!/bin/sh
# SPDX-License-Identifier: GPL-2.0+
# Build a KICKPI-K7 N-Boot recovery and A/B system card.
# Copyright 2026 Pharos Tech

set -eu

die()
{
	echo "build_sd_image: $*" >&2
	exit 1
}

[ "$#" -eq 5 ] || die \
	"usage: $0 IDBLOADER NBOOT_FIT TRUST NUTTX_BIN OUTPUT"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
idbloader=$1
nboot=$2
trust=$3
nuttx=$4
output=$5

for file in "$idbloader" "$nboot" "$trust" "$nuttx"; do
	[ -r "$file" ] || die "input is not readable: $file"
done

[ "$(stat -c %s "$idbloader")" -le $(((16384 - 64) * 512)) ] ||
	die "IDBlock exceeds its reserved region"
[ "$(stat -c %s "$nboot")" -le $((4 * 1024 * 1024)) ] ||
	die "N-Boot FIT exceeds 4 MiB"
[ "$(stat -c %s "$trust")" -le $((4 * 1024 * 1024)) ] ||
	die "trust image exceeds 4 MiB"
[ "$(stat -c %s "$nuttx")" -le $((64 * 1024 * 1024)) ] ||
	die "NuttX image exceeds 64 MiB"

set -- $(od -An -N4 -tx1 "$nboot")
[ "$1$2$3$4" = "d00dfeed" ] || die "N-Boot payload has no FIT magic"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
python3 "$script_dir/bootctrl.py" init \
	--output "$work/bootctrl.bin" --nuttx-a "$nuttx" --nuttx-b "$nuttx"
python3 "$script_dir/bootctrl.py" inspect "$work/bootctrl.bin" >/dev/null

mkdir -p "$(dirname -- "$output")"
truncate -s 4G "$output"
data_end=$((4 * 1024 * 1024 * 1024 / 512 - 2049))
sgdisk -og "$output" >/dev/null
sgdisk -U 4b374142-0000-4000-8000-000000000002 "$output" >/dev/null
sgdisk -n 1:16384:24575 -c 1:uboot -t 1:8300 "$output" >/dev/null
sgdisk -n 2:24576:32767 -c 2:trust -t 2:8300 "$output" >/dev/null
sgdisk -n 3:32768:34815 -c 3:bootctrl -t 3:8300 "$output" >/dev/null
sgdisk -n 4:36864:167935 -c 4:nuttx_a -t 4:8300 "$output" >/dev/null
sgdisk -n 5:167936:299007 -c 5:nuttx_b -t 5:8300 "$output" >/dev/null
sgdisk -n 6:299008:1347583 -c 6:amp_a -t 6:8300 "$output" >/dev/null
sgdisk -n 7:1347584:2396159 -c 7:amp_b -t 7:8300 "$output" >/dev/null
sgdisk -n 8:2396160:"$data_end" -c 8:data -t 8:8300 "$output" >/dev/null

dd if="$idbloader" of="$output" bs=512 seek=64 conv=notrunc status=none
if [ "$(stat -c %s "$idbloader")" -le $((1024 * 512)) ]; then
	dd if="$idbloader" of="$output" bs=512 seek=1088 \
		conv=notrunc status=none
fi
dd if="$nboot" of="$output" bs=512 seek=16384 conv=notrunc status=none
dd if="$trust" of="$output" bs=512 seek=24576 conv=notrunc status=none
dd if="$work/bootctrl.bin" of="$output" bs=512 seek=32768 \
	conv=notrunc status=none
dd if="$nuttx" of="$output" bs=512 seek=36864 conv=notrunc status=none
dd if="$nuttx" of="$output" bs=512 seek=167936 conv=notrunc status=none

sgdisk -v "$output"
echo "OK: $output"
