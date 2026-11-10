#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 在设备上编译并安装 KWin 环境色温效果。
# 用法：sudo ./install.sh
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限安装到 /usr/lib/qt6/plugins/kwin/effects/plugins" >&2
    exit 1
fi

for tool in cmake ninja; do
    command -v "$tool" >/dev/null || {
        echo "缺少 $tool，请先安装" >&2
        exit 1
    }
done

build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

cmake -S "$script_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir"
cmake --install "$build_dir"

echo "安装完成：/usr/lib/qt6/plugins/kwin/effects/plugins/ambientcolor.so"
echo "启用方式：系统设置 → 工作区行为 → 桌面特效 → 外观，勾选「环境色温」"
