#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 从主机推送 tools/device/ppd-qcom 与驱动源码到设备并执行安装。
# 用法：tools/host/ppd-qcom-deploy.sh [root@10.0.0.2]
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$script_dir/../.." && pwd)
device=${1:-root@10.0.0.2}
remote_dir=/root/ppd-qcom-install
staging="$root/.tmp/ppd-qcom-deploy"

# 驱动源码以 tools/upstream 为权威版本，随部署一起送到设备。
mkdir -p "$staging"
cp -a "$root/tools/device/ppd-qcom/." "$staging/"
install -m644 \
    "$root/tools/upstream/power-profiles-daemon/ppd-driver-qcom.c" \
    "$root/tools/upstream/power-profiles-daemon/ppd-driver-qcom.h" \
    "$root/tools/upstream/power-profiles-daemon/integration.patch" \
    "$staging/"

tar -C "$staging" -cf - . |
    ssh "$device" "mkdir -p $remote_dir && tar -C $remote_dir -xf -"
ssh "$device" "$remote_dir/install.sh"
