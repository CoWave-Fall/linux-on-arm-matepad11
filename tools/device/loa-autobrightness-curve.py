#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""把自动亮度的密集曲线写进 KWin 的输出配置。

KWin 只在启动时读取 ~/.config/kwinoutputconfig.json，曲线本身又只能在
手动调亮度时按当前照度逐点学习。重装 rootfs 后先执行本脚本，再重新登录
桌面，即可恢复基线曲线。

已有四个以上控制点时保留现状，避免抹掉使用中学习到的点；--force 无条件
写回基线，用于主动重置。
"""
import argparse
import json
import os
import sys
from pathlib import Path

# 环境光（lux）到亮度设定的映射。KWin 把设定值线性写入背光的感知亮度
# 接口，35~150 lux 区间的斜率约 0.12%/lux，远低于学习曲线的量级。
CURVE = [
    (0, 0.10),
    (1, 0.12),
    (5, 0.15),
    (20, 0.22),
    (50, 0.30),
    (150, 0.42),
    (400, 0.55),
    (1000, 0.70),
    (4000, 0.85),
    (20000, 1.0),
]
MIN_POINTS = 4
CONNECTOR = "DSI-1"


def config_path():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(Path.home(), ".config")
    return Path(base) / "kwinoutputconfig.json"


def find_output(document):
    for section in document:
        if section.get("name") != "outputs":
            continue
        for output in section.get("data", []):
            if output.get("connectorName") == CONNECTOR:
                return output
    raise SystemExit(f"{CONNECTOR} 不在配置文件里，先登录一次桌面再执行")


def write_atomic(path, document):
    """先写同目录临时文件再 rename，避免 KWin 读到半截 JSON。"""
    st = path.stat()
    tmp = path.with_name(path.name + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, st.st_mode & 0o777)
    with os.fdopen(fd, "w") as f:
        json.dump(document, f, indent=4)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main():
    parser = argparse.ArgumentParser(description="写入自动亮度基线曲线")
    parser.add_argument("--force", action="store_true",
                        help="忽略现有点数，无条件写回基线曲线")
    args = parser.parse_args()

    path = config_path()
    if not path.exists():
        raise SystemExit(f"找不到 {path}，先登录一次桌面再执行")
    with path.open() as f:
        document = json.load(f)
    output = find_output(document)

    current = output.get("autoBrightnessCurve")
    if not args.force and current and len(current) >= MIN_POINTS:
        print(f"{CONNECTOR} 已有 {len(current)} 个控制点，保留现状（--force 可写回基线）")
        return 0

    output["autoBrightnessCurve"] = [[lux, brightness] for lux, brightness in CURVE]
    output["automaticBrightness"] = True
    write_atomic(path, document)
    print(f"已写入 {len(CURVE)} 个控制点的自动亮度曲线，下次登录生效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
