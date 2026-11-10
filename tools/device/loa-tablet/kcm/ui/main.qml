// SPDX-License-Identifier: GPL-3.0-or-later
// 系统设置内嵌页面。内容主体与独立应用共用 TabletSettingsView。
import QtQuick
import org.kde.kcmutils as KCMUtils

KCMUtils.SimpleKCM {
    id: root

    TabletSettingsView {
        width: parent.width
    }
}
