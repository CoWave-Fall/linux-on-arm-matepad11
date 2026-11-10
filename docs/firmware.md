# 固件清单

更新时间：2026-11-10 22:41 +8

本仓库省略厂商二进制文件，只记录文件位置、用途与校验值。构建前请从
设备自身的 vendor 分区取得对应文件，放入下表的目标位置。设备原厂系统
与分区备份提供了这些文件。

## 内核内置固件

以下文件在构建内核前放入 `kernel/firmware/`，`scripts/build-kernel.sh`
会把它们复制到内核源码树的 `firmware/` 目录。

| 文件 | 来源 | 校验值 |
|:--|:--|:--|
| `novatek/NT36523_THP1300.bin` | vendor 分区中 AFE HAL 库内嵌的 `NT36523_THP_BIN` | MD5 `777df0f42cbbe0628ce22b78c98a8cf1` |
| `ath11k/QCA6390/hw2.0/amss.bin` | 主机 `linux-firmware` 的 ath11k 目录 | SHA-256 `8ab2a06422e3989012df423bb6a156fde2c3feb4cd5db4c5d3588f674dc872ff` |
| `ath11k/QCA6390/hw2.0/m3.bin` | 主机 `linux-firmware` 的 ath11k 目录 | SHA-256 `1921d25edb52f90a7a0acc7275861e14399f904d579aae9f7ab1c4d28533fad6` |
| `ath11k/QCA6390/hw2.0/board.bin` | 由 vendor 分区的 `dby_lx9_bdwlan.bin` 生成 | 见下文 |
| `ath11k/QCA6390/hw2.0/board-2.bin` | 与 `board.bin` 使用同一份板级数据 | 见下文 |

### ath11k 板级数据

原厂设备树的 `huawei_wifi_info` 节点给出 `wifi,pubfile_id = "dby_lx9"`，
对应 `/vendor/etc/wifi/dby_lx9_bdwlan.bin`。该文件为 56180 字节的裸 BDF
负载，SHA-256 为
`91c58e8f8b282a1ac78a8440b035bc1e18f1a9858add29aaed36b04c4103271b`。

`ath11k` 会把非 ELF 的板级数据当作 BIN 类型，随后继续请求 `cal-*.bin`。
QCA6390 没有 caldata，因此需要先把裸负载装入原厂 `bdwlan.e25` 的单段 ELF
容器，再生成 `board-2.bin`。`bdwlan.e25` 位于 modem 分区的
`image/qca6390/` 目录。`tools/host/ath11k-board-data.py` 提供两个子命令：

```bash
python3 tools/host/ath11k-board-data.py wrap \
  --template bdwlan.e25 --payload dby_lx9_bdwlan.bin \
  --output kernel/firmware/ath11k/QCA6390/hw2.0/board.bin

python3 tools/host/ath11k-board-data.py container \
  --name 'bus=pci,qmi-chip-id=0,qmi-board-id=255,variant=Huawei_DBY-W09' \
  --data kernel/firmware/ath11k/QCA6390/hw2.0/board.bin \
  --output kernel/firmware/ath11k/QCA6390/hw2.0/board-2.bin
```

`board-2.bin` 的条目名称需要与设备树的
`qcom,calibration-variant = "Huawei_DBY-W09"` 一致。

## 根文件系统固件

以下文件放在 SD 根文件系统。`scripts/install-rootfs.sh --firmware <dir>`
可以复制一份按目标目录组织的固件树。

| 目标位置 | 来源 | 说明 |
|:--|:--|:--|
| `/lib/firmware/qcom/a650_sqe.fw` | 主机 `linux-firmware` | Adreno 650 SQE |
| `/lib/firmware/qcom/a650_gmu.bin` | 主机 `linux-firmware` 或 vendor 分区 | 两份文件逐字节一致 |
| `/lib/firmware/qcom/sm8250/a650_zap.mdt` | vendor 分区 `/vendor/firmware/` | 签名 zap shader |
| `/lib/firmware/qcom/sm8250/a650_zap.b00` | 同上 | 分片 |
| `/lib/firmware/qcom/sm8250/a650_zap.b01` | 同上 | 分片 |
| `/lib/firmware/qcom/sm8250/a650_zap.b02` | 同上 | 分片 |
| `/lib/firmware/qcom/sm8250/dby-w09/slpi.mdt` | vendor 分区 | SLPI 主固件 |
| `/lib/firmware/qcom/sm8250/dby-w09/slpi.b00..b21` | vendor 分区 | SLPI 分片 |
| `/usr/share/qcom/sm8250/HUAWEI/dby-w09/sensors/` | vendor 分区的 `/vendor/etc/sensors/` 与 `/mnt/vendor/persist/sensors/registry/` | 传感器配置与注册表 |
| `/usr/share/qcom/sm8250/HUAWEI/dby-w09/socinfo` | 符号链接到 `/sys/devices/soc0` | 由服务创建 |

`/usr/share/qcom/sm8250/HUAWEI/dby-w09/sensors/` 需要同时包含：

- `config/`
- `persist/registry/`
- `persist/sns_reg_version`
- `persist/file1`
- `persist/file2`
- `sns_reg.conf`

## 发行版软件包

以下固件由 Arch Linux ARM 的软件包提供，无需手工放置：

| 软件包 | 文件 |
|:--|:--|
| `linux-firmware-atheros` | `/lib/firmware/qca/htbtfw20.tlv`、ath11k 固件 |
| `linux-firmware` | Adreno SQE 与 GMU 固件（以包内容为准） |

`linux-firmware-atheros` 更新后需要复测 WiFi 与蓝牙。
