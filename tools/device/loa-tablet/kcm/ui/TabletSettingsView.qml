// SPDX-License-Identifier: GPL-3.0-or-later
// 平板设置内容主体。独立应用与系统设置的 KCM 共用本文件，
// 状态读写统一走 loa-tabletd 的 org.loa.Tablet1 接口。
import QtQuick
import QtQuick.Layouts
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.plasma.workspace.dbus as DBus

ColumnLayout {
    id: view

    // 后端返回的显示状态，字段见 loa-tabletd 的 GetState。
    property var state: ({})
    // 命令执行期间暂停轮询，避免开关被旧状态覆盖。
    property bool busy: false
    // 程序化设置开关时抑制 onToggled 回环。
    property bool syncing: false
    property string statusText: ""
    // 组合键绑定，字段见 loa-tabletd 的 GetKeyBindings。
    property var keyBindings: ({ comboVolumeDown: "", comboVolumeUp: "" })
    // 程序化设置下拉框时抑制保存回环。
    property bool keySyncing: false
    // 自定义命令的本地草稿；切回预设后仍保留，方便再次选择。
    property string customVolumeDown: ""
    property string customVolumeUp: ""
    // 预设命令由 loa-tabletd 的 GetKeyBindings 返回，首次应答前使用内置值。
    property string screenshotCommand: "spectacle -b -f"
    property string powerMenuCommand: "qdbus6 org.kde.LogoutPrompt /LogoutPrompt org.kde.LogoutPrompt.promptAll"
    readonly property var keyActionLabels: ["无动作", "截屏", "电源菜单", "自定义命令"]

    // 两种宿主都把本组件放进 ColumnLayout，需要撑满可用宽度。
    Layout.fillWidth: true
    spacing: 0

    function reportError(message) {
        statusText = message;
        console.warn("loa-tablet:", message);
    }

    function call(member, arguments, onSuccess, onError) {
        const pending = DBus.SessionBus.asyncCall({
            service: "org.loa.Tablet",
            path: "/org/loa/Tablet",
            iface: "org.loa.Tablet1",
            member: member,
            arguments: arguments || []
        });
        pending.finished.connect(() => {
            const failed = pending.isError;
            const value = pending.value;
            const message = failed ? pending.error.message : "";
            pending.destroy();
            if (failed) {
                if (onError) {
                    onError(message);
                } else {
                    view.reportError(message);
                }
                return;
            }
            view.statusText = "";
            if (onSuccess && value !== undefined) {
                onSuccess(value);
            } else if (onSuccess) {
                onSuccess();
            }
        });
    }

    function applyState(next) {
        syncing = true;
        state = next;
        autoRotateSwitch.checked = next.autoRotate === "always" || next.autoRotate === "automatic";
        autoBrightnessSwitch.checked = next.autoBrightness === true;
        syncing = false;
    }

    function refresh() {
        call("GetState", [], (value) => {
            try {
                view.applyState(JSON.parse(value));
            } catch (error) {
                view.reportError("状态解析失败：" + error);
            }
        });
    }

    function keyActionIndex(command) {
        if (!command) {
            return 0;
        }
        if (command === screenshotCommand) {
            return 1;
        }
        if (command === powerMenuCommand) {
            return 2;
        }
        return 3;
    }

    function keyActionCommand(index, custom) {
        switch (index) {
        case 0: return "";
        case 1: return screenshotCommand;
        case 2: return powerMenuCommand;
        default: return custom;
        }
    }

    function applyKeyBindings(bindings) {
        keySyncing = true;
        keyBindings = bindings || ({ comboVolumeDown: "", comboVolumeUp: "" });
        const presets = keyBindings.presets || ({});
        if (presets.screenshot) {
            screenshotCommand = presets.screenshot;
        }
        if (presets.powerMenu) {
            powerMenuCommand = presets.powerMenu;
        }
        const down = keyBindings.comboVolumeDown || "";
        const up = keyBindings.comboVolumeUp || "";
        const downIndex = keyActionIndex(down);
        const upIndex = keyActionIndex(up);
        volumeDownCombo.currentIndex = downIndex;
        volumeUpCombo.currentIndex = upIndex;
        if (downIndex === 3) {
            customVolumeDown = down;
            if (!volumeDownCustom.fieldActiveFocus) {
                volumeDownCustom.text = down;
            }
        }
        if (upIndex === 3) {
            customVolumeUp = up;
            if (!volumeUpCustom.fieldActiveFocus) {
                volumeUpCustom.text = up;
            }
        }
        keySyncing = false;
    }

    function refreshKeyBindings() {
        call("GetKeyBindings", [], (value) => {
            try {
                view.applyKeyBindings(JSON.parse(value));
            } catch (error) {
                view.reportError("按键配置解析失败：" + error);
            }
        });
    }

    function saveKeyBindings() {
        // 保存期间控件被禁用会再次触发 editingFinished，忽略重入。
        if (busy) {
            return;
        }
        const downIndex = volumeDownCombo.currentIndex;
        const upIndex = volumeUpCombo.currentIndex;
        // 选中自定义命令但尚未输入时保留原有绑定，另一侧的修改照常保存。
        const down = downIndex === 3 && !customVolumeDown
            ? (keyBindings.comboVolumeDown || "")
            : keyActionCommand(downIndex, customVolumeDown);
        const up = upIndex === 3 && !customVolumeUp
            ? (keyBindings.comboVolumeUp || "")
            : keyActionCommand(upIndex, customVolumeUp);
        busy = true;
        call("SetKeyBindings", [new DBus.string(JSON.stringify({
            comboVolumeDown: down,
            comboVolumeUp: up
        }))],
            () => { busy = false; refreshKeyBindings(); },
            (message) => { busy = false; reportError(message); });
    }

    function setAutoRotate(enabled) {
        busy = true;
        call("SetAutoRotate", [new DBus.bool(enabled)],
            () => { busy = false; refresh(); },
            (message) => { busy = false; reportError(message); });
    }

    function setAutoBrightness(enabled) {
        busy = true;
        call("SetAutoBrightness", [new DBus.bool(enabled)],
            () => { busy = false; refresh(); },
            (message) => { busy = false; reportError(message); });
    }

    function rotate(orientation) {
        busy = true;
        call("Rotate", [new DBus.string(orientation)],
            () => { busy = false; refresh(); },
            (message) => { busy = false; reportError(message); });
    }

    function rotationLabel(rotation) {
        switch (rotation) {
        case "normal": return "竖屏";
        case "inverted": return "竖屏（倒置）";
        case "right": return "横屏";
        case "left": return "横屏（反向）";
        default: return "未知";
        }
    }

    function policyLabel(policy) {
        switch (policy) {
        case "never": return "关闭";
        case "automatic": return "仅平板模式";
        case "always": return "开启";
        default: return "未知";
        }
    }

    function brightnessLabel(value) {
        if (value === null || value === undefined) {
            return "不可用";
        }
        return value ? "已开启" : "已关闭";
    }

    Component.onCompleted: {
        refresh();
        refreshKeyBindings();
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        onTriggered: if (!view.busy) view.refresh()
    }

    FormCard.FormHeader {
        Layout.fillWidth: true
        title: "屏幕旋转"
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        FormCard.FormSwitchDelegate {
            id: autoRotateSwitch
            text: "自动旋转"
            description: "跟随加速度计调整屏幕方向"
            enabled: !view.busy
            onToggled: if (!view.syncing) view.setAutoRotate(checked)
        }
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        FormCard.FormButtonDelegate {
            text: "横屏"
            icon.name: "object-rotate-right"
            enabled: !view.busy
            onClicked: view.rotate("right")
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormButtonDelegate {
            text: "竖屏"
            icon.name: "object-rotate-left"
            enabled: !view.busy
            onClicked: view.rotate("normal")
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormButtonDelegate {
            text: "竖屏（倒置）"
            icon.name: "object-rotate-left"
            enabled: !view.busy
            onClicked: view.rotate("inverted")
        }
    }

    FormCard.FormHeader {
        Layout.fillWidth: true
        title: "亮度"
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        FormCard.FormSwitchDelegate {
            id: autoBrightnessSwitch
            text: "自动适应环境亮度"
            description: "使用环境光传感器调整背光，手动调整会记录为偏移"
            enabled: !view.busy && view.state.autoBrightness !== null
            onToggled: if (!view.syncing) view.setAutoBrightness(checked)
        }
    }

    FormCard.FormHeader {
        Layout.fillWidth: true
        title: "按键"
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        FormCard.FormTextDelegate {
            text: "电源键"
            description: "短按开关屏幕；按住 2.5 秒弹出离开对话框，继续按住到 7 秒强制重启"
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormComboBoxDelegate {
            id: volumeDownCombo
            text: "电源 + 音量减"
            description: "默认截屏，组合键不会改变音量"
            model: view.keyActionLabels
            enabled: !view.busy
            onActivated: if (!view.keySyncing) view.saveKeyBindings()
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormComboBoxDelegate {
            id: volumeUpCombo
            text: "电源 + 音量加"
            description: "默认无动作"
            model: view.keyActionLabels
            enabled: !view.busy
            onActivated: if (!view.keySyncing) view.saveKeyBindings()
        }
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        visible: volumeDownCombo.currentIndex === 3
        FormCard.FormTextFieldDelegate {
            id: volumeDownCustom
            label: "音量减自定义命令"
            placeholderText: "以会话用户的 shell 执行"
            enabled: !view.busy
            onEditingFinished: {
                if (view.keySyncing) {
                    return;
                }
                view.customVolumeDown = text;
                view.saveKeyBindings();
            }
        }
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        visible: volumeUpCombo.currentIndex === 3
        FormCard.FormTextFieldDelegate {
            id: volumeUpCustom
            label: "音量加自定义命令"
            placeholderText: "以会话用户的 shell 执行"
            enabled: !view.busy
            onEditingFinished: {
                if (view.keySyncing) {
                    return;
                }
                view.customVolumeUp = text;
                view.saveKeyBindings();
            }
        }
    }

    FormCard.FormHeader {
        Layout.fillWidth: true
        title: "状态"
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        FormCard.FormTextDelegate {
            text: "当前方向"
            description: view.rotationLabel(view.state.rotation)
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormTextDelegate {
            text: "自动旋转策略"
            description: view.policyLabel(view.state.autoRotate)
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormTextDelegate {
            text: "自动亮度"
            description: view.brightnessLabel(view.state.autoBrightness)
        }
        FormCard.FormDelegateSeparator {}
        FormCard.FormTextDelegate {
            text: "缩放"
            description: view.state.scale ? Math.round(view.state.scale * 100) + "%" : "未知"
        }
    }

    FormCard.FormCard {
        Layout.fillWidth: true
        visible: view.statusText.length > 0
        FormCard.FormTextDelegate {
            text: "操作失败"
            description: view.statusText
        }
    }
}
