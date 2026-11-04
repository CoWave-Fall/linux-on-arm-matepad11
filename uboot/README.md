# U-Boot

基于 U-Boot 2026.10。`patches/` 中的补丁按编号顺序应用，defconfig 由
构建脚本直接放入源码树。

## 文件

| 文件 | 目标位置 | 内容 |
|:--|:--|:--|
| `dby-w09_defconfig` | `configs/dby-w09_defconfig` | DBY-W09 的 U-Boot 配置 |

板级设备树由 `0004-dts-qcom-add-dby-w09.patch` 创建，默认环境由
`0005-board-qualcomm-add-dby-w09-env.patch` 创建，二者分别是 DTS 与
默认环境的唯一来源。

## 补丁

| 补丁 | 内容 |
|:--|:--|
| `0001-gpio-qcom-pmic-add-bias-and-input-config.patch` | PMIC GPIO 的上下拉与输入配置 |
| `0002-button-gpio-apply-pinctrl-default.patch` | 请求 GPIO 后应用 pinctrl 默认状态 |
| `0003-input-button-kbd-initialize-state.patch` | 以启动时的按键状态作为轮询初值 |
| `0004-dts-qcom-add-dby-w09.patch` | 新增板级设备树，DTS 的唯一来源 |
| `0005-board-qualcomm-add-dby-w09-env.patch` | 新增默认环境 `board/qualcomm/dby-w09.env`，启动菜单与 `bootargs` 的唯一来源 |

## 关键配置

- `CONFIG_DEFAULT_DEVICE_TREE="qcom/sm8250-huawei-dby-w09"`
- `CONFIG_ENV_DEFAULT_ENV_TEXT_FILE="board/qualcomm/dby-w09.env"`
- `CONFIG_FASTBOOT_FLASH_BLOCK=y`
- `CONFIG_FASTBOOT_FLASH_BLOCK_INTERFACE_NAME="scsi"`
- `CONFIG_FASTBOOT_FLASH_BLOCK_DEVICE_ID`（默认 0）
- `CONFIG_CMD_BCB=y`

## 构建

```bash
scripts/fetch-sources.sh
scripts/build-uboot.sh
```

输出 `out/u-boot.bin`。recovery 镜像的封装见
`scripts/pack-recovery.sh` 与 [操作全流程](../docs/install.md)。
