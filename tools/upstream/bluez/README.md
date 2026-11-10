# BlueZ HOGP 补丁

华为磁吸键盘的 HID over GATT 服务中，CCC 描述符拒绝读取。BlueZ 5.87 的
`profiles/input/hog-lib.c: ccc_read_cb()` 在读取失败后直接放弃订阅，导致
`uhid` 输入设备无法创建。补丁在读取失败时仍然写入通知使能并继续处理。

## 版本

- BlueZ 5.87
- 补丁：`0001-hog-ccc-read-failure.patch`

## 在设备上构建

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
```

## 在主机上交叉编译

`build.sh` 使用 `SRC` 与 `SYS` 两个环境变量。`SYS` 为设备的 aarch64
sysroot，需要包含 `usr/include`、`usr/lib`、pkg-config 文件与
`lib -> usr/lib` 符号链接。

```bash
SRC=$PWD/bluez-5.87 SYS=$PWD/arm64-sysroot tools/upstream/bluez/build.sh
```

## 部署

```bash
install -Dm755 bluetoothd /usr/local/libexec/bluetooth/bluetoothd
systemctl daemon-reload
systemctl restart bluetooth
```

`rootfs/etc/systemd/system/bluetooth.service.d/override.conf` 已指向
`/usr/local/libexec/bluetooth/bluetoothd`，并加载
`/etc/bluetooth/loa-main.conf`。

## 验证

重启后确认键盘自动回连：

```bash
bluetoothctl devices
bluetoothctl info <键盘地址>
cat /proc/bus/input/devices
```

`hid-generic` 应创建键盘输入节点，按键事件正常上报。

## 来源与许可证

源码为 BlueZ 5.87，上游项目地址
`https://www.kernel.org/pub/linux/bluetooth/`。BlueZ 采用
`GPL-2.0-or-later`，本目录的补丁沿用该许可证。
