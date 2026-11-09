#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""生成 ath11k 的 board data 文件。

子命令：
  wrap       把裸 BDF 负载写入单段 ELF 容器（QCA6390 的 bdwlan.eXX 格式）。
  container  按 QCA-ATH11K-BOARD 容器格式生成 board-2.bin。

容器格式取自 drivers/net/wireless/ath/ath11k/core.c：magic 含结尾空字节
并补齐到 4 字节，随后是若干 BOARD IE（id 0）；每个 BOARD IE 内含板名子
IE（id 0）与板数据子 IE（id 1），子 IE 长度补齐到 4 字节倍数。
"""

import argparse
import struct

MAGIC = b"QCA-ATH11K-BOARD\x00"
IE_BOARD = 0
IE_BOARD_NAME = 0
IE_BOARD_DATA = 1
PT_LOAD = 1


def align4(data: bytes) -> bytes:
    return data + b"\x00" * (-len(data) % 4)


def wrap(args: argparse.Namespace) -> None:
    with open(args.template, "rb") as f:
        blob = bytearray(f.read())
    with open(args.payload, "rb") as f:
        payload = f.read()

    phoff = struct.unpack_from("<I", blob, 0x1C)[0]
    if phoff + 32 > len(blob):
        raise SystemExit("模板的程序头超出文件范围")

    p_type, p_offset, _, _, p_filesz, _, _, _ = struct.unpack_from(
        "<IIIIIIII", blob, phoff
    )

    if p_type != PT_LOAD:
        raise SystemExit(f"程序头类型 {p_type} 不是 PT_LOAD")
    if p_offset + p_filesz > len(blob):
        raise SystemExit("模板的程序头段超出文件范围")
    if len(payload) != p_filesz:
        raise SystemExit(f"负载 {len(payload)} 字节与段尺寸 {p_filesz} 不符")

    blob[p_offset : p_offset + p_filesz] = payload
    with open(args.output, "wb") as f:
        f.write(blob)
    print(
        f"{args.output}: {len(blob)} 字节，负载 {len(payload)} 字节位于 0x{p_offset:x}"
    )


def sub_ie(ie_id: int, payload: bytes) -> bytes:
    return struct.pack("<II", ie_id, len(payload)) + align4(payload)


def container(args: argparse.Namespace) -> None:
    with open(args.data, "rb") as f:
        data = f.read()
    body = sub_ie(IE_BOARD_NAME, args.name.encode()) + sub_ie(IE_BOARD_DATA, data)
    blob = align4(MAGIC) + struct.pack("<II", IE_BOARD, len(body)) + align4(body)
    with open(args.output, "wb") as f:
        f.write(blob)
    print(f"{args.output}: {len(blob)} 字节，条目 {args.name}，数据 {len(data)} 字节")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_wrap = sub.add_parser("wrap", help="把裸负载装入 ELF 容器")
    p_wrap.add_argument("--template", required=True, help="原厂 bdwlan.eXX 容器")
    p_wrap.add_argument("--payload", required=True, help="裸 BDF 负载")
    p_wrap.add_argument("--output", required=True)
    p_wrap.set_defaults(func=wrap)

    p_cont = sub.add_parser("container", help="生成 board-2.bin")
    p_cont.add_argument(
        "--name",
        required=True,
        help="板名，例如 bus=pci,qmi-chip-id=0,qmi-board-id=255",
    )
    p_cont.add_argument("--data", required=True, help="板数据文件")
    p_cont.add_argument("--output", required=True)
    p_cont.set_defaults(func=container)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
