#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 把内核与设备树打包为 U-Boot FIT 镜像。
#
# 用法：scripts/pack-fit.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/out"
work="$root/build/fit"
its="$root/tools/fit/dby-w09-sd.its"

[ -f "$out/Image.gz" ] || {
    echo "缺少 $out/Image.gz，请先执行 scripts/build-kernel.sh" >&2
    exit 1
}
[ -f "$out/dby-w09.dtb" ] || {
    echo "缺少 $out/dby-w09.dtb，请先执行 scripts/build-kernel.sh" >&2
    exit 1
}

mkimage=${MKIMAGE:-}
if [ -z "$mkimage" ]; then
    mkimage=$(command -v mkimage || true)
fi
if [ -z "$mkimage" ] && [ -x "$root/build/u-boot-2026.10/tools/mkimage" ]; then
    mkimage="$root/build/u-boot-2026.10/tools/mkimage"
fi
if [ -z "$mkimage" ]; then
    echo "找不到 mkimage，请安装 uboot-tools 或先构建 U-Boot" >&2
    exit 1
fi

rm -rf "$work"
mkdir -p "$work"
cp "$its" "$work/dby-w09-sd.its"
cp "$out/Image.gz" "$work/Image.gz"
cp "$out/dby-w09.dtb" "$work/dby-w09.dtb"

(
    cd "$work"
    "$mkimage" -f dby-w09-sd.its "$out/dby-w09.itb"
)

echo "输出：$out/dby-w09.itb"
