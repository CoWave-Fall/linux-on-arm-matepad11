# Linux on ARM: 华为 MatePad 11 2021（DBY-W09）

在华为 MatePad 11 2021（`DBY-W09`，Qualcomm SM8250）上运行主线 Linux 的
发布仓库。

U-Boot 写入 `recovery` 分区，Linux 从 microSD 启动，HarmonyOS
保留正常启动能力。

## 支持情况

| 功能 | 状态 | 说明 |
|:--|:--|:--|
| U-Boot 与双系统菜单 | 可用 | 启动 Linux、HarmonyOS、fastboot 与复位 |
| 显示 | 可用 | 双 DSI，1600 x 2560，30 / 60 / 120 Hz |
| 触摸 | 可用 | 五点坐标、双指、手掌抑制、开关屏恢复 |
| 背光 | 可用 | RT8555，10 位亮度 |
| 电池与充电 | 可用 | PM8150B，容量、电压、电流、温度 |
| WiFi | 可用 | QCA6391，ath11k，NetworkManager |

## 构建与安装

构建脚本位于 `scripts/`。安装流程、固件来源与已知限制随文档补充。
