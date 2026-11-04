#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 构建 Linux 7.3.1 的 DBY-W09 内核与设备树。
#
# 用法：scripts/build-kernel.sh [--clean]
#   --clean  删除 build/linux-7.3.1 后重新解压、打补丁并编译
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
version=7.3.1
src="$root/build/linux-$version"
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
    echo "缺少 Linux 源码，请先执行 scripts/fetch-sources.sh" >&2
    exit 1
fi

required_firmware=(
    novatek/NT36523_THP1300.bin
    ath11k/QCA6390/hw2.0/amss.bin
    ath11k/QCA6390/hw2.0/m3.bin
    ath11k/QCA6390/hw2.0/board.bin
    ath11k/QCA6390/hw2.0/board-2.bin
)

missing=0
for f in "${required_firmware[@]}"; do
    if [ ! -f "$root/kernel/firmware/$f" ]; then
        echo "缺少固件：kernel/firmware/$f" >&2
        missing=1
    fi
done
if [ "$missing" -ne 0 ]; then
    echo "固件来源见 docs/firmware.md" >&2
    exit 1
fi

if [ ! -f "$src/.loa-patched" ]; then
    echo "== 应用内核补丁"
    for p in "$root"/kernel/patches/*.patch; do
        echo "   $(basename "$p")"
        patch -d "$src" -p1 <"$p"
    done

    install -m644 "$root/kernel/defconfig" \
        "$src/arch/arm64/configs/dby-w09_defconfig"

    for f in "${required_firmware[@]}"; do
        install -Dm644 "$root/kernel/firmware/$f" "$src/firmware/$f"
    done
    touch "$src/.loa-patched"
else
    echo "== 补丁已应用，跳过"
fi

echo "== 配置内核"
make -C "$src" ARCH=arm64 CROSS_COMPILE="$cross" dby-w09_defconfig

echo "== 编译内核与设备树"
make -C "$src" ARCH=arm64 CROSS_COMPILE="$cross" -j"$jobs" Image dtbs

mkdir -p "$out"
gzip -9 -c "$src/arch/arm64/boot/Image" >"$out/Image.gz"
install -m644 "$src/arch/arm64/boot/dts/qcom/sm8250-huawei-dby-w09.dtb" \
    "$out/dby-w09.dtb"
echo "输出："
echo "  $out/Image.gz"
echo "  $out/dby-w09.dtb"
