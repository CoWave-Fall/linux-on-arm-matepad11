#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

status=/sys/bus/i2c/devices/3-002b/tx_status

# 吸附事件与开机自检共用此脚本：霍尔有效表示附件仍在位，充电完成后
# 发射端会停止供电，此时蓝牙数据通道仍需保持可用。
if ! grep -qE '^hall_(keyboard|pen): 1$' "$status" 2>/dev/null; then
    exit 0
fi

# 开机阶段 hci0 由 qca-bt-rebind 重新绑定后才出现，BlueZ 注册控制器
# 还需要一段时间，以 D-Bus 查询结果为准等待就绪。
i=0
while ! bluetoothctl list 2>/dev/null | grep -q "Controller"; do
    i=$((i + 1))
    if [ "$i" -ge 60 ]; then
        exit 0
    fi
    sleep 0.5
done

/usr/bin/rfkill unblock bluetooth
/usr/bin/bluetoothctl power on
