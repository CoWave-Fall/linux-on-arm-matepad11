#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 把发布仓库中的系统配置安装到已解压 Arch Linux ARM 的 SD 卡根文件系统。
#
# 用法：sudo scripts/install-rootfs.sh --root /mnt/loa-root [选项]
#   --root <dir>         SD 卡根文件系统的挂载点
#   --user <name>        创建桌面账户并加入 wheel 组
#   --ssh-key <file>     把公钥安装到 /root/.ssh/authorized_keys
#   --firmware <dir>     复制固件目录，目录内按 lib/firmware 与 usr/share/qcom 布局
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
target=
user=
ssh_key=
firmware=

usage() {
    # 打印文件头部的注释块，遇到第一行非注释即停止
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --root)
        target=${2:?}
        shift 2
        ;;
    --user)
        user=${2:?}
        shift 2
        ;;
    --ssh-key)
        ssh_key=${2:?}
        shift 2
        ;;
    --firmware)
        firmware=${2:?}
        shift 2
        ;;
    -h | --help) usage ;;
    *)
        echo "未知参数：$1" >&2
        usage
        ;;
    esac
done

[ -n "$target" ] || usage
[ "$(id -u)" -eq 0 ] || {
    echo "需要 root 权限写入 SD 卡" >&2
    exit 1
}
[ -d "$target/etc" ] || {
    echo "$target 不像已解压的根文件系统" >&2
    exit 1
}

echo "== 安装系统配置"
# 只复制 etc 与 usr，仓库内的说明文件留在发布目录。
# --no-preserve=ownership 避免把宿主用户的 uid 带进目标根文件系统；
# 脚本以 root 运行，复制结果归 root:root。
install -d "$target/etc" "$target/usr"
cp -a --no-preserve=ownership "$root/rootfs/etc/." "$target/etc/"
cp -a --no-preserve=ownership "$root/rootfs/usr/." "$target/usr/"

install -Dm755 "$root/tools/device/bt-pair-accessory.sh" \
    "$target/usr/local/libexec/bt-pair-accessory.sh"

chmod 0755 "$target/usr/local/sbin/dby-console.sh"
chmod 0755 "$target/usr/local/libexec/bt-awake.py"
chmod 0755 "$target/usr/local/libexec/mt5727-bt-link.sh"
chmod 0755 "$target/usr/local/libexec/pen-link-keepalive.py"
# sudo 拒绝加载属主不是 root 的 sudoers 文件；显式修正，避免依赖 cp 选项。
chown root:root "$target/etc/sudoers.d/90-wheel"
chmod 0440 "$target/etc/sudoers.d/90-wheel"

if [ -n "$ssh_key" ]; then
    echo "== 安装 root 公钥"
    install -d -m700 "$target/root/.ssh"
    install -m600 "$ssh_key" "$target/root/.ssh/authorized_keys"
fi

if [ -n "$firmware" ]; then
    echo "== 安装固件与传感器文件"
    for dir in "$firmware/lib/firmware" "$firmware/usr/share/qcom"; do
        [ -d "$dir" ] || {
            echo "固件目录缺少 $dir" >&2
            exit 1
        }
    done
    install -d "$target/lib/firmware" "$target/usr/share/qcom"
    cp -a --no-preserve=ownership "$firmware/lib/firmware/." "$target/lib/firmware/"
    cp -a --no-preserve=ownership "$firmware/usr/share/qcom/." "$target/usr/share/qcom/"
fi

if [ -n "$user" ]; then
    if ! grep -q "^$user:" "$target/etc/passwd"; then
        echo "== 创建账户 $user"
        useradd --root "$target" -m -G wheel -s /bin/bash "$user"
    fi
fi

if ! grep -q '^IgnorePkg.*linux-aarch64' "$target/etc/pacman.conf"; then
    echo "== 忽略 linux-aarch64 包"
    sed -i '/^\[options\]/a IgnorePkg = linux-aarch64' "$target/etc/pacman.conf"
fi

# 基础单元在首次启动前启用；桌面与蓝牙单元依赖 pacman 安装的软件包，
# 在设备上按 docs/install.md 的步骤启用。
echo "== 启用基础服务"
for unit in systemd-networkd systemd-resolved sshd dby-console; do
    if [ -e "$target/usr/lib/systemd/system/$unit.service" ]; then
        systemctl --root="$target" enable "$unit.service" >/dev/null
    else
        echo "   跳过 $unit（目标系统尚未安装该单元）"
    fi
done

echo "安装完成：$target"
echo "首次启动前请在设备控制台设置 root 密码；桌面账户密码用 passwd 设置。"
