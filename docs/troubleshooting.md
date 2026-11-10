# 故障排查

更新时间：2026-11-10 23:08 +8

本文按子系统汇总已知故障、原因与处理方式。命令默认在仓库根目录执行，
设备地址为 `root@10.0.0.2`。

## 启动与 U-Boot

### `fastboot boot` 失败

该设备的 ABL 禁止 `fastboot boot`。首次写入使用 ABL fastboot 刷写
`recovery`；后续迭代在 U-Boot 菜单选择 `Enable fastboot mode`，用
`reserved3` 暂存 FIT 后执行 `bootm`。

### 内核停在启动菜单或反复重启

检查 `bootargs` 是否写入 U-Boot 环境。FIT 配置中的 `bootargs` 生效条件
有限，统一使用：

```bash
fastboot oem run:"setenv bootargs 'console=tty0 loglevel=4 panic=10 \
clk_ignore_unused pd_ignore_unused root=PARTLABEL=archroot \
rw rootwait rootfstype=f2fs'"
```

`root=PARTLABEL=archroot` 与 SD 卡的 GPT 分区标号一致。分区标号缺失时
内核停在 `VFS: Unable to mount root fs`。

### 启动卡死

启动命令行加入 `iommu.passthrough=1` 会让设备卡死。该参数从所有启动
脚本中移除。

### 写入 `/dev/mem` 后 panic

`no-map` 保留区由固件与显示链路占用，向该区域写入会触发翻译异常。
诊断时改用设备树节点或驱动接口。

### 驱动停在 `-EPROBE_DEFER`

镜像只包含 `Image` 与设备树，没有模块目录。用到的驱动需要编入内核
（`=y`）。常见对象：

- `CONFIG_QCOM_OCMEM` 会把 `DRM_MSM` 限制为 `m`，改为未设置。
- `CONFIG_INTERCONNECT_QCOM_OSM_L3` 需要为 `y`。
- `CONFIG_POWER_RESET_QCOM_PON` 需要为 `y`。
- `CONFIG_REGULATOR_QCOM_REFGEN` 需要为 `y`。

### U-Boot 菜单记录读取失败

`fastboot oem console` 会被 DWC3 的 `request ... was not queued to
ep1in-bulk` 日志填满，读取返回错误。该日志对 fastboot 传输没有影响，
调试信息以屏幕与串口为准。

### `fastboot oem run` 参数被拆分

整条 U-Boot 命令需要作为单个参数传入：

```bash
fastboot oem run:'run boot_harmonyos'
```

## 显示

### 冷启动黑屏

首次 `prepare` 时面板供电由引导阶段保持，regulator 记录的状态与硬件
存在差异。驱动的 `power_cycle_on_boot` 会先执行一次真实断电上电。
修改面板驱动后确认该标志仍然存在。

### 右半屏黑屏

从 PHY 的位时钟需要由本地 PLL 产生。确认 `&mdss_dsi1_phy` 带有
`qcom,dsi-phy-own-pll`，并确认 `dsi_phy_7nm.c` 的补丁已应用。

### 右半屏稀疏短横线

SDR104 下偶发出现，部分启动完全正常。可对照以下方案：

- 把 SD 限速到 SDR50；
- 让 DSI1 控制器与 PHY1 的 PLL 同源；
- 补齐主 PHY 向从 PHY 输出位时钟的通路。

3.3 V 高速模式会让短横线消失，同时引入约 5 Hz 周期性闪烁并导致触摸
AFE 无法启动，当前配置保持 SDR104。

### `PLL lock failed`

`CONFIG_REGULATOR_QCOM_REFGEN` 需要为 `y`。该驱动提供 DSI 控制器的
`refgen` 供电，模块方式在无模块目录的镜像中无法注册。

### 面板停在 deferred probe

`regulator-fixed` 节点的电压上下限需要相等。上下限存在差异时
`reg-fixed-voltage` 以 `-EINVAL` 拒绝注册。检查
`vreg_panel_iovcc`、`vreg_panel_vsp` 与 `vreg_panel_vsn`。

## 触摸

### `irq: type mismatch`

触摸中断需要配置为下降沿：

```dts
interrupts = <39 IRQ_TYPE_EDGE_FALLING>;
```

其他触发类型会让 `spi->irq` 取值为 0，probe 失败。

### 触摸无响应或事件风暴

显示链路初始化完成后 AFE 才会启动。确认：

```bash
dmesg | grep -i 'AFE'
cat /sys/module/novatek_n9z/parameters/pen_sync_count
```

重新绑定驱动可以快速恢复：

```bash
echo spi14.0 > /sys/bus/spi/drivers/novatek-n9z/unbind
echo spi14.0 > /sys/bus/spi/drivers/novatek-n9z/bind
```

连续无效帧会让驱动重载固件。显示侧只出半屏时，AFE 会停在未启动状态，
先处理显示问题。

### 上电校准暂态

约 10% 的亮屏周期出现约 4 秒的整帧偏移，期间没有触点上报。驱动等待
固件自行恢复，连续 10 秒未恢复才重载。该现象与显示初始化有关。

## 存储

### SD 卡识别

卡检测引脚 gpio77 的电平无法区分插卡状态，Linux 侧使用 `broken-cd`
轮询。U-Boot 侧使用低电平有效的 `cd-gpios`。

### SD 频率

`max-frequency` 取 `202000000`。写成 `201500000` 会让
`clk_rcg2_floor_ops` 落到 100 MHz 档，DLL 无法锁定。

### U-Boot 块号

U-Boot 的 `mmc write <addr> <blk#> <cnt>` 按十六进制解析块号与数量。
`34` 表示 `0x34`，写入前先换算。

### `cache` 与 `log` 分区

两者由 HarmonyOS 挂载。向 `cache` 写入 FIT 会让原厂系统在
`enablefilecrypto` 阶段反复重启。恢复：

```bash
mkfs.ext4 -F -L cache -O ^orphan_file,^metadata_csum_seed \
  /dev/disk/by-partlabel/cache
dd if=/dev/zero of=/dev/disk/by-partlabel/misc bs=1M count=2 conv=fsync
sync
reboot
```

## USB

### 主机打开 ACM 端口报 `EAGAIN` 或 `EIO`

偶发出现，重试即可。`tools/host/usb-console.py` 已包含重试逻辑。

### UDC 绑定状态判断

configfs 属性文件的 `stat()` 尺寸恒为 PAGE_SIZE，`[ -s ]` 始终成立。
判断绑定状态需要读取 `UDC` 文件内容。

### `bootm` 后 fastboot 报错

内核接管 USB 控制器后 fastboot 会话结束，主机侧报
`usb_read failed: No such device`。该报错属于正常交接，等待 SSH 恢复。

### 解包 Arch 包后 `ping` 缺少 capability

`GNU tar` 解包会丢失 `security.capability`。重新安装对应包或执行：

```bash
setcap cap_net_raw+p /usr/bin/ping
```

## 电源与按键

### 电源键或音量下键缺失

`CONFIG_POWER_RESET_QCOM_PON` 需要为 `y`。该驱动在 probe 时创建
`pwrkey` 与 `resin` 子设备。

### 音量加键读数漂移

U-Boot 的 PMIC GPIO 在输入模式下会写入「无上拉」。确认
`qcom_pmic_gpio.c` 的 bias 配置补丁与 `button-gpio.c` 的 pinctrl 补丁
已应用。

### 启动菜单刚显示就被确认

`button_kbd.c` 需要以启动时的按键状态作为 `old_state` 初值。补丁缺失
时，用于打开菜单的按住会被当成 Enter。

### 重启 logind 后屏幕停在 tty

重启 `systemd-logind` 会让运行中的 KWin 丢失 DRM master。部署脚本省略
自动重启 logind；drop-in 内容变化后重启系统。

## 蓝牙与 WiFi

### 冷启动没有 `hci0`

`hci_qca` 在根文件系统挂载前 probe，固件读取失败。确认
`qca-bt-rebind.service` 已启用并在 `local-fs.target` 之后执行。

### 键盘输入节点缺失

华为磁吸键盘的 CCC 描述符拒绝读取。部署补丁版 `bluetoothd`，并确认
`bluetooth.service` 的 `ExecStart` 指向 `/usr/local/libexec/bluetooth/bluetoothd`。

### 笔在扫描中无法发现

`FilterDiscoverable` 需要设为 `false`。确认
`/etc/bluetooth/loa-main.conf` 与 `bluetooth.service.d/override.conf`
已安装。

### 笔连接后约 8 秒断开

笔在等待厂商通知特征 `fe42` 的订阅。确认 `pen-link-keepalive.service`
运行，并在 `bluetoothd` 重启后同步重启该服务。

### `wlan0` 命名失败

`.link` 文件使用 `Type=wlan` 与 `Driver=ath11k_pci` 匹配。使用
`Path=pci-0000:01:00.0` 时，PCIe 父设备派生的接口会落到默认命名，
`wpa_supplicant@wlan0` 等待约 90 秒后超时。

### ath11k probe 失败

板级数据需要为单段 ELF 容器。裸 BDF 会被当作 BIN 类型，随后请求
`cal-*.bin`。按 [固件清单](firmware.md) 生成 `board.bin` 与 `board-2.bin`。

### WiFi MAC 每次变化

WLAN MAC 由固件生成。依赖 MAC 白名单的网络需要在 NetworkManager 中
固定克隆地址，或在网络侧放开限制。

## 传感器

### 传感器未注册

确认：

```bash
systemctl status dby-slpi-firmware hexagonrpcd-sdsp
ls /dev/fastrpc-sdsp
```

`dby-slpi-firmware.service` 在根文件系统就绪后重绑 `5c00000.remoteproc`。
`hexagonrpcd-sdsp.service` 等待 `/dev/fastrpc-sdsp` 后启动，并在就绪后
重启 `iio-sensor-proxy`。

### 自动旋转方向相反

注册表的 `t1000_0_platform.placement` 需要为对角线 `-1, -1, 1`。
`rootfs/` 省略该文件，按 [固件清单](firmware.md) 从原厂注册表取得。

### 环境色温服务缺失

确认 CCT 版 libssc 位于 `/usr/local/loa-libssc`，`loa-alsd.service`
随图形会话启动，KWin 效果已启用。

## 桌面

### 登录界面没有屏幕键盘

`plasmalogin` 需要以 `--inputmethod plasma-keyboard` 启动 Greeter。
确认 `org.kde.kwin.VirtualKeyboard` 的 `available` 为 `true`。

### KCM 条目缺失

systemsettings 6.8 会按设备形态过滤标注为 `tablet` 的 KCM。元数据省略
`FormFactors` 字段。确认 `kcm_loa_tablet.so` 位于
`/usr/lib/qt6/plugins/plasma/kcms/systemsettings/`。

### 手动构建的文件属主异常

用 `cp -a` 把手工文件安装到 `/usr` 会保留源属主。根目录属主异常会让
`systemd-tmpfiles-setup` 跳过条目并以 73 退出，Xwayland 与 `DISPLAY`
随之缺失。核对：

```bash
find / -xdev -user <构建用户> -not -path '/home/*'
```

把相关目录改回 `root:root`，根目录权限恢复 755，再执行
`systemd-tmpfiles --create` 并重新登录。

### 休眠恢复后设备复位

启动期恢复尚未通过。镜像读取完成后设备 panic，下一次启动报告
`PM: Image not found`。在 `PMSG_QUIESCE` 阶段保持显示链路点亮可以
获取恢复阶段日志，该工作尚未完成。

## 回滚

任何阶段都可以回到原厂系统：

```bash
fastboot flash recovery dumps/recovery.img
fastboot flash recovery_vbmeta dumps/recovery_vbmeta.img
fastboot flash misc dumps/misc.img
fastboot reboot
```

ABL fastboot 无法进入时使用 9008 EDL 与 `dumps/` 中的分区镜像。EDL 的
programmer 与客户端按设备备份资料准备。
