#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 重建 power-profiles-daemon 的 qcom 驱动版本。
# pacman 在 power-profiles-daemon 包更新后通过 PostTransaction hook 调用；
# 手动执行同样有效。编译失败时保留 /usr/local 下已有的二进制，服务继续运行。
set -euo pipefail

pkg_name=power-profiles-daemon
src_dir=${PPD_SRC_DIR:-/usr/src/loa-ppd}
build_dir="$src_dir/build"
dest_dir=/usr/local/libexec/loa
dest="$dest_dir/power-profiles-daemon"
prev="$dest_dir/power-profiles-daemon.prev"
state_dir=/var/lib/loa
state="$state_dir/ppd-qcom.state"
log=/var/log/loa-ppd-rebuild.log

# 保留原始 stderr，脚本输出写入日志，关键提示同时返回给 pacman。
exec 3>&2
exec >>"$log" 2>&1

log_line() {
    printf '%s %s\n' "$(date '+%F %T %Z')" "$*"
}

notify() {
    echo "$*" >&3
}

# 图形会话内同时发桌面通知；没有活动会话时只保留 pacman 输出与日志。
desktop_notice() {
    local message=$1 runtime uid user
    for runtime in /run/user/*; do
        [ -S "$runtime/bus" ] || continue
        uid=${runtime##*/}
        user=$(id -un "$uid" 2>/dev/null) || continue
        runuser -u "$user" -- env XDG_RUNTIME_DIR="$runtime" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
            notify-send -a power-profiles-daemon "电源模式" "$message" \
            >/dev/null 2>&1 || true
    done
}

write_state() {
    local result=$1 reason=${2:-}
    {
        echo "result=$result"
        if [ -n "$reason" ]; then
            echo "reason=$reason"
        fi
        echo "checked_at=$(date '+%F %T %Z')"
        grep -E '^(package_version|source_version|built_at|source_dir)=' "$state" 2>/dev/null || true
    } >"$state.new"
    mv "$state.new" "$state"
}

fail() {
    # 状态目录可能尚未创建，失败路径本身不能再触发 ERR。
    trap - ERR
    log_line "错误：$*"
    mkdir -p "$state_dir" 2>/dev/null || true
    write_state failed "$*" 2>/dev/null || true
    notify "power-profiles-daemon qcom 重建失败：$*（详情见 $log）"
    desktop_notice "qcom 驱动重建失败，继续使用上一份二进制，详情见 $log"
    exit 1
}

trap 'fail "第 $LINENO 行命令失败"' ERR

log_line "开始重建"
mkdir -p "$dest_dir" "$state_dir"

[ -f "$src_dir/meson.build" ] || fail "缺少源码树 $src_dir"
[ -f "$src_dir/src/ppd-driver-qcom.c" ] || fail "源码树 $src_dir 中没有 qcom 驱动"

package_version=$(pacman -Q "$pkg_name" | awk '{print $2}')
source_version=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" \
    "$src_dir/meson.build" | head -n1)
[ -n "$source_version" ] || fail "无法读取 $src_dir/meson.build 的版本"

# 包版本变化意味着系统库可能一起更新，强制重新编译与链接，避免复用旧对象。
previous_version=$(sed -n 's/^package_version=//p' "$state" 2>/dev/null | head -n1)
if [ "${previous_version:-}" != "$package_version" ]; then
    log_line "包版本由 ${previous_version:-无} 变为 $package_version，重新编译"
    find "$src_dir/src" -type f \( -name '*.c' -o -name '*.h' \) -exec touch {} +
fi

# 已有的 build 目录复用配置；包更新带来的头文件与库变化由 ninja 重新链接。
if [ -d "$build_dir" ]; then
    meson setup --reconfigure "$build_dir" "$src_dir" \
        --prefix=/usr --libexecdir=/usr/libexec -Dtests=false
else
    meson setup "$build_dir" "$src_dir" \
        --prefix=/usr --libexecdir=/usr/libexec -Dtests=false
fi
ninja -C "$build_dir"

# 安装前保留上一份可用二进制，供编译异常时手动回退。
if [ -x "$dest" ]; then
    install -m755 "$dest" "$prev"
fi
install -m755 "$build_dir/src/power-profiles-daemon" "$dest"

{
    echo "package_version=$package_version"
    echo "source_version=$source_version"
    echo "built_at=$(date '+%F %T %Z')"
    echo "source_dir=$src_dir"
} >"$state"
write_state ok

if [ "${PPD_SKIP_RESTART:-0}" != 1 ]; then
    if ! systemctl try-restart power-profiles-daemon.service; then
        log_line "警告：服务重启失败，新二进制将在下次启动时生效"
        notify "power-profiles-daemon 已更新，服务重启失败，将在下次启动时生效"
    fi
fi

log_line "完成：包 $package_version，源码 $source_version"

package_base=${package_version%-*}
if [ "$package_base" != "$source_version" ]; then
    message="power-profiles-daemon 包版本 $package_version 与源码版本 $source_version 存在差异，qcom 功能保持可用；请同步 $src_dir 到新版本后重新编译"
    log_line "$message"
    notify "$message"
    desktop_notice "$message"
fi
