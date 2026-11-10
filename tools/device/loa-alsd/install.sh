#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 在设备上编译并安装环境光色温服务。
# 依赖 tools/upstream/libssc 的 CCT 补丁安装到 /usr/local/loa-libssc。
# 用法：sudo ./install.sh <桌面用户名>
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
user=${1:-${SUDO_USER:-}}
if [ -z "$user" ] || [ "$user" = root ]; then
    echo "用法：sudo ./install.sh <桌面用户名>" >&2
    exit 2
fi
uid=$(id -u "$user")
group=$(id -gn "$user")
home=$(getent passwd "$user" | cut -d: -f6)

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限安装到 /usr/local 与 /etc/systemd/user" >&2
    exit 1
fi

libssc_prefix=/usr/local/loa-libssc
if [ ! -e "$libssc_prefix/lib/libssc.so" ]; then
    echo "缺少 CCT 版 libssc：$libssc_prefix/lib/libssc.so" >&2
    echo "先按 tools/upstream/libssc/README.md 在设备上构建并安装" >&2
    exit 1
fi

build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT

read -ra cflags <<<"$(PKG_CONFIG_PATH="$libssc_prefix/lib/pkgconfig" \
    pkg-config --cflags glib-2.0 gio-2.0 libssc)"
read -ra libs <<<"$(PKG_CONFIG_PATH="$libssc_prefix/lib/pkgconfig" \
    pkg-config --libs glib-2.0 gio-2.0 libssc)"

cc -O2 -Wall -Wextra \
    -o "$build_dir/loa-alsd" "$script_dir/loa-alsd.c" \
    "${cflags[@]}" "${libs[@]}" \
    -Wl,-rpath,"$libssc_prefix/lib" -lm

install -Dm755 "$build_dir/loa-alsd" /usr/local/libexec/loa-alsd
install -Dm644 "$script_dir/loa-alsd.service" /etc/systemd/user/loa-alsd.service

# 直接写入 graphical-session.target 的依赖链接，用户未登录时也能完成启用。
wants_dir="$home/.config/systemd/user/graphical-session.target.wants"
install -d -o "$user" -g "$group" "$wants_dir"
ln -sfn /etc/systemd/user/loa-alsd.service "$wants_dir/loa-alsd.service"

session_env=(XDG_RUNTIME_DIR="/run/user/$uid"
    DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus")

if [ -S "/run/user/$uid/bus" ]; then
    runuser -u "$user" -- env "${session_env[@]}" systemctl --user daemon-reload
    runuser -u "$user" -- env "${session_env[@]}" systemctl --user restart loa-alsd.service
else
    echo "用户会话未运行，服务将在 $user 下次登录时启动"
fi

echo "安装完成：/usr/local/libexec/loa-alsd"
echo "服务状态：systemctl --user status loa-alsd.service"
echo "属性查看：gdbus introspect --session --dest org.loa.AmbientLight --object-path /org/loa/AmbientLight"
