#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 构建 U-Boot 2026.10 的 DBY-W09 版本。
#
# 用法：scripts/build-uboot.sh [--clean]
#   --clean  删除 build/u-boot-2026.10 后重新解压、打补丁并编译
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
version=2026.10
src="$root/build/u-boot-$version"
out="$root/out"
cross=${CROSS_COMPILE:-aarch64-linux-gnu-}
jobs=${JOBS:-$(nproc)}

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$src"
    # 从 build/ 中缓存的源码包重新解压；缺少源码包时 fetch-sources.sh
    # 会下载并校验，避免删除后直接报“缺少源码”。
    "$root/scripts/fetch-sources.sh"
fi

if [ ! -d "$src" ]; then
    echo "缺少 U-Boot 源码，请先执行 scripts/fetch-sources.sh" >&2
    exit 1
fi

if [ ! -f "$src/.loa-patched" ]; then
    echo "== 应用 U-Boot 补丁"
    for p in "$root"/uboot/patches/*.patch; do
        echo "   $(basename "$p")"
        patch -d "$src" -p1 <"$p"
    done

    install -m644 "$root/uboot/dby-w09_defconfig" "$src/configs/dby-w09_defconfig"
    touch "$src/.loa-patched"
else
    echo "== 补丁已应用，跳过"
fi

echo "== 配置 U-Boot"
make -C "$src" CROSS_COMPILE="$cross" dby-w09_defconfig

echo "== 编译 U-Boot"
make -C "$src" CROSS_COMPILE="$cross" -j"$jobs"

mkdir -p "$out"
install -m644 "$src/u-boot.bin" "$out/u-boot.bin"
echo "输出：$out/u-boot.bin"
