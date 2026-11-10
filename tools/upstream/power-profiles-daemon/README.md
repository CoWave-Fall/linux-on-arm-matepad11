# power-profiles-daemon 的 qcom 驱动

DBY-W09 的 CPU 由 `qcom-cpufreq-hw` 调节。该驱动没有 energy performance
preference 接口，上游 power-profiles-daemon 也没有对应的 CPU 驱动，KDE
电源模式只能显示，切换后没有硬件动作。

本目录为 power-profiles-daemon 增加 `qcom` CPU 驱动，并同时限制 Adreno
devfreq 的频率范围。KDE PowerDevil 通过
`org.freedesktop.UPower.PowerProfiles` 调用该驱动。

## 档位

| Profile | CPU governor | CPU 上限 | GPU min/max |
|:--|:--|:--|:--|
| performance | `performance` | 各 cluster 最高频 | 587 / 587 MHz |
| balanced | `schedutil` | 各 cluster 最高频 | 305 / 587 MHz |
| power-saver | `schedutil` | 最高频的 60% | 305 / 400 MHz |

CPU 上限按 `scaling_available_frequencies` 向下取档，GPU 上限按
`available_frequencies` 向下取档。

## 源码版本

驱动基于 power-profiles-daemon 提交 `1a151cc`（2026-10-08）：

```
https://gitlab.freedesktop.org/upower/power-profiles-daemon.git
```

| 文件 | 用途 |
|:--|:--|
| `ppd-driver-qcom.c` | CPU 与 GPU 档位驱动 |
| `ppd-driver-qcom.h` | 类型声明 |
| `integration.patch` | 把新驱动加入 meson 构建与驱动列表 |

## 部署

设备需要 `gcc`、`meson`、`ninja`、`glib2`（提供 `glib-mkenums`）与
`python-gobject`（提供 `powerprofilesctl`）。

设备上先取得对应版本的源码树：

```bash
git clone https://gitlab.freedesktop.org/upower/power-profiles-daemon.git \
  /root/ppd-src
git -C /root/ppd-src checkout 1a151cc
```

在主机执行：

```bash
tools/host/ppd-qcom-deploy.sh
```

脚本把驱动源码与 `tools/device/ppd-qcom/` 下的 systemd drop-in、pacman
hook 送到设备，并在设备上编译安装。自编译二进制位于
`/usr/local/libexec/loa/power-profiles-daemon`，`/usr/lib/power-profiles-daemon`
保持发行包原件。

## 软件包更新

`/etc/pacman.d/hooks/50-ppd-qcom.hook` 在 `power-profiles-daemon` 包更新后
执行 `/usr/local/libexec/loa/ppd-qcom-rebuild.sh`。脚本用
`/usr/src/loa-ppd` 重新编译并安装，随后重启服务；编译失败时保留上一份
二进制，状态写入 `/var/lib/loa/ppd-qcom.state`，日志位于
`/var/log/loa-ppd-rebuild.log`。

手动重建：

```bash
/usr/local/libexec/loa/ppd-qcom-rebuild.sh
```

## 验证

```bash
powerprofilesctl list
busctl get-property org.freedesktop.UPower.PowerProfiles \
  /org/freedesktop/UPower/PowerProfiles \
  org.freedesktop.UPower.PowerProfiles ActiveProfile
busctl set-property org.freedesktop.UPower.PowerProfiles \
  /org/freedesktop/UPower/PowerProfiles \
  org.freedesktop.UPower.PowerProfiles ActiveProfile s power-saver
```

## 来源与许可证

源码为 power-profiles-daemon 提交 `1a151cc`，上游项目地址
`https://gitlab.freedesktop.org/upower/power-profiles-daemon.git`。
项目采用 `GPL-3.0-or-later`，本目录的驱动与补丁沿用该许可证。
