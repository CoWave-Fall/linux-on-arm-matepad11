#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 从 U-Boot 的 fastboot 启动 FIT 镜像。
#
# U-Boot 的 fastboot 后端只能写 scsi 0 上的分区，主机端 fastboot boot
# 会把非 Android 镜像重新封装，因此改用：写入暂存分区 -> 读回内存 -> bootm。
#
# 暂存分区必须选 HarmonyOS 不挂载、也不参与 AVB 校验的分区。
# reserved3（24 MiB）满足这两个条件，可用 STAGE_PART 覆盖。
#
# 用法：tools/host/boot-linux.sh [fit]
set -euo pipefail

FIT="${1:-out/dby-w09.itb}"
FIT_ADDR=0x98000000
STAGE_PART="${STAGE_PART:-reserved3}"

case "$STAGE_PART" in
cache | log)
    echo "暂存分区 $STAGE_PART 由 HarmonyOS 挂载，拒绝使用" >&2
    exit 1
    ;;
esac

# U-Boot 的 bootm 不读取 FIT 配置里的 bootargs，必须提前写入环境变量。
# root=PARTLABEL=archroot 与 scripts/install-rootfs.sh 创建的分区标号一致。
BOOTARGS="${BOOTARGS:-console=tty0 loglevel=4 panic=10 clk_ignore_unused pd_ignore_unused root=PARTLABEL=archroot rw rootwait rootfstype=f2fs}"

[ -f "$FIT" ] || {
    echo "找不到镜像：$FIT" >&2
    exit 1
}

SIZE=$(stat -c '%s' "$FIT")
BLOCKS=$(((SIZE + 511) / 512))

# reserved3 分区为 24 MiB，超出的镜像会写到相邻分区
if [ "$STAGE_PART" = reserved3 ] && [ "$SIZE" -gt $((24 * 1024 * 1024)) ]; then
    echo "FIT 大小 $SIZE 字节超过 reserved3 的 24 MiB" >&2
    exit 1
fi

echo "== 刷入暂存分区 $STAGE_PART：$FIT（$SIZE 字节，$BLOCKS 块）"
fastboot flash "$STAGE_PART" "$FIT"

echo "== 设置 bootargs"
fastboot oem run:"setenv bootargs '$BOOTARGS'"

echo "== 查询 $STAGE_PART 起始块"
fastboot oem run:part start scsi 0 "$STAGE_PART" cs

echo "== 读入内存 $FIT_ADDR"
# '$cs' 由 U-Boot 环境变量展开，不能在主机侧展开
# shellcheck disable=SC2016
fastboot oem run:scsi read "$FIT_ADDR" '$cs' "$(printf '0x%x' "$BLOCKS")"

echo "== 启动 FIT"
# 内核接管 USB 控制器后 fastboot 会话结束，这里忽略返回码
fastboot oem run:bootm "$FIT_ADDR" || true
