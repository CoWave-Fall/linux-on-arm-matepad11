#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# 交叉编译 BlueZ 5.87，产出带 HOGP 补丁的 bluetoothd。
# 依赖与部署步骤见同目录 README.md。
set -eu

: "${SRC:?请设置 SRC 为 bluez-5.87 源码目录}"
: "${SYS:?请设置 SYS 为 aarch64 sysroot 目录}"

cd "$SRC"

if [ ! -f config.status ]; then
    PKG_CONFIG_LIBDIR="$SYS/usr/lib/pkgconfig:$SYS/usr/share/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$SYS" \
        CPPFLAGS="-I$SYS/usr/include" \
        CFLAGS="--sysroot=$SYS -I$SYS/usr/include -O2" \
        LDFLAGS="--sysroot=$SYS -L$SYS/usr/lib -L$SYS/lib" \
        ./configure --host=aarch64-linux-gnu --build=x86_64-pc-linux-gnu \
        --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --runstatedir=/run --libexecdir=/usr/lib --libdir=/usr/lib \
        --disable-tools --disable-monitor --disable-cups --disable-obex \
        --with-udevdir=/usr/lib/udev \
        --with-dbusconfdir=/usr/share/dbus-1 \
        --with-dbussystembusdir=/usr/share/dbus-1/system-services \
        --with-dbussessionbusdir=/usr/share/dbus-1/services \
        --with-systemdsystemunitdir=/usr/lib/systemd/system
fi

make -j"$(nproc)"
aarch64-linux-gnu-strip src/bluetoothd -o bluetoothd
