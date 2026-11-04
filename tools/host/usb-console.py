#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""通过设备的 USB ACM 串口执行命令并取回输出。

设备侧的 dby-console 服务建立了 gadget，主机侧出现 /dev/ttyACM*。
该脚本以 raw 模式打开串口，发送命令后收集一段时间的输出。

用法：
    tools/host/usb-console.py "dmesg | tail -30"
    tools/host/usb-console.py --read 5            # 只读取 5 秒
"""

import argparse
import errno
import os
import select
import sys
import termios
import time


def open_port(port):
    # 设备刚枚举或控制台服务重启时，主机可能短暂返回 EAGAIN 或 EIO
    for attempt in range(20):
        try:
            fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            break
        except OSError as error:
            if error.errno not in (errno.EAGAIN, errno.EIO) or attempt == 19:
                raise
            time.sleep(0.2)
    attrs = termios.tcgetattr(fd)
    # raw 模式：不做行缓冲与回显转换
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
    iflag &= ~(
        termios.IGNBRK
        | termios.BRKINT
        | termios.PARMRK
        | termios.ISTRIP
        | termios.INLCR
        | termios.IGNCR
        | termios.ICRNL
        | termios.IXON
    )
    oflag &= ~termios.OPOST
    lflag &= ~(
        termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN
    )
    cflag &= ~(termios.CSIZE | termios.PARENB)
    cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    termios.tcsetattr(
        fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    )
    return fd


def write_all(fd, data):
    """非阻塞串口可能短写，循环写出全部数据。"""
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written <= 0:
            raise OSError("串口写入失败")
        view = view[written:]


def drain(fd, seconds):
    """收集指定时长内的全部输出。"""
    chunks = []
    deadline = time.monotonic() + seconds
    while True:
        remain = deadline - time.monotonic()
        if remain <= 0:
            break
        ready, _, _ = select.select([fd], [], [], remain)
        if not ready:
            break
        try:
            data = os.read(fd, 4096)
        except BlockingIOError:
            continue
        if not data:
            break
        chunks.append(data)
    return b"".join(chunks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", nargs="?", help="要执行的命令")
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--read", type=float, default=None, help="只读取指定秒数")
    parser.add_argument("--wait", type=float, default=3.0, help="发送后收集输出的秒数")
    args = parser.parse_args()

    fd = open_port(args.port)
    try:
        # 丢弃接入前积压的数据
        drain(fd, 0.3)

        if args.read is not None:
            sys.stdout.write(drain(fd, args.read).decode("utf-8", "replace"))
            return 0

        if not args.command:
            parser.error("需要命令或 --read")

        write_all(fd, (args.command + "\n").encode())
        out = drain(fd, args.wait)
        sys.stdout.write(out.decode("utf-8", "replace"))
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
