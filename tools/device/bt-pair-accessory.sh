#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# 把蓝牙附件从零配对到本机控制器。
#
# 用法：bt-pair-accessory.sh <蓝牙地址>
#
# 键盘吸附后持续广播，可随时配对。笔只在重新吸附后的数秒内发送可连接
# ADV_IND，之后转为不可连接广播，因此对目标地址持续重试 pair，避免轮询
# 设备列表带来的延迟。NoInputNoOutput agent 自动确认 Just Works。
#
# 前置条件：bluetoothd 使用的配置文件（默认 /etc/bluetooth/loa-main.conf）
# 在 [General] 段设置 FilterDiscoverable = false。BlueZ 5.87 默认 true，
# 会丢弃缺少 General 或 Limited Discoverable 标志的广播，M-Pencil 3 属于
# 这种情况。

set -eu

if [ $# -ne 1 ]; then
    echo "用法: $0 <蓝牙地址>" >&2
    exit 1
fi

target=$1
retries=${RETRIES:-180}

config=/etc/bluetooth/loa-main.conf
[ -f "$config" ] || config=/etc/bluetooth/main.conf
if ! grep -qiE '^[[:space:]]*FilterDiscoverable[[:space:]]*=[[:space:]]*false' \
    "$config"; then
    echo "请先在 $config 的 [General] 段设置 FilterDiscoverable = false" >&2
    exit 1
fi

# bluetoothd 可能已经完成上电，此时 btmgmt 返回 Busy，继续执行即可。
btmgmt le on >/dev/null 2>&1 || true
btmgmt bondable on >/dev/null 2>&1 || true

{
    printf 'agent NoInputNoOutput\n'
    printf 'default-agent\n'
    printf 'power on\n'
    printf 'scan on\n'

    i=0
    while [ "$i" -lt "$retries" ]; do
        printf 'pair %s\n' "$target"
        i=$((i + 1))
        sleep 1
    done

    printf 'trust %s\n' "$target"
    printf 'connect %s\n' "$target"
    sleep 5
    printf 'info %s\n' "$target"
    printf 'quit\n'
} | timeout $((retries + 40)) bluetoothctl
