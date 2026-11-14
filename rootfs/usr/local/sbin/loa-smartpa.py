#!/usr/bin/env python3
"""DBY-W09 四颗 TFA9874 扬声器功放的用户空间控制。

内核侧只提供 TDM 数据通路与 GPIO 109 复位 hog，功放的初始化序列来自
设备自带的 /odm 配置（tfa9874_0..3_configs.xml），由安装期脚本提取到
/etc/loa-smartpa/。

行为：
- 播放 PCM 进入 RUNNING 后，逐颗执行 INIT_CHIP 与 POWER_ON_L；
- 播放停止 0.5 s 后执行 POWER_OFF_L；
- 播放期间每 0.5 s 检查一次 0x00 的 AMPE 位，TDM 时钟短暂中断会让芯片
  自行退出上电状态，检测到后重新执行 POWER_ON_L。

寄存器访问用 I2C_RDWR ioctl，按 [reg, hi, lo] 发送。tfa9874 是 16 位
大端寄存器；i2cget/i2cset 的 SMBus word 模式会交换高低字节。
"""
import ctypes
import fcntl
import os
import re
import signal
import sys
import time

I2C_RDWR = 0x0707
I2C_M_RD = 0x0001
I2C_BUS = 3
AMP_BASE = 0x34
AMP_COUNT = 4
CONF_DIR = "/etc/loa-smartpa"
POLL_SEC = 0.1
OFF_DELAY_SEC = 0.5
AMPE_CHECK_SEC = 0.5
AMPE_MASK = 0x0008
CARD_NAME = "DBYW09"

# 用户空间 PCM 由 ALSA plug 的 duplicate 策略把立体声复制成
# [左, 右, 左, 右] 四个槽位。索引 0 至 3 对应 0x34 至 0x37，按设备竖屏
# 坐标固定为左上、左下、右上、右下。
#
# 槽位按内容坐标分配：以用户看到的内容上方为前、内容左方为左，四个
# PCM 声道 FL、FR、RL、RR 依次占 0 至 3。屏幕旋转改变内容上方在设备
# 上的指向，映射表因此随朝向切换。
ORIENTATION_PATH = "/var/lib/loa/display-orientation"
SLOT_MAPS = {
    # 横屏：内容上方指向设备右侧，左前 = 右上
    "right": {0: 2, 1: 3, 2: 0, 3: 1},
    # 横屏翻转 180 度：内容上方指向设备左侧，左前 = 左下
    "left": {0: 1, 1: 0, 2: 3, 3: 2},
    # 竖屏：内容上方指向设备上方，左前 = 左上
    "normal": {0: 0, 1: 2, 2: 1, 3: 3},
    # 竖屏翻转 180 度：内容上方指向设备下方，左前 = 右下
    "inverted": {0: 3, 1: 1, 2: 2, 3: 0},
}

# 0x26 的两个槽位字段（TFA9874_BF_TDMSPKS 位于低 3 位、TFA9874_BF_TDMDCS
# 位于 bit4 至 bit6）。芯片把扬声器与 DCDC 两个 sink 的槽位编进同一个
# 查找表，两者取同一个槽时报 TDMLUTER（0x11 bit8）并停掉扬声器输出，
# 因此改写扬声器槽位时必须同时给出一个不同的 DCDC 槽位。原厂 XML 对
# 槽位为 3 的那颗取 2、其余三颗取 3。
TDM_SLOT_MASK = 0x0077

# 0x53 至 0xc4 一带的模拟与校准寄存器受 KEY1 保护：未解锁时读回 0，
# 写入被芯片丢弃。XML 把解锁行写成固定值 0x005a，直接写入无法解锁，
# 芯片要求 KEY1 = 读回 0xfb ^ 0x5a（NXP 驱动 tfa9874_specific() 的做法）。
# 0x0f 写入 0x5a6b 之后 0xa0 与 0xa1 才可写，KEY2 取固定值 0x5a。
KEY1_REG = 0xA0
KEY1_SOURCE_REG = 0xFB
KEY1_XOR = 0x005A
KEY2_REG = 0xA1
KEY2_VALUE = 0x005A

# 这几位按芯片状态给出回读值，与写入值不同属于正常：0x00 的 PWDN 在
# 无时钟时保持置位，0x10 是读清除的锁存状态，0x44 是读清除的告警位。
WRITE_VERIFY_SKIP = {0x00, 0x10, 0x44}


def dcdc_slot_for(slot):
    return 2 if slot == 3 else 3


def read_orientation():
    """读取屏幕旋转；文件缺失或取值未知时按竖屏处理。"""
    try:
        with open(ORIENTATION_PATH, encoding="utf-8") as file:
            name = file.read().strip()
    except FileNotFoundError:
        return "normal"
    return name if name in SLOT_MAPS else "normal"


class I2CMsg(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("len", ctypes.c_uint16),
        ("buf", ctypes.POINTER(ctypes.c_uint8)),
    ]


class I2CRdwrData(ctypes.Structure):
    _fields_ = [
        ("msgs", ctypes.POINTER(I2CMsg)),
        ("nmsgs", ctypes.c_uint32),
    ]


class Bus:
    def __init__(self, bus=I2C_BUS):
        self.fd = open(f"/dev/i2c-{bus}", "r+b", buffering=0)

    def _xfer(self, msgs):
        arr = (I2CMsg * len(msgs))(*msgs)
        done = fcntl.ioctl(self.fd.fileno(), I2C_RDWR,
                           I2CRdwrData(arr, len(msgs)))
        if done != len(msgs):
            raise OSError(f"I2C 传输未完成：{done}/{len(msgs)} 条消息")

    def read16(self, addr, reg):
        rbuf = (ctypes.c_uint8 * 1)(reg)
        wbuf = (ctypes.c_uint8 * 2)()
        self._xfer([
            I2CMsg(addr, 0, 1, rbuf),
            I2CMsg(addr, I2C_M_RD, 2, wbuf),
        ])
        return (wbuf[0] << 8) | wbuf[1]

    def write16(self, addr, reg, value):
        buf = (ctypes.c_uint8 * 3)(reg, (value >> 8) & 0xFF, value & 0xFF)
        self._xfer([I2CMsg(addr, 0, 3, buf)])


def parse_scene(path, scene):
    text = open(path, encoding="utf-8").read()
    match = re.search(r'<SCENCE NAME="%s".*?VALUE="\{(.*?)\}"'
                      % re.escape(scene), text, re.S)
    if not match:
        raise RuntimeError(f"{path}: 缺少场景 {scene}")
    rows = []
    for line in match.group(1).replace("}", "").splitlines():
        line = re.sub(r"#[^,\n]*", "", line)
        cells = [c.strip() for c in line.strip().rstrip(",").split(",")]
        if len(cells) < 4:
            continue
        try:
            reg, mask, val, delay = (int(c, 16) for c in cells[:4])
        except ValueError:
            continue
        rows.append((reg, mask, val, delay))
    return rows


def apply_scene(bus, addr, rows):
    mismatched = []
    for reg, mask, val, delay in rows:
        # mask 为 0 的行只带延时，不写寄存器
        if mask == 0:
            if delay:
                time.sleep(delay / 1000.0)
            continue
        if reg == KEY1_REG:
            # KEY1 与芯片的 0xfb 相关，不能照抄 XML 的固定值；随后补写
            # KEY2，两个密钥都解开后受保护寄存器才接受读写。
            bus.write16(addr, KEY1_REG,
                        bus.read16(addr, KEY1_SOURCE_REG) ^ KEY1_XOR)
            bus.write16(addr, KEY2_REG, KEY2_VALUE)
        else:
            if mask == 0xFFFF:
                new = val
            else:
                new = (bus.read16(addr, reg) & ~mask) | (val & mask)
            bus.write16(addr, reg, new)
            if reg not in WRITE_VERIFY_SKIP and bus.read16(addr, reg) != new:
                mismatched.append(reg)
        if delay:
            time.sleep(delay / 1000.0)
    return mismatched


class SmartPA:
    def __init__(self):
        self.bus = Bus()
        self.scenes = {}
        for idx in range(AMP_COUNT):
            path = os.path.join(CONF_DIR, f"tfa9874_{idx}_configs.xml")
            self.scenes[idx] = {
                scene: parse_scene(path, scene)
                for scene in ("INIT_CHIP", "POWER_ON_L", "POWER_OFF_L")
            }
        self.initialized = False
        self.powered = False
        self.orientation = None

    def _apply(self, idx, scene):
        mismatched = apply_scene(self.bus, AMP_BASE + idx, self.scenes[idx][scene])
        if mismatched:
            log(f"0x{AMP_BASE + idx:02x} 的 {scene} 有写入未生效："
                + "、".join(f"0x{reg:02x}" for reg in mismatched))

    def apply_orientation(self, name):
        """按屏幕朝向改写 0x26 的扬声器与 DCDC 槽位，保留其余位。"""
        for idx, slot in SLOT_MAPS[name].items():
            addr = AMP_BASE + idx
            reg = self.bus.read16(addr, 0x26)
            value = ((reg & ~TDM_SLOT_MASK) | ((dcdc_slot_for(slot) & 0x7) << 4)
                     | (slot & 0x7))
            self.bus.write16(addr, 0x26, value)
        self.orientation = name

    def power_on(self):
        if not self.initialized:
            for idx in range(AMP_COUNT):
                self._apply(idx, "INIT_CHIP")
            self.initialized = True
        for idx in range(AMP_COUNT):
            self._apply(idx, "POWER_ON_L")
        # 2 号的 POWER_ON_L 场景本身会写 0x26，槽位修正必须放在它之后
        self.apply_orientation(read_orientation())
        self.powered = True
        log(f"功放已上电（屏幕朝向 {self.orientation}）")

    def power_off(self):
        for idx in range(AMP_COUNT):
            self._apply(idx, "POWER_OFF_L")
        self.powered = False
        log("功放已下电")

    def ampe_lost(self):
        """任一颗芯片的 AMPE 被清掉时返回 True。"""
        for idx in range(AMP_COUNT):
            if not self.bus.read16(AMP_BASE + idx, 0x00) & AMPE_MASK:
                return True
        return False


def log(msg):
    print(f"loa-smartpa: {msg}", flush=True)


def find_pcm_status():
    """按声卡名字找 PCM0 播放子流的状态文件路径。"""
    with open("/proc/asound/cards", encoding="utf-8") as fh:
        for line in fh:
            match = re.match(r"\s*(\d+)\s+\[\s*(\S+)\s*\]", line)
            if match and match.group(2) == CARD_NAME:
                return (f"/proc/asound/card{match.group(1)}"
                        "/pcm0p/sub0/status")
    raise RuntimeError(f"找不到声卡 {CARD_NAME}")


def pcm_running(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return "state: RUNNING" in fh.read()
    except FileNotFoundError:
        return False


def main():
    running = True

    def stop(signum, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    status_path = find_pcm_status()
    pa = SmartPA()
    log(f"启动，状态文件 {status_path}")

    last_on = 0.0
    last_ampe_check = 0.0
    while running:
        now = time.monotonic()
        if pcm_running(status_path):
            last_on = now
            if not pa.powered:
                pa.power_on()
                last_ampe_check = now
            elif now - last_ampe_check >= AMPE_CHECK_SEC:
                last_ampe_check = now
                orientation = read_orientation()
                if orientation != pa.orientation:
                    log(f"屏幕旋转为 {orientation}，重排槽位")
                    pa.apply_orientation(orientation)
                if pa.ampe_lost():
                    log("检测到 AMPE 被清掉，重新上电")
                    pa.power_on()
        elif pa.powered and now - last_on >= OFF_DELAY_SEC:
            pa.power_off()
        time.sleep(POLL_SEC)

    if pa.powered:
        pa.power_off()
    return 0


if __name__ == "__main__":
    sys.exit(main())
