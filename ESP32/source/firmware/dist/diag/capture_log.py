#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
capture_log.py  ——  ESP32 串口日志抓取（配合诊断版固件使用）

用法:
    python capture_log.py COM7            # 抓 60 秒
    python capture_log.py COM7 90         # 抓 90 秒
    python capture_log.py                 # 自动找 CH340 串口

做了什么:
  1. 打开串口 115200 8N1
  2. 用 DTR/RTS 给 ESP32 来一次「硬复位」，让它从正常运行模式启动
     （顺序: DTR=0 / RTS=1 拉低 EN 进复位 -> RTS=0 释放 -> 正常启动）
  3. 持续读取并实时打印，同时写一份到 diag-log.txt
  4. 结束时报出: 抓到多少字节、发生了几次复位、以及关键行摘录

为什么要复位: ESP32 的日志只在启动/事件发生时打印，
             复位一次才能看到完整的启动过程（蓝牙初始化那几行）。
"""
import sys
import os
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("[错误] 没找到 pyserial。请用随固件提供的 Python 运行本脚本，")
    print("       或执行: pip install pyserial")
    sys.exit(1)

BAUD = 115200
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "diag-log.txt")

KEYWORDS = (
    "connection_cb", "connected", "discoverable", "connecting",
    "reply", "authentication", "bt_address", "hid device",
    "setting hid parameters", "starting hid device", "device class",
    "easycon", "EasyCon", "panic", "Guru Meditation", "abort()",
    "Backtrace", "rst:0x",
)


def pick_port():
    cands = list(list_ports.comports())
    if not cands:
        print("[错误] 没有找到任何串口。请确认板子已插好、驱动已装。")
        sys.exit(1)
    for p in cands:
        desc = (p.description or "") + " " + (p.hwid or "")
        if "CH340" in desc.upper() or "1A86" in desc.upper():
            print("[自动选择] %s  (%s)" % (p.device, p.description))
            return p.device
    print("[错误] 没自动认出 CH340 串口。可用串口如下，请把端口名作为参数传进来:")
    for p in cands:
        print("   %-8s %s" % (p.device, p.description))
    sys.exit(1)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else pick_port()
    dur = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0

    print("=" * 66)
    print(" 端口: %s   波特率: %d   抓取时长: %.0f 秒" % (port, BAUD, dur))
    print(" 输出文件: %s" % OUT)
    print("=" * 66)

    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD
        ser.bytesize = serial.EIGHTBITS
        ser.parity = serial.PARITY_NONE
        ser.stopbits = serial.STOPBITS_ONE
        ser.timeout = 0.2
        ser.write_timeout = 1
        ser.open()
    except Exception as e:
        print("[错误] 打不开 %s -> %s" % (port, e))
        print("       常见原因: EasyCon 等上位机占着这个串口，请先关掉它。")
        sys.exit(1)

    # --- 硬复位：进正常运行模式，不是下载模式 ---
    try:
        ser.dtr = False
        ser.rts = True          # EN 拉低 -> 复位
        time.sleep(0.15)
        ser.dtr = False         # IO0 保持高 -> 正常启动
        ser.rts = False         # 释放复位
    except Exception as e:
        print("[警告] 无法操作 DTR/RTS (%s)。仍会继续抓取，"
              "但可能抓不到启动日志——请手动按一下板子的 RST/EN 键。" % e)

    t0 = time.time()
    buf = bytearray()
    lines = []
    partial = b""

    print("\n---- 已开始抓取，请现在去做: 打开 Switch2 的『更换握把顺序』尝试配对 ----\n")

    while time.time() - t0 < dur:
        try:
            chunk = ser.read(4096)
        except Exception as e:
            print("\n[错误] 读串口失败: %s" % e)
            break
        if not chunk:
            continue
        buf += chunk
        text = partial + chunk
        parts = text.split(b"\n")
        partial = parts.pop()
        for raw in parts:
            line = raw.decode("utf-8", errors="replace").rstrip("\r")
            lines.append(line)
            print(line)
        sys.stdout.flush()

    if partial:
        line = partial.decode("utf-8", errors="replace").rstrip("\r")
        lines.append(line)
        print(line)

    try:
        ser.dtr = False
        ser.rts = False
        ser.close()
    except Exception:
        pass

    # ---- 写入文件 ----
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    # ---- 汇总 ----
    resets = sum(1 for l in lines if "rst:0x" in l)
    print("\n" + "=" * 66)
    print(" 抓取结束")
    print("   收到字节数 : %d" % len(buf))
    print("   日志行数   : %d" % len(lines))
    print("   复位次数   : %d  %s" % (
        resets,
        "  <-- 注意! 大于 1 说明板子在反复重启" if resets > 1 else ""))
    print("   日志文件   : %s" % OUT)
    print("=" * 66)

    hits = [l for l in lines if any(k in l for k in KEYWORDS)]
    if hits:
        print("\n---- 关键行摘录 (%d 条) ----" % len(hits))
        for l in hits[-60:]:
            print("  " + l)
    else:
        print("\n[!] 没有抓到任何关键行。")
        print("    可能原因: (a) 这个串口不是 ESP32; (b) 固件不是诊断版;")
        print("             (c) 波特率不对; (d) 板子没上电。")


if __name__ == "__main__":
    main()
