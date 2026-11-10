# 工具

| 目录 | 内容 |
|:--|:--|
| `host/` | 主机侧启动、控制台、部署脚本与主机网络配置 |
| `device/` | 设备侧程序与安装脚本 |
| `fit/` | FIT 描述文件 |
| `upstream/` | BlueZ、hexagonrpcd、libssc 与 power-profiles-daemon 的补丁 |
| `vendor/` | AOSP `mkbootimg` 与 `avbtool` |

`upstream/` 中的补丁用于本仓库的本地构建，暂无提交上游的计划；补丁注释
沿用中文。

## 主机侧

| 文件 | 作用 |
|:--|:--|
| `host/boot-linux.sh` | 写入 FIT 并执行 `bootm` |
| `host/usb-console.py` | 通过 USB ACM 执行命令 |
| `host/powerkey-deploy.sh` | 交叉编译并部署 `dby-powerkeyd` |
| `host/ppd-qcom-deploy.sh` | 部署 power-profiles-daemon 的 `qcom` 驱动 |
| `host/ath11k-board-data.py` | 生成 ath11k 板级数据 |
| `host/10-loa-usb0.link` | 主机端固定命名 |
| `host/loa-usb0.nmconnection` | 主机端静态地址 |

## 设备侧

| 文件位置 | 作用 |
|:--|:--|
| `device/dby-powerkeyd.c` | 电源键与音量键手势 |
| `device/loa-pen.c` | 面板笔与蓝牙压力合并 |
| `device/loa-tablet/` | 平板设置后端、命令行、KCM 与附件通知 |
| `device/loa-alsd/` | 环境光色温的会话 D-Bus 服务 |
| `device/kwin-ambient-color/` | KWin 环境色温效果 |
| `device/ppd-qcom/` | power-profiles-daemon 的部署与 pacman hook |
| `device/plasma-tablet-setup.sh` | 虚拟键盘、触摸边缘手势与旋转快捷方式 |
| `device/bt-pair-accessory.sh` | 蓝牙附件配对的辅助脚本 |
| `device/vibra-test.c` | 震动马达测试 |

Python 服务与 shell 脚本由 `rootfs/` 安装。原生程序由
`scripts/build-device-tools.sh` 交叉编译，再推送到设备。
