# 内核

基于 Linux 7.3.1。`patches/` 中的补丁按编号顺序应用，`defconfig` 为
`arch/arm64/configs/dby-w09_defconfig` 的内容。

## 补丁

| 补丁 | 内容 |
|:--|:--|
| `0001-firmware-psci-add-s2ram-via-cpu-suspend.patch` | 通过 `CPU_SUSPEND` 进入系统挂起，`sm8250.dtsi` 增加 `psci` 标签 |
| `0002-arm64-dts-qcom-add-dby-w09.patch` | 新增板级设备树（DTS 的唯一来源）与 `Makefile` 条目 |
| `0003-input-touchscreen-add-novatek-n9z.patch` | Novatek N9Z 触摸驱动、绑定与 Kconfig / Makefile |
| `0004-backlight-add-rt8555.patch` | RT8555 背光驱动、绑定与 Kconfig / Makefile |
| `0005-power-supply-add-mt5727-and-pm8150b.patch` | MT5727 无线供电与 PM8150B 电池驱动、绑定与 Kconfig / Makefile |
| `0006-drm-panel-nt36523-add-dby-w09.patch` | NT36523 面板的 DBY-W09 描述、初始化序列与供电 |
| `0007-drm-msm-dsi-phy-7nm-add-own-pll.patch` | 从 PHY 使用本地 PLL 产生位时钟 |

补丁顺序固定：`0001` 为 `0002` 提供 `&psci` 标签，其余补丁相互独立。

## 配置

`defconfig` 的关键项：

- `CONFIG_LOCALVERSION="-loa"`，内核版本为 `7.3.1-loa`。
- 触摸、显示、电源、无线与传感器驱动全部编入内核。
- `CONFIG_EXTRA_FIRMWARE` 包含触摸固件与 ath11k 校准数据。
- `CONFIG_DEBUG_FS` 供 `bt-awake` 调整蓝牙空闲休眠延迟。

## 固件

构建前把 [固件清单](../docs/firmware.md) 中的文件放入 `kernel/firmware/`。
`scripts/build-kernel.sh` 会复制到内核源码树的 `firmware/` 目录。

## 构建

```bash
scripts/fetch-sources.sh
scripts/build-kernel.sh
```

输出 `out/Image.gz` 与 `out/dby-w09.dtb`。重新构建使用
`scripts/build-kernel.sh --clean`。
