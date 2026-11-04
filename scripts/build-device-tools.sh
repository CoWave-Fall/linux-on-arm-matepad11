#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 交叉编译设备侧的原生程序。Python、QML 与 KCM 在设备上安装或编译。
#
# 用法：scripts/build-device-tools.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/out/device"
cc=${CC:-aarch64-linux-gnu-gcc}

mkdir -p "$out"

echo "== dby-powerkeyd"
"$cc" -O2 -Wall -Wextra -static \
    -o "$out/dby-powerkeyd" "$root/tools/device/dby-powerkeyd.c"

echo "== loa-pen"
"$cc" -O2 -Wall -Wextra -static \
    -o "$out/loa-pen" "$root/tools/device/loa-pen.c"

echo "== vibra-test"
"$cc" -O2 -Wall -Wextra -static \
    -o "$out/vibra-test" "$root/tools/device/vibra-test.c"

ls -lh "$out"
