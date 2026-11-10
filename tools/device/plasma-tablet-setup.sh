#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 平板桌面体验配置：虚拟键盘、触摸屏边缘手势与旋转快捷方式。
# 以桌面用户身份在图形会话内运行；重复执行结果相同。

set -euo pipefail

if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    echo "缺少图形会话环境，请在桌面用户下执行" >&2
    exit 1
fi

kw() {
    kwriteconfig6 --file kwinrc "$@"
}

# 虚拟键盘：选用 Plasma Keyboard，触摸与数位板输入时自动显示，
# 叠加在窗口之上，避免压缩当前窗口。
kw --group Wayland --key InputMethod /usr/share/applications/org.kde.plasma.keyboard.desktop
kw --group Wayland --key VirtualKeyboardMode 1
kw --group Windows --key OverlayVirtualKeyboardOnWindows true

# 触摸屏边缘手势，动作取值见 KWin 的 electricBorderAction()
kw --group TouchEdges --key Left applicationlauncher
kw --group TouchEdges --key Right krunner
kw --group TouchEdges --key Bottom showdesktop

# 边缘触发宽度，默认 8 px 对指尖偏窄
kw --group ScreenEdges --key TouchTarget 16

# 屏幕旋转快捷方式统一走平板设置后端，手动旋转会同时关闭自动旋转。
apps="$HOME/.local/share/applications"
mkdir -p "$apps"

write_desktop() {
    local file="$1" name="$2" orientation="$3"
    cat >"$apps/$file" <<EOF
[Desktop Entry]
Type=Application
Name=$name
Exec=/usr/local/bin/loa-tabletctl rotate $orientation
Icon=object-rotate-left
Terminal=false
Categories=System;
EOF
}

write_desktop loa-rotate-normal.desktop "切换到横屏" right
write_desktop loa-rotate-left.desktop "切换到竖屏" normal
write_desktop loa-rotate-right.desktop "切换到竖屏（反向）" inverted

# 边缘手势立即生效；虚拟键盘在下次登录时加载
qdbus6 org.kde.KWin /KWin reconfigure 2>/dev/null || true

echo "桌面配置已写入，虚拟键盘在下次登录后启用"
