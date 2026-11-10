// SPDX-License-Identifier: GPL-3.0-or-later
// 平板设置的独立窗口入口。页面内容与系统设置 KCM 共用 TabletSettingsView。
import QtQuick
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

Kirigami.ApplicationWindow {
    id: root
    title: "平板设置"
    width: 720
    height: 640
    minimumWidth: 480
    minimumHeight: 400

    pageStack.initialPage: FormCard.FormCardPage {
        title: "平板设置"
        TabletSettingsView {}
    }
}
