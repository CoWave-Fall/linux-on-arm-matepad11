#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""保持 HUAWEI M-Pencil 3 的厂商通知订阅。

笔在连接后约 8 秒内如果没有看到厂商通知特征（UUID 0xfe42）的订阅，就以
reason 0x13 主动断开。订阅建立后，空闲连接可以长期保持。

GATT 对象路径在每次重连后都会变化，不同笔之间也各不相同，因此通过 BlueZ
的对象管理器查找特征，避免写死路径。
"""

import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

VENDOR_SERVICE_PREFIX = "0000fe40"
VENDOR_NOTIFY_PREFIX = "0000fe42"
RETRY_SECONDS = 1.0


def find_notify_characteristic(bus):
    """返回厂商通知特征的对象路径。

    笔未连接或 BlueZ 尚未导出厂商服务时返回 None。
    """
    try:
        reply = bus.call_sync(
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
    except Exception:  # noqa: BLE001 - BlueZ 重启属于正常现象
        return None

    objects = reply.unpack()[0]
    services = set()
    for path, interfaces in objects.items():
        service = interfaces.get("org.bluez.GattService1")
        if service and str(service.get("UUID", "")).lower().startswith(
            VENDOR_SERVICE_PREFIX
        ):
            services.add(path)

    for path, interfaces in objects.items():
        characteristic = interfaces.get("org.bluez.GattCharacteristic1")
        if not characteristic:
            continue
        if str(characteristic.get("UUID", "")).lower().startswith(
            VENDOR_NOTIFY_PREFIX
        ):
            service_path = str(characteristic.get("Service", ""))
            if service_path in services or not services:
                return path
    return None


def start_notify(bus, path):
    """启用该特征的通知，返回是否成功。"""
    try:
        bus.call_sync(
            "org.bluez",
            path,
            "org.bluez.GattCharacteristic1",
            "StartNotify",
            None,
            None,
            Gio.DBusCallFlags.NONE,
            1000,
            None,
        )
        return True
    except Exception:  # noqa: BLE001 - 笔可能在调用期间断开
        return False


def main():
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    enabled = None
    while True:
        path = find_notify_characteristic(bus)
        current = False
        if path is not None:
            current = start_notify(bus, path)

        if current != enabled:
            if current:
                print("notify enabled", flush=True)
            else:
                print("notify pending", flush=True)
            enabled = current
        time.sleep(RETRY_SECONDS)


if __name__ == "__main__":
    main()
