#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 把 U-Boot 二进制封装为 Android recovery 镜像，并生成关闭校验的 vbmeta。
#
# 用法：scripts/pack-recovery.sh [原厂 recovery 镜像]
#   默认读取 dumps/recovery.img。
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/out"
work="$root/build/recovery"
vendor=${1:-$root/dumps/recovery.img}

[ -f "$out/u-boot.bin" ] || {
    echo "缺少 $out/u-boot.bin，请先执行 scripts/build-uboot.sh" >&2
    exit 1
}
[ -f "$vendor" ] || {
    echo "找不到原厂 recovery 镜像：$vendor" >&2
    echo "请传入备份中的 recovery.img" >&2
    exit 1
}

mkbootimg="$root/tools/vendor/mkbootimg/mkbootimg.py"
unpack_bootimg="$root/tools/vendor/mkbootimg/unpack_bootimg.py"
avbtool="$root/tools/vendor/avbtool/avbtool.py"

rm -rf "$work"
mkdir -p "$work/unpacked"
gzip -9 -c "$out/u-boot.bin" >"$work/u-boot.bin.gz"

echo "== 解包原厂 recovery"
python3 "$unpack_bootimg" --boot_img "$vendor" --out "$work/unpacked"

for f in ramdisk dtb recovery_dtbo; do
    [ -f "$work/unpacked/$f" ] || {
        echo "原厂 recovery 镜像缺少 $f" >&2
        exit 1
    }
done

cmdline='earlycon=msm_geni_serial,0xa90000 androidboot.hardware=qcom androidboot.console=ttyMSM0 androidboot.memcg=1 lpm_levels.sleep_disabled=1 video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237 service_locator.enable=1 androidboot.usbcontroller=a600000.dwc3 swiotlb=2048 loop.max_part=7 cgroup.memory=nokmem,nosocket reboot=panic_warm androidboot.selinux=enforce unmovable_isolate1=2:256M,3:312M,4:348M buildvariant=user'

echo "== 封装 recovery 镜像"
PYTHONPATH="$root/tools/vendor/mkbootimg" \
    python3 "$mkbootimg" \
    --kernel "$work/u-boot.bin.gz" \
    --ramdisk "$work/unpacked/ramdisk" \
    --dtb "$work/unpacked/dtb" \
    --recovery_dtbo "$work/unpacked/recovery_dtbo" \
    --pagesize 4096 --header_version 2 --base 0 \
    --kernel_offset 0x00008000 --ramdisk_offset 0x02000000 \
    --dtb_offset 0x01f00000 --tags_offset 0x00000100 \
    --os_version 11.0.0 --os_patch_level 2020-11 \
    --cmdline "$cmdline" \
    --output "$out/recovery.img"

echo "== 生成关闭校验的 vbmeta"
python3 "$avbtool" make_vbmeta_image \
    --flags 2 --padding_size 1048576 \
    --output "$out/recovery_vbmeta.img"

ls -lh "$out/recovery.img" "$out/recovery_vbmeta.img"
