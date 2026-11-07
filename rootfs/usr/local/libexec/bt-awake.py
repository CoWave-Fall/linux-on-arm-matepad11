#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""连接笔或键盘时阻止 QCA 蓝牙控制器进入带内休眠。

hci_qca 在 TX 空闲达到 tx_idle_delay 后发送 HCI_IBS_SLEEP_IND，控制器
进入 IBS 休眠，下一次输入需要额外的唤醒握手。这里通过 BlueZ 的
Device1 属性判断是否存在已连接的 HID 设备，并调整驱动的 debugfs 参数：
有 HID 时设为接近无限，无 HID 时恢复默认 2000 ms。
"""

import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

HID_UUID = "00001812-0000-1000-8000-00805f9b34fb"
APPEARANCE_KEYBOARD = 0x03C1
APPEARANCE_DIGITIZER = 0x03C7

DELAY_PATH = "/sys/kernel/debug/bluetooth/hci0/ibs/tx_idle_delay"
DEFAULT_DELAY = 2000
AWAKE_DELAY = 0xFFFFFFFF


def hid_connected(bus):
    """返回是否存在已连接的 HID 设备。"""
    result = bus.call_sync(
        "org.bluez",
        "/",
        "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects",
        None,
        GLib.VariantType.new("(a{oa{sa{sv}}})"),
        Gio.DBusCallFlags.NONE,
        2000,
        None,
    )
    objects = result.unpack()[0]

    for interfaces in objects.values():
        dev = interfaces.get("org.bluez.Device1")
        if not dev or not dev.get("Connected", False):
            continue
        uuids = dev.get("UUIDs", [])
        appearance = dev.get("Appearance", 0)
        if HID_UUID in uuids or appearance in (
            APPEARANCE_KEYBOARD,
            APPEARANCE_DIGITIZER,
        ):
            return True
    return False


def read_delay():
    """读取当前空闲休眠延迟，控制器尚未出现时返回 None。"""
    try:
        with open(DELAY_PATH) as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


def write_delay(delay):
    """写入驱动的空闲休眠延迟，控制器重新绑定时可能暂时失败。"""
    try:
        with open(DELAY_PATH, "w") as f:
            f.write(f"{delay}\n")
        return True
    except OSError:
        return False


def main():
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    while True:
        try:
            connected = hid_connected(bus)
        except Exception:  # noqa: BLE001
            # BlueZ 重启期间 GetManagedObjects 可能失败，下一轮重试
            connected = None

        if connected is not None:
            want = AWAKE_DELAY if connected else DEFAULT_DELAY
            current = read_delay()
            # 控制器重新绑定后 debugfs 值会回到默认，按实际值判断
            if current is not None and current != want and write_delay(want):
                print("hold awake" if connected else "allow sleep", flush=True)
        time.sleep(1.0)


if __name__ == "__main__":
    main()
