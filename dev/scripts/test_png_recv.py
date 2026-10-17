#!/usr/bin/env python3
"""
safe_png_recv 的本机回归测试。

这个工具是安全边界（校验外部传入的图片），所以测试覆盖正常路径和
各种拒绝路径。临时路径写死为 /tmp/notify_image*，测试结束后清理。

用法：./scripts/test_png_recv.py
"""

import os
import struct
import subprocess
import sys
import zlib

SRC = "./src/safe_png_recv.c"
BIN = "/tmp/safe_png_recv_test"          # 本机测试版
FINAL = "/tmp/notify_image.png"
TMP = "/tmp/notify_image.png.tmp"
READY = "/tmp/notify_image.ready"

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ✓ {name}")
    else:
        failed += 1
        print(f"  ✗ {name}   {detail}")


def cleanup():
    for p in (FINAL, TMP, READY):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass


def png_chunk(typ: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + typ + data +
            struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))


def make_png(w, h, payload=b"\x00\xff\x00\x00") -> bytes:
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) +
            png_chunk(b"IDAT", zlib.compress(payload)) + png_chunk(b"IEND", b""))


def run(data: bytes):
    cleanup()
    p = subprocess.run([BIN], input=data, capture_output=True, timeout=20)
    landed = os.path.exists(FINAL)
    ready = os.path.exists(READY)
    tmp_left = os.path.exists(TMP)
    return p.returncode, landed, ready, tmp_left


print("══ 编译本机测试版 ══")
r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-o", BIN, SRC],
                   capture_output=True, text=True)
if r.returncode != 0:
    print("  ✗ 编译失败:")
    print(r.stderr)
    sys.exit(1)
print("  ✓ 编译通过")
print()

print("══ 正常路径 ══")
rc, landed, ready, tmp = run(make_png(480, 480))
check("480x480 合法 PNG → 退出 0", rc == 0, f"rc={rc}")
check("文件已落盘", landed)
check("就绪标志已创建", ready)
check("临时文件已清理", not tmp)

rc, *_ = run(make_png(1, 1))
check("1x1 最小尺寸 → 通过", rc == 0, f"rc={rc}")

rc, *_ = run(make_png(320, 240))
check("320x240 普通尺寸 → 通过", rc == 0, f"rc={rc}")
print()

print("══ 拒绝路径 ══")
rc, landed, _, tmp = run(b"NOT-A-PNG-AT-ALL" + b"\x00" * 100)
check("非 PNG 数据 → 退出 2", rc == 2, f"rc={rc}")
check("非法数据不落盘", not landed)
check("非法数据不留临时文件", not tmp)

rc, *_ = run(make_png(481, 480))
check("宽度 481 越界 → 退出 2", rc == 2, f"rc={rc}")

rc, *_ = run(make_png(480, 481))
check("高度 481 越界 → 退出 2", rc == 2, f"rc={rc}")

rc, *_ = run(make_png(1920, 1080))
check("1920x1080 越界 → 退出 2", rc == 2, f"rc={rc}")

rc, *_ = run(b"\x89PNG\r\n\x1a\n")
check("只有签名 8 字节 → 退出 5（过短）", rc == 5, f"rc={rc}")

rc, *_ = run(b"")
check("空输入 → 退出 5", rc == 5, f"rc={rc}")

big = b"\x89PNG\r\n\x1a\n" + b"\x00" * (600 * 1024)
rc, landed, _, tmp = run(big)
check("超过 512KiB → 退出 4", rc == 4, f"rc={rc}")
check("超限不落盘", not landed)
check("超限不留临时文件", not tmp)
print()

print("══ 完整性校验（作者原版没有的部分）══")
truncated = make_png(100, 100)[:-20]          # 砍掉 IEND
rc, landed, _, _ = run(truncated)
check("截断的 PNG（无 IEND）→ 退出 2", rc == 2, f"rc={rc}")
check("截断的 PNG 不落盘", not landed)

no_ihdr = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IDAT", b"\x00") + png_chunk(b"IEND", b"")
rc, *_ = run(no_ihdr)
check("缺少 IHDR → 退出 2", rc == 2, f"rc={rc}")
print()

print("══ 残留清理（安全边界）══")
cleanup()
os.makedirs("/tmp", exist_ok=True)
open(TMP, "wb").write(b"OLD STALE DATA")
rc, _, _, _ = run(make_png(50, 50))
check("启动时清除旧临时文件", rc == 0 and not os.path.exists(TMP))
check("旧文件内容未被继承",
      os.path.exists(FINAL) and b"OLD STALE" not in open(FINAL, "rb").read())

cleanup()
print()
print("══ 结果 ══")
print(f"  通过 {passed} / 失败 {failed}")
sys.exit(0 if failed == 0 else 1)
