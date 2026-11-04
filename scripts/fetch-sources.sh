#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 下载并解压发布所用的上游源码。
#
# 用法：scripts/fetch-sources.sh [--force]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build"
mkdir -p "$build"

linux_version=7.3.1
uboot_version=2026.10
linux_tar="linux-$linux_version.tar.xz"
uboot_tar="u-boot-$uboot_version.tar.gz"
linux_url="https://cdn.kernel.org/pub/linux/kernel/v7.x/$linux_tar"
uboot_url="https://github.com/u-boot/u-boot/archive/refs/tags/v$uboot_version.tar.gz"

linux_sha256=fa595aad93734f829c963c2a90ef7c3673224fd7fdae7b49406e397cde2de231
uboot_sha256=f943a88389c8fd70a6b0d061a88267465e143c7ca6ea2b75837ab20f5b60c739

force=0
[ "${1:-}" = "--force" ] && force=1

fetch() {
    local url=$1 file=$2 sha=$3
    if [ "$force" = 1 ] || [ ! -f "$build/$file" ]; then
        echo "== 下载 $file"
        curl -fL --retry 3 -o "$build/$file.part" "$url"
        mv "$build/$file.part" "$build/$file"
    fi
    echo "$sha  $build/$file" | sha256sum -c -
}

fetch "$linux_url" "$linux_tar" "$linux_sha256"
fetch "$uboot_url" "$uboot_tar" "$uboot_sha256"

if [ "$force" = 1 ] || [ ! -d "$build/linux-$linux_version" ]; then
    echo "== 解压 Linux $linux_version"
    rm -rf "$build/linux-$linux_version"
    tar -C "$build" -xf "$build/$linux_tar"
fi

if [ "$force" = 1 ] || [ ! -d "$build/u-boot-$uboot_version" ]; then
    echo "== 解压 U-Boot $uboot_version"
    rm -rf "$build/u-boot-$uboot_version"
    tar -C "$build" -xf "$build/$uboot_tar"
fi

echo "源码已就绪："
echo "  $build/linux-$linux_version"
echo "  $build/u-boot-$uboot_version"
