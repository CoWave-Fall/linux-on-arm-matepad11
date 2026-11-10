# 根文件系统配置

本目录中的文件按设备上的目标位置组织，由
`scripts/install-rootfs.sh --root <挂载点>` 复制到 SD 卡。

## 网络

| 文件 | 目标位置 | 作用 |
|:--|:--|:--|
| `etc/systemd/network/10-usb0.network` | 同位置 | USB ECM 的静态地址 `10.0.0.2/24` |
| `etc/systemd/network/70-wlan0.link` | 同位置 | 按 `Type=wlan` 与 `Driver=ath11k_pci` 固定命名 |
| `etc/NetworkManager/conf.d/90-loa.conf` | 同位置 | `usb0` 由 systemd-networkd 管理，DNS 交给 systemd-resolved |

## 服务

| 单元 | 作用 |
|:--|:--|
| `dby-console.service` | USB ACM 控制台与 ECM 网络 |
| `dby-powerkey.service` | 电源键与音量键手势 |
| `dby-slpi-firmware.service` | 根文件系统就绪后重绑 SLPI |
| `hexagonrpcd-sdsp.service` | 传感器 DSP 的文件服务 |
| `loa-pen.service` | 面板笔与蓝牙 HID 合并 |
| `bt-awake.service` | 连接 HID 时保持蓝牙控制器唤醒 |
| `mt5727-bt-link.service` | 附件吸附后给蓝牙上电 |
| `pen-link-keepalive.service` | 维持笔的厂商通知订阅 |
| `qca-bt-rebind.service` | 根文件系统就绪后重新绑定蓝牙 UART |

## 蓝牙

| 文件 | 作用 |
|:--|:--|
| `etc/bluetooth/loa-main.conf` | 关闭 `FilterDiscoverable`，用于发现 M-Pencil 3 |
| `etc/systemd/system/bluetooth.service.d/override.conf` | 使用补丁版 `bluetoothd` |
| `etc/udev/rules.d/70-mt5727-bt-link.rules` | 吸附事件触发蓝牙上电 |
| `etc/udev/rules.d/80-loa-pen.rules` | 让 libinput 忽略笔的原始节点 |

## 桌面

平板设置与环境色温的用户级单元由 `tools/device/` 下的安装脚本部署。
本目录只包含系统级单元与配置。

## 安装

```bash
sudo scripts/install-rootfs.sh --root /mnt/loa-root \
  --user <桌面用户名> --firmware firmware-root
```

脚本会启用基础单元并加入 `IgnorePkg = linux-aarch64`。桌面与蓝牙单元
在安装对应软件包后按 [操作全流程](../docs/install.md) 启用。
