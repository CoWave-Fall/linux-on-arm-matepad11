# 操作全流程

更新时间：2026-11-10 23:52 +8

本文从已解锁并完成备份的设备开始，依次完成 U-Boot 构建、recovery 刷写、
SD 卡根文件系统、内核启动、系统配置与桌面安装。全程保留 HarmonyOS 的
启动能力。

阅读前请确认：

- 设备型号为 `DBY-W09`，原厂系统为 HarmonyOS 4.2。
- Bootloader 已解锁，设备已取得 Root 权限。
- `recovery`、`recovery_vbmeta`、`misc`、`recovery_ramdisk`、`boot`、
  `super`、`persist`、`modemst1`、`modemst2`、`fsg`、`oeminfo`、`secdata`
  已备份，并逐项核对 SHA-256。
- 主机可执行 `adb`、`fastboot` 与 `aarch64-linux-gnu-` 交叉工具链。
- microSD 卡容量至少 32 GB。

流程会写入 `recovery` 分区。写入前请再次确认备份可用；任何阶段都可以
经 fastboot 或 9008 EDL 回滚。

## 1. 主机准备

Arch Linux 与 CachyOS 的主机安装以下软件包：

```bash
sudo pacman -S --needed base-devel git curl xz bzip2 bc bison flex \
  aarch64-linux-gnu-gcc swig openssl python python-setuptools \
  dtc uboot-tools android-tools gptfdisk f2fs-tools iptables shellcheck ruff
```

Debian 与 Ubuntu 的主机安装对应包：

```bash
sudo apt install build-essential git curl xz-utils bzip2 bc bison flex \
  gcc-aarch64-linux-gnu libssl-dev swig python3 python3-setuptools \
  device-tree-compiler u-boot-tools android-tools-adb \
  android-tools-fastboot gdisk f2fs-tools iptables shellcheck
```

`mkbootimg` 与 `avbtool` 已收录在 `tools/vendor/`，主机无需额外安装。

## 2. 获取仓库与上游源码

```bash
git clone https://github.com/CoWave-Fall/linux-on-arm-matepad11.git
cd linux-on-arm-matepad11
scripts/fetch-sources.sh
```

脚本下载 Linux 7.3.1 与 U-Boot 2026.10 到 `build/`，并用 SHA-256 校验。
主机没有外网时，可以手动把两个源码包放入 `build/`，脚本会跳过下载。

## 3. 准备固件

按 [固件清单](firmware.md) 把厂商文件放入 `kernel/firmware/`。构建内核前
至少需要：

```
kernel/firmware/novatek/NT36523_THP1300.bin
kernel/firmware/ath11k/QCA6390/hw2.0/amss.bin
kernel/firmware/ath11k/QCA6390/hw2.0/m3.bin
kernel/firmware/ath11k/QCA6390/hw2.0/board.bin
kernel/firmware/ath11k/QCA6390/hw2.0/board-2.bin
```

触摸固件的 MD5 应为 `777df0f42cbbe0628ce22b78c98a8cf1`。WiFi 板级数据
由 `tools/host/ath11k-board-data.py` 从原厂 `dby_lx9_bdwlan.bin` 生成。

根文件系统侧的 GPU、SLPI 与传感器文件按 [固件清单](firmware.md) 组织为
一个目录，例如 `firmware-root/`，其中包含 `lib/firmware/` 与
`usr/share/qcom/` 两棵子树。

## 4. 构建 U-Boot

```bash
scripts/build-uboot.sh
```

脚本把 `uboot/` 下的 defconfig 放入 U-Boot 源码树，应用
`uboot/patches/` 中的五个补丁（设备树与默认环境由补丁创建），然后编译。
输出为 `out/u-boot.bin`。

重新构建：

```bash
scripts/build-uboot.sh --clean
```

## 5. 生成 recovery 镜像

把备份中的 `recovery.img` 放到仓库的 `dumps/` 目录，然后执行：

```bash
scripts/pack-recovery.sh dumps/recovery.img
```

脚本用原厂 recovery 镜像中的 ramdisk、dtb 与 `recovery_dtbo` 重新封装
U-Boot，并生成 `out/recovery.img` 与 `out/recovery_vbmeta.img`。
`recovery_vbmeta` 的 flags 为 2，关闭 recovery 的哈希校验。

原厂镜像缺失时，流程无法继续。请从备份中恢复 `recovery.img` 后重试。

## 6. 刷写 recovery

设备关机，进入 ABL fastboot：

- 按住音量下键，连接 USB 线，再短按电源键；或
- 长按电源键约 30 秒，松开后同时按住电源键与音量下键。

主机执行：

```bash
fastboot devices
fastboot flash recovery out/recovery.img
fastboot flash recovery_vbmeta out/recovery_vbmeta.img
fastboot reboot
```

设备重启后出现 U-Boot 启动菜单。菜单使用音量下键移动，电源键确认。

刷写失败或菜单未出现时，回到 ABL fastboot 执行回滚：

```bash
fastboot flash recovery dumps/recovery.img
fastboot flash recovery_vbmeta dumps/recovery_vbmeta.img
fastboot flash misc dumps/misc.img
fastboot reboot
```

ABL fastboot 也无法进入时，使用 9008 EDL 与 `dumps/` 中的分区镜像恢复。

## 7. 准备 SD 卡

确认卡设备名：

```bash
lsblk -o NAME,SIZE,TYPE,MODEL
```

以下命令以 `/dev/sdX` 为例。请替换为实际设备，写错会清除该磁盘的数据。

```bash
sudo sgdisk --clear --new=1:0:0 --typecode=1:8300 \
  --change-name=1:archroot /dev/sdX
sudo mkfs.f2fs -l archroot /dev/sdX1
```

分区标号 `archroot` 与内核启动参数 `root=PARTLABEL=archroot` 对应。

下载 Arch Linux ARM 根文件系统并解压：

```bash
curl -fLO http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz
sudo mkdir -p /mnt/loa-root
sudo mount /dev/sdX1 /mnt/loa-root
sudo tar -C /mnt/loa-root -xpf ArchLinuxARM-aarch64-latest.tar.gz
```

安装发布仓库中的配置：

```bash
sudo scripts/install-rootfs.sh --root /mnt/loa-root \
  --user <桌面用户名> \
  --firmware firmware-root \
  --ssh-key ~/.ssh/id_ed25519.pub
```

脚本完成以下操作：

- 复制 `rootfs/` 中的 systemd、udev、NetworkManager、SSH 与蓝牙配置；
- 创建桌面账户并加入 `wheel` 组；
- 安装 root 的 SSH 公钥（建议提供；未提供时只有 USB 主机 `10.0.0.1`
  可以密码登录）；
- 复制 `firmware-root/` 中的固件与传感器文件；
- 在 `pacman.conf` 中加入 `IgnorePkg = linux-aarch64`；
- 启用 `systemd-networkd`、`systemd-resolved`、`sshd` 与 `dby-console`。

卸载前执行 `sync`：

```bash
sync
sudo umount /mnt/loa-root
```

## 8. 构建内核与 FIT

```bash
scripts/build-kernel.sh
scripts/pack-fit.sh
```

输出：

```
out/Image.gz
out/dby-w09.dtb
out/dby-w09.itb
```

## 9. 首次启动 Linux

把 SD 卡插入设备，开机进入 U-Boot 菜单。选择 `Enable fastboot mode`
（音量下键移动两次，电源键确认）。主机确认 fastboot 设备后执行：

```bash
tools/host/boot-linux.sh out/dby-w09.itb
```

脚本把 FIT 写入 `reserved3`，设置 `bootargs`，读回内存并执行 `bootm`。
内核接管 USB 控制器后主机侧的 fastboot 会话结束，该报错属于正常现象。
设备通常在 20 至 40 秒内恢复 SSH。

面板在显示初始化完成前保持 U-Boot 画面，随后切换到内核控制台与登录界面。

## 10. 主机 USB 网络

安装主机侧的固定命名与地址配置：

```bash
sudo install -m644 tools/host/10-loa-usb0.link /etc/systemd/network/
sudo install -m600 -o root -g root tools/host/loa-usb0.nmconnection \
  /etc/NetworkManager/system-connections/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=net --action=add
sudo nmcli connection reload
sudo nmcli connection up loa-usb0
```

验证：

```bash
ip -4 addr show loa-usb0
ping -c 3 10.0.0.2
ssh root@10.0.0.2
```

USB ACM 控制台用于 SSH 无法建立时排查：

```bash
tools/host/usb-console.py --wait 5 'uname -a'
```

## 11. 系统基础配置

通过 SSH 登录设备：

```bash
ssh root@10.0.0.2
```

SSH 默认只接受公钥。USB 主机 `10.0.0.1` 是唯一允许 root 密码登录的来源，
首次部署可直接使用 Arch Linux ARM 的初始密码 `root`。安装了第 7 步的公钥时，
WiFi 等其他来源也使用密钥登录。首次登录后立即修改 root 密码。

设置 root 密码、时区与 locale：

```bash
passwd
timedatectl set-timezone Asia/Shanghai
sed -i 's/^#zh_CN.UTF-8/zh_CN.UTF-8/' /etc/locale.gen
locale-gen
localectl set-locale LANG=zh_CN.UTF-8
```

设置桌面账户密码：

```bash
passwd <桌面用户名>
```

### 首次联网

方式一：主机通过 USB 链路提供 NAT。

```bash
sudo scripts/host-usb-nat.sh up
```

设备侧执行：

```bash
ip route add default via 10.0.0.1
echo 'nameserver 1.1.1.1' > /etc/resolv.conf
```

方式二：设备侧直接连接 WiFi。

```bash
iwctl station wlan0 connect <SSID>
```

`iwd` 缺失时使用 `wpa_supplicant` 与 `dhcpcd`，或先按方式一取得网络后
安装 `iwd`。

验证外网：

```bash
ping -c 3 1.1.1.1
pacman -Syu
```

## 12. 安装软件包

```bash
pacman -S --needed plasma-desktop plasma-workspace kwin plasma-integration \
  systemsettings kinfocenter kscreen powerdevil polkit-kde-agent \
  plasma-keyboard plasma-systemmonitor bluedevil breeze breeze-gtk \
  xdg-desktop-portal xdg-desktop-portal-kde noto-fonts-cjk kwallet-pam \
  plasma-login-manager networkmanager plasma-nm spectacle \
  bluez bluez-utils iio-sensor-proxy power-profiles-daemon upower \
  meson ninja cmake extra-cmake-modules gcc pkgconf python-gobject \
  glib2 json-c git patch evtest linux-firmware-atheros
```

需要中文衬线字体时，另装霞鹜文楷屏幕阅读版：

```bash
install -Dm644 LXGWWenKaiScreen.ttf \
  /usr/share/fonts/TTF/LXGWWenKaiScreen.ttf
fc-cache -f
```

字体文件可从 `lxgw/LxgwWenKai-Screen` 的 release 取得。

## 13. 部署设备侧程序

在主机编译静态程序：

```bash
scripts/build-device-tools.sh
```

推送并安装：

```bash
scp out/device/dby-powerkeyd root@10.0.0.2:/usr/local/sbin/
scp out/device/loa-pen root@10.0.0.2:/usr/local/libexec/
ssh root@10.0.0.2 'chmod 755 /usr/local/sbin/dby-powerkeyd \
  /usr/local/libexec/loa-pen'
```

把发布目录中的 `tools/` 复制到设备，后续在设备上应用上游补丁与运行
安装脚本时使用：

```bash
ssh root@10.0.0.2 'install -d -m755 /usr/local/share/loa-tools'
scp -r tools/. root@10.0.0.2:/usr/local/share/loa-tools/
```

`scp -r tools/.` 重复执行仍然保持单层目录结构，桌面用户也能读取该位置。

Python 服务已由 `scripts/install-rootfs.sh` 安装到
`/usr/local/libexec/`。启动这些服务：

```bash
ssh root@10.0.0.2 'systemctl daemon-reload &&
  systemctl enable --now dby-powerkey loa-pen bt-awake \
    mt5727-bt-link pen-link-keepalive'
```

### 平板设置

```bash
ssh root@10.0.0.2 \
  /usr/local/share/loa-tools/device/loa-tablet/install.sh <桌面用户名>
```

安装脚本在设备上编译 KCM，需要 `cmake`、`ninja` 与
`extra-cmake-modules`。

## 14. 部署上游组件

### BlueZ HOGP 补丁

设备的 BlueZ 版本为 5.87。带补丁的 `bluetoothd` 安装在
`/usr/local/libexec/bluetooth/`，系统包提供的文件保持原样。

在设备上构建：

```bash
pacman -S --needed base-devel
curl -LO https://www.kernel.org/pub/linux/bluetooth/bluez-5.87.tar.xz
tar -xf bluez-5.87.tar.xz
cd bluez-5.87
patch -p1 < /usr/local/share/loa-tools/upstream/bluez/0001-hog-ccc-read-failure.patch
./configure --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
  --runstatedir=/run --libexecdir=/usr/lib --libdir=/usr/lib \
  --disable-tools --disable-monitor --disable-cups --disable-obex \
  --with-udevdir=/usr/lib/udev \
  --with-dbusconfdir=/usr/share/dbus-1 \
  --with-dbussystembusdir=/usr/share/dbus-1/system-services \
  --with-dbussessionbusdir=/usr/share/dbus-1/services \
  --with-systemdsystemunitdir=/usr/lib/systemd/system
make -j"$(nproc)"
install -Dm755 src/bluetoothd /usr/local/libexec/bluetooth/bluetoothd
systemctl restart bluetooth
```

`rootfs/` 中的 `bluetooth.service.d/override.conf` 已指向该文件，并加载
`/etc/bluetooth/loa-main.conf`。该配置把 `FilterDiscoverable` 设为
`false`，用于发现 M-Pencil 3。

### hexagonrpcd

传感器需要带八个补丁的 `hexagonrpcd`。按
`tools/upstream/hexagonrpc/README.md` 在主机交叉编译，然后部署：

```bash
scp hexagonrpcd root@10.0.0.2:/usr/bin/
ssh root@10.0.0.2 'chmod 755 /usr/bin/hexagonrpcd &&
  systemctl enable --now dby-slpi-firmware hexagonrpcd-sdsp'
```

### libssc CCT 补丁

环境色温需要带 CCT 的 libssc，安装在独立前缀 `/usr/local/loa-libssc`：

```bash
curl -LO https://codeberg.org/DylanVanAssche/libssc/archive/v0.4.4.tar.gz
tar -xf v0.4.4.tar.gz
cd libssc
patch -p1 < /usr/local/share/loa-tools/upstream/libssc/0001-light-expose-cct.patch
meson setup build --prefix=/usr/local/loa-libssc --libexecdir=lib
meson compile -C build
meson install -C build
```

验证：

```bash
LD_LIBRARY_PATH=/usr/local/loa-libssc/lib \
  /usr/local/loa-libssc/bin/ssccli --sensor light --timeout 3
```

输出应包含照度与 `Light sensor CCT` 两行。

### power-profiles-daemon 的 qcom 驱动

先在设备上取得对应版本的源码树：

```bash
git clone https://gitlab.freedesktop.org/upower/power-profiles-daemon.git \
  /root/ppd-src
git -C /root/ppd-src checkout 1a151cc
```

然后在主机执行：

```bash
tools/host/ppd-qcom-deploy.sh
```

脚本把驱动源码与 systemd drop-in 送到设备，在设备上编译并安装到
`/usr/local/libexec/loa/power-profiles-daemon`。pacman hook 会在软件包
更新后重新编译。

## 15. 桌面会话与平板配置

启用登录管理器：

```bash
systemctl enable plasmalogin
```

登录界面使用 Plasma Keyboard 输入密码。取消自动登录时，确认
`/etc/plasmalogin.conf.d/` 中没有自动登录配置。

以桌面用户身份在设备上执行平板配置：

```bash
/usr/local/share/loa-tools/device/plasma-tablet-setup.sh
```

脚本写入虚拟键盘、触摸边缘手势、旋转快捷方式与边缘触发宽度。

### 环境色温

安装 libssc CCT 后，在设备上执行：

```bash
ssh root@10.0.0.2 \
  /usr/local/share/loa-tools/device/loa-alsd/install.sh <桌面用户名>
ssh root@10.0.0.2 \
  /usr/local/share/loa-tools/device/kwin-ambient-color/install.sh
```

启用 KWin 效果：

```bash
kwriteconfig6 --file kwinrc --group Plugins --key ambientcolorEnabled true
gdbus call --session --dest org.kde.KWin --object-path /KWin \
  --method org.kde.KWin.reconfigure
```

### 自动旋转、自动亮度与按键

`loa-tabletd` 与 `loa-tablet-notify` 由平板设置的安装脚本启用。命令行：

```bash
loa-tabletctl state
loa-tabletctl auto-rotate on
loa-tabletctl auto-brightness on
loa-tabletctl rotate normal
loa-tabletctl key-bindings
```

### 蓝牙配对

键盘与笔从零配对：

```bash
/usr/local/libexec/bt-pair-accessory.sh <蓝牙地址>
```

笔的地址可在 `bluetoothctl devices` 或原厂系统的蓝牙设置中读取。配对后
`pen-link-keepalive` 会自动订阅厂商通知特征。

## 16. 启用服务

```bash
systemctl enable --now systemd-networkd systemd-resolved sshd NetworkManager \
  bluetooth qca-bt-rebind dby-powerkey dby-slpi-firmware hexagonrpcd-sdsp \
  loa-pen bt-awake mt5727-bt-link pen-link-keepalive power-profiles-daemon \
  plasmalogin
```

检查失败的单元：

```bash
systemctl --failed
```

## 17. 验收

```bash
uname -a
cat /sys/class/drm/card0-DSI-1/status
cat /sys/class/backlight/rt8555/max_brightness
cat /sys/class/power_supply/battery/capacity
ip -4 addr show wlan0
bluetoothctl show
monitor-sensor
kscreen-doctor -o
```

预期结果：

| 项目 | 预期 |
|:--|:--|
| `uname -r` | `7.3.1-loa` |
| DSI 状态 | `connected` |
| 背光上限 | `1023` |
| 电池容量 | `0..100` |
| `wlan0` | 已分配地址 |
| 蓝牙控制器 | `Powered: yes` |
| 传感器 | 加速度计、环境光、指南针 |
| 输出 | `DSI-1`，1600 x 2560 |

触摸与笔：

```bash
evtest /dev/input/event<编号>
```

多指、双指、手掌平按与笔的悬浮书写都应产生连续事件。

## 18. 双系统切换与回滚

### 切换到 HarmonyOS

```bash
systemctl reboot
```

在 U-Boot 菜单选择 `Boot HarmonyOS`。菜单会清空 `misc` 的 `command`
字段，ABL 随即启动原厂系统。

### 返回 Linux

在 HarmonyOS 侧执行：

```bash
adb reboot recovery
```

U-Boot 菜单出现后，10 秒无操作会启动 `reserved3` 中的 Linux。

### 回滚 U-Boot

```bash
fastboot flash recovery dumps/recovery.img
fastboot flash recovery_vbmeta dumps/recovery_vbmeta.img
fastboot flash misc dumps/misc.img
fastboot reboot
```

### 分区约束

`cache` 与 `log` 由 HarmonyOS 挂载，FIT 暂存统一使用 `reserved3`。
向 `cache` 写入非 ext4 数据会让原厂系统在 `enablefilecrypto` 阶段反复
重启。恢复方式：

```bash
mkfs.ext4 -F -L cache -O ^orphan_file,^metadata_csum_seed \
  /dev/disk/by-partlabel/cache
dd if=/dev/zero of=/dev/disk/by-partlabel/misc bs=1M count=2 conv=fsync
sync
reboot
```

## 19. 更新与维护

### 更新内核

```bash
scripts/build-kernel.sh
scripts/pack-fit.sh
```

设备重启进入 U-Boot fastboot，主机执行：

```bash
tools/host/boot-linux.sh out/dby-w09.itb
```

### 更新 U-Boot

重新执行 `scripts/build-uboot.sh` 与 `scripts/pack-recovery.sh`，在 U-Boot
fastboot 下写入 `recovery`。`recovery_vbmeta` 保持原样。

### 更新根文件系统配置

重新挂载 SD 卡并执行：

```bash
sudo scripts/install-rootfs.sh --root /mnt/loa-root --user <桌面用户名>
```

### 软件包更新

```bash
pacman -Syu
```

`power-profiles-daemon` 更新后由 pacman hook 自动重建。BlueZ 更新会覆盖
`/usr/lib/bluetooth/bluetoothd`，`/usr/local/libexec/bluetooth/bluetoothd`
保持原样；大版本更新后建议重新构建补丁版。

`linux-firmware-atheros` 更新后复测 WiFi 与蓝牙。
