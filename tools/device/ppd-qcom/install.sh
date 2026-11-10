#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 安装 power-profiles-daemon 的 qcom 驱动版本、systemd drop-in 与 pacman hook。
# 在设备上以 root 执行；重复执行结果相同。
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
pkg_name=power-profiles-daemon
src_dir=/usr/src/loa-ppd
dest_dir=/usr/local/libexec/loa
dest="$dest_dir/power-profiles-daemon"

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限安装到 /usr/local 与 /etc" >&2
    exit 1
fi

# 源码树只在首次部署时初始化。已有源码树时保留，方便按包版本手动同步；
# 驱动文件随后按项目仓库版本补齐，避免旧源码树覆盖新版本。
if [ ! -f "$src_dir/meson.build" ]; then
    if [ ! -f /root/ppd-src/meson.build ]; then
        echo "缺少源码树，请先准备 $src_dir 或 /root/ppd-src" >&2
        exit 1
    fi
    mkdir -p "$src_dir"
    tar -C /root/ppd-src --exclude='./build' -cf - . | tar -C "$src_dir" -xf -
    chown -R root:root "$src_dir"
fi

# 驱动与注册补丁以项目仓库为准，覆盖设备源码树中的旧版本。
if [ -f "$script_dir/ppd-driver-qcom.c" ]; then
    install -m644 "$script_dir/ppd-driver-qcom.c" "$src_dir/src/ppd-driver-qcom.c"
    install -m644 "$script_dir/ppd-driver-qcom.h" "$src_dir/src/ppd-driver-qcom.h"
fi
if [ -f "$script_dir/integration.patch" ] &&
    ! grep -q 'ppd-driver-qcom' "$src_dir/src/meson.build"; then
    patch --dry-run -p1 -d "$src_dir" <"$script_dir/integration.patch" >/dev/null
    patch -p1 -d "$src_dir" <"$script_dir/integration.patch"
fi

install -Dm755 "$script_dir/rebuild.sh" "$dest_dir/ppd-qcom-rebuild.sh"

# 首次编译在安装 hook 之前完成，避免 pacman 重装发行包时重复触发。
PPD_SRC_DIR="$src_dir" PPD_SKIP_RESTART=1 "$dest_dir/ppd-qcom-rebuild.sh"

if ! strings "$dest" | grep 'PpdDriverQcom' >/dev/null; then
    echo "重建产物中没有 qcom 驱动，终止部署" >&2
    exit 1
fi

# 把 /usr/lib 下的守护进程恢复成发行包原件，后续由 drop-in 指向 /usr/local。
if ! pacman -Qkkk "$pkg_name" >/dev/null 2>&1; then
    echo "恢复发行包中的 /usr/lib/power-profiles-daemon"
    pacman -S --noconfirm "$pkg_name"
fi

install -Dm644 "$script_dir/power-profiles-daemon.service.d/override.conf" \
    /etc/systemd/system/power-profiles-daemon.service.d/override.conf
install -Dm644 "$script_dir/50-ppd-qcom.hook" \
    /etc/pacman.d/hooks/50-ppd-qcom.hook

systemctl daemon-reload
systemctl restart power-profiles-daemon.service

# 服务通过 D-Bus 激活后才会出现在 powerprofilesctl 的输出里。
for _ in {1..10}; do
    if powerprofilesctl list 2>/dev/null | grep 'CpuDriver:.*qcom' >/dev/null; then
        break
    fi
    sleep 1
done

if ! powerprofilesctl list | grep 'CpuDriver:.*qcom' >/dev/null; then
    echo "服务未报告 qcom 驱动，当前输出：" >&2
    powerprofilesctl list >&2 || true
    exit 1
fi

exec_start=$(systemctl show power-profiles-daemon.service -p ExecStart --value)
case "$exec_start" in
*"/usr/local/libexec/loa/power-profiles-daemon"*) ;;
*)
    echo "systemd 未使用 /usr/local 下的二进制：$exec_start" >&2
    exit 1
    ;;
esac

if ! pacman -Qkkk "$pkg_name" >/dev/null 2>&1; then
    echo "发行包文件仍有改动，请检查 pacman -Qkkk $pkg_name 的输出" >&2
    exit 1
fi

echo "安装完成：$dest"
echo "ExecStart：$exec_start"
powerprofilesctl list
