#!/bin/bash
# 从 super 分区的 odm 逻辑分区提取设备自带的音频配置到 /etc/loa-smartpa。
set -euo pipefail

SRC=${1:-/dev/disk/by-partlabel/super}
DEST=/etc/loa-smartpa
# odm 镜像约 646 MiB，/tmp 是 tmpfs，放不下，改用磁盘上的临时目录
TMP=$(mktemp -d /var/tmp/odm-audio.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

lpunpack -p odm "$SRC" "$TMP"
IMG=$(ls "$TMP"/odm*.img | head -n 1)
fsck.erofs --extract="$TMP/root" "$IMG"
install -d -m 0755 "$DEST"
install -m 0644 "$TMP"/root/etc/audio/nxp/tfa9874_0_configs.xml \
	"$TMP"/root/etc/audio/nxp/tfa9874_1_configs.xml \
	"$TMP"/root/etc/audio/nxp/tfa9874_2_configs.xml \
	"$TMP"/root/etc/audio/nxp/tfa9874_3_configs.xml "$DEST/"
echo "已提取到 $DEST"
