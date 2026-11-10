# 支持情况与已知限制

更新时间：2026-11-10 19:06 +8

## 验证环境

| 项目 | 值 |
|:--|:--|
| 设备 | 华为 MatePad 11 2021，`DBY-W09` |
| SoC | Qualcomm SM8250，Adreno 650 |
| 内核 | Linux 7.3.1，`7.3.1-loa` |
| U-Boot | 2026.10 |

## 功能状态

| 子系统 | 状态 | 验证内容 |
|:--|:--|:--|
| 启动链 | 可用 | U-Boot 菜单启动 Linux、HarmonyOS、fastboot；双系统往返切换 |
| 存储 | 可用 | UFS 枚举、microSD 根文件系统、SDR104 |
| USB | 可用 | ACM 控制台与 ECM 网络，SSH 与 SCP |
| 显示 | 可用 | 1600 x 2560 整屏，30 / 60 / 120 Hz，冷启动出图 |
| 背光 | 可用 | 10 位亮度，`max_brightness` 1023 |
| 触摸 | 可用 | 五点、双指、手掌抑制、开关屏恢复、分段线性校准 |
| 磁吸键盘 | 可用 | HOGP 输入、自动回连、吸附供电 |
| 无线供电 | 可用 | MT5727 键盘线圈、霍尔自动控制、私有握手 |
| WiFi | 可用 | 扫描、连接、DHCP、重启自动连接 |
| 蓝牙 | 可用 | HCI 固件加载、从零配对、HID 合并 |
| GPU | 可用 | GMU、zap shader、devfreq，Plasma 渲染 |
| CPU 调频 | 可用 | 三个 freq domain，`schedutil` / `performance` / `powersave` |
| 电池与充电 | 可用 | SMB5 状态、FG 容量、电压、电流、温度 |
| 传感器 | 可用 | 加速度计、环境光、磁力计、指南针 |
| 自动旋转 | 可用 | KWin 与 iio-sensor-proxy |
| 自动亮度 | 可用 | KWin 读取环境光 |

## 已知限制

各子系统的限制随功能接入补充。
