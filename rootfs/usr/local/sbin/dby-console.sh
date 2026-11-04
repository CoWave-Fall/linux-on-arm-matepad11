#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# DBY-W09 USB ACM 控制台与 ECM 网络服务。
# 在 configfs 中创建 ACM + ECM gadget，然后在 ttyGS0 上运行非交互命令循环。
# 命令循环与 initramfs 阶段保持一致：read 出错时重新打开 tty，
# 主机反复开关端口不会终止会话。

g=/sys/kernel/config/usb_gadget/dby

# configfs 属性文件的 stat 尺寸恒为 PAGE_SIZE，[ -s ] 始终成立，
# 判断是否已绑定 UDC 必须读取文件内容。
udc_bound() {
    [ -n "$(cat "$g/UDC" 2>/dev/null)" ]
}

# 配置里缺少函数链接时才创建；已绑定时 configfs 拒绝删除链接，
# 无条件的 ln -sf 会在每次服务重启时报告 EBUSY。
link_function() {
    [ -L "$g/configs/c.1/$1" ] || ln -s "$g/functions/$1" "$g/configs/c.1/$1"
}

setup_gadget() {
    mkdir -p "$g"
    echo 0x18d1 >"$g/idVendor"
    echo 0x4ee7 >"$g/idProduct"
    mkdir -p "$g/strings/0x409"
    echo "DBY-W09" >"$g/strings/0x409/serialnumber"
    echo "Huawei" >"$g/strings/0x409/manufacturer"
    echo "DBY-W09 Linux console" >"$g/strings/0x409/product"
    mkdir -p "$g/configs/c.1/strings/0x409"
    echo "ACM+ECM" >"$g/configs/c.1/strings/0x409/configuration"

    # 控制台函数
    mkdir -p "$g/functions/acm.usb0"
    link_function acm.usb0

    # 网络函数：ECM 在 Linux 主机上由 cdc_ether 驱动直接支持，
    # 免去 RNDIS 依赖。固定 MAC 便于主机侧识别，也避免每次启动地址变化。
    mkdir -p "$g/functions/ecm.usb0"
    echo 02:11:22:33:44:55 >"$g/functions/ecm.usb0/dev_addr"
    echo 02:11:22:33:44:56 >"$g/functions/ecm.usb0/host_addr"
    link_function ecm.usb0

    # configfs 只允许在绑定 UDC 之前修改配置，因此函数全部就绪后才绑定。
    # 绑定失败必须显式退出：ttyGS0 在函数加入配置时就已存在，
    # 继续执行会让服务看似正常但主机完全枚举不到设备。
    if ! udc_bound; then
        # 取 sysfs 目录中的第一个 UDC；glob 避免解析 ls 输出
        udc=
        for entry in /sys/class/udc/*; do
            [ -e "$entry" ] || continue
            udc=${entry##*/}
            break
        done
        [ -n "$udc" ] || return 1
        echo "$udc" >"$g/UDC" || return 1
    fi
}

# 旧版脚本创建的 gadget 只含 ACM 且已绑定 UDC。
# configfs 拒绝向使用中的配置添加函数，先解绑再统一重建。
if udc_bound && [ ! -d "$g/functions/ecm.usb0" ]; then
    echo "" >"$g/UDC"
fi

setup_gadget || exit 1

# 等待 devtmpfs 建立 ttyGS0 节点
i=0
while [ ! -c /dev/ttyGS0 ] && [ "$i" -lt 40 ]; do
    sleep 0.5
    i=$((i + 1))
done
[ -c /dev/ttyGS0 ] || exit 1

# 主机反复开关端口时不要挂断设备端 tty
stty -hupcl -echo </dev/ttyGS0 2>/dev/null

# 非交互命令循环：保证主机侧 tools/host/usb-console.py 可用
while true; do
    if read -r cmd </dev/ttyGS0; then
        printf '=> %s\n' "$cmd" >/dev/ttyGS0
        /bin/sh -c "$cmd" </dev/null >/dev/ttyGS0 2>&1
    else
        sleep 1
    fi
done
