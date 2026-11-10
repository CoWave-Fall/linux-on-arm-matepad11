#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 交叉编译 DBY-W09 电源键守护进程，并推送二进制、服务单元与 logind drop-in。
# 用法：tools/host/powerkey-deploy.sh [root@10.0.0.2]
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$script_dir/../.." && pwd)
device=${1:-root@10.0.0.2}
staging="$root/.tmp/powerkey-deploy"

mkdir -p "$staging"
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -static \
    -o "$staging/dby-powerkeyd" "$root/tools/device/dby-powerkeyd.c"

# 守护进程正在运行时不能直接覆盖二进制（ETXTBSY），先传临时文件再重命名。
scp -q "$staging/dby-powerkeyd" "$device:/usr/local/sbin/dby-powerkeyd.new"
ssh "$device" 'chmod 755 /usr/local/sbin/dby-powerkeyd.new &&
	mv /usr/local/sbin/dby-powerkeyd.new /usr/local/sbin/dby-powerkeyd'
scp -q "$root/rootfs/etc/systemd/system/dby-powerkey.service" \
    "$device:/etc/systemd/system/dby-powerkey.service"

# logind 的 drop-in 只在内容变化时提示重启系统。运行中的图形会话依赖
# logind 的设备租约，重启 logind 会让 KWin 丢失 DRM master，因此不在
# 部署脚本里自动重启。
logind_conf="$root/rootfs/etc/systemd/logind.conf.d/10-dby-powerkey-logind.conf"
old_hash=$(ssh "$device" \
    'sha256sum /etc/systemd/logind.conf.d/10-dby-powerkey-logind.conf 2>/dev/null |
	 cut -d" " -f1')
ssh "$device" 'install -d /etc/systemd/logind.conf.d'
scp -q "$logind_conf" \
    "$device:/etc/systemd/logind.conf.d/10-dby-powerkey-logind.conf"
new_hash=$(sha256sum "$logind_conf" | cut -d" " -f1)
if [ "$old_hash" != "$new_hash" ]; then
    echo "logind drop-in 已更新，重启系统后生效" >&2
fi

ssh "$device" 'chmod 755 /usr/local/sbin/dby-powerkeyd &&
	systemctl daemon-reload &&
	systemctl enable dby-powerkey &&
	systemctl restart dby-powerkey'
