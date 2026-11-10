#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# 在设备上安装平板设置后端、独立应用与系统设置 KCM，并启用用户服务。
# 用法：sudo ./install.sh <桌面用户名>
set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
user=${1:-${SUDO_USER:-}}
if [ -z "$user" ] || [ "$user" = root ]; then
    echo "用法：sudo ./install.sh <桌面用户名>" >&2
    exit 2
fi
uid=$(id -u "$user")

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限安装到 /usr 与 /etc/systemd/user" >&2
    exit 1
fi

# KCM 在设备上就地编译，缺少构建组件时直接终止。
for tool in cmake ninja; do
    command -v "$tool" >/dev/null || {
        echo "缺少 $tool，请先安装" >&2
        exit 1
    }
done
if [ ! -d /usr/share/ECM ]; then
    echo "缺少 extra-cmake-modules，请先安装" >&2
    exit 1
fi

# 组合键配置由当前用户所有，root 的 dby-powerkeyd 只读；重复安装时保留
# 已有内容。首次创建写入默认绑定：电源+音量下截屏（与 loa-tabletd 的
# PRESET_KEY_BINDINGS 一致）。目录交给该用户，loa-tabletd 才能用临时
# 文件加 rename 的方式原子替换配置。
group=$(id -gn "$user")
install -d -m 755 -o "$user" -g "$group" /var/lib/loa
if [ ! -e /var/lib/loa/powerkey.conf ]; then
    printf '%s\n' \
        '# 电源键组合动作，由平板设置写入。空值表示不响应。' \
        'combo_volume_down=spectacle -b -f' \
        'combo_volume_up=' >/var/lib/loa/powerkey.conf
    chown "$user:$group" /var/lib/loa/powerkey.conf
    chmod 644 /var/lib/loa/powerkey.conf
fi

install -Dm755 "$script_dir/loa-tabletd" /usr/local/libexec/loa-tabletd
install -Dm755 "$script_dir/loa-tablet-notify" /usr/local/libexec/loa-tablet-notify
install -Dm755 "$script_dir/loa-tablet-settings" /usr/local/bin/loa-tablet-settings
install -Dm755 "$script_dir/loa-tabletctl" /usr/local/bin/loa-tabletctl
install -Dm644 "$script_dir/main.qml" /usr/local/share/loa-tablet-settings/main.qml
install -Dm644 "$script_dir/kcm/ui/TabletSettingsView.qml" \
    /usr/local/share/loa-tablet-settings/TabletSettingsView.qml
install -Dm644 "$script_dir/org.loa.tablet.settings.desktop" \
    /usr/local/share/applications/org.loa.tablet.settings.desktop
install -Dm644 "$script_dir/settings-hardware-tablet.desktop" \
    /usr/local/share/systemsettings/categories/settings-hardware-tablet.desktop
install -Dm644 "$script_dir/loa-tabletd.service" /etc/systemd/user/loa-tabletd.service
install -Dm644 "$script_dir/loa-tablet-notify.service" \
    /etc/systemd/user/loa-tablet-notify.service

# 早期版本以外部应用模块接入，条目与内嵌 KCM 重复。
rm -f /usr/local/share/plasma/systemsettings/externalmodules/org.loa.tablet.settings.desktop
# 早期构建产物带 lib 前缀，与 KCM 的 Id 不一致。
rm -f /usr/lib/qt6/plugins/plasma/kcms/systemsettings/libkcm_loa_tablet.so

# kcmutils_add_qml_kcm 生成插件与 desktop 文件，QML 打包在插件的 QRC 内。
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
cmake -S "$script_dir/kcm" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=ON
cmake --build "$build_dir"
cmake --install "$build_dir"

# 直接写入 graphical-session.target 的依赖链接，用户未登录时也能完成启用。
home=$(getent passwd "$user" | cut -d: -f6)
wants_dir="$home/.config/systemd/user/graphical-session.target.wants"
install -d -o "$user" -g "$group" "$wants_dir"
for unit in loa-tabletd.service loa-tablet-notify.service; do
    ln -sfn "/etc/systemd/user/$unit" "$wants_dir/$unit"
done

session_env=(XDG_RUNTIME_DIR="/run/user/$uid"
    DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus")

if [ -S "/run/user/$uid/bus" ]; then
    runuser -u "$user" -- env "${session_env[@]}" systemctl --user daemon-reload
    runuser -u "$user" -- env "${session_env[@]}" systemctl --user restart loa-tabletd.service
    runuser -u "$user" -- env "${session_env[@]}" systemctl --user restart loa-tablet-notify.service
    # 新桌面文件需要重建 KDE 服务缓存。
    runuser -u "$user" -- env "${session_env[@]}" kbuildsycoca6 >/dev/null 2>&1 || true
else
    echo "用户会话未运行，服务将在 $user 下次登录时启动"
fi

echo "安装完成：/usr/local/bin/loa-tablet-settings"
echo "系统设置条目：已连接的设备 → 平板"
echo "服务状态：systemctl --user status loa-tabletd.service"
