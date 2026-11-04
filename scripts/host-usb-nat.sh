#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 为设备的 USB ECM 链路提供互联网访问，用于首次安装软件包。
#
# 用法：sudo scripts/host-usb-nat.sh up|down
#   UPLINK=<接口> 可覆盖自动检测的上行接口
set -euo pipefail

iface=loa-usb0
network=10.0.0.0/24

action=${1:-}
[ "$action" = up ] || [ "$action" = down ] || {
    echo "用法：sudo $0 up|down" >&2
    exit 2
}

[ "$(id -u)" -eq 0 ] || {
    echo "需要 root 权限修改转发规则" >&2
    exit 1
}

uplink=${UPLINK:-$(ip -4 route show default | awk '$1 == "default" {print $5; exit}')}
[ -n "$uplink" ] || {
    echo "找不到默认路由，请设置 UPLINK" >&2
    exit 1
}

ipt() {
    iptables -w "$@"
}

if [ "$action" = up ]; then
    sysctl -q -w net.ipv4.ip_forward=1
    ipt -t nat -C POSTROUTING -s "$network" -o "$uplink" -j MASQUERADE 2>/dev/null ||
        ipt -t nat -A POSTROUTING -s "$network" -o "$uplink" -j MASQUERADE
    ipt -C FORWARD -i "$iface" -o "$uplink" -j ACCEPT 2>/dev/null ||
        ipt -A FORWARD -i "$iface" -o "$uplink" -j ACCEPT
    ipt -C FORWARD -i "$uplink" -o "$iface" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null ||
        ipt -A FORWARD -i "$uplink" -o "$iface" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
    echo "NAT 已启用：$network -> $uplink"
    echo "设备侧执行：ip route add default via 10.0.0.1; echo 'nameserver 1.1.1.1' > /etc/resolv.conf"
else
    ipt -t nat -D POSTROUTING -s "$network" -o "$uplink" -j MASQUERADE 2>/dev/null || true
    ipt -D FORWARD -i "$iface" -o "$uplink" -j ACCEPT 2>/dev/null || true
    ipt -D FORWARD -i "$uplink" -o "$iface" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
    echo "NAT 规则已移除"
fi
