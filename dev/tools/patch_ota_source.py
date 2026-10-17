#!/usr/bin/env python3
"""
patch_ota_source — 把 OTA 更新源从上游作者仓库改到本 fork。

背景
----
作者的 `api.cgi` 把 OTA 下载地址**硬编码在二进制里**，共 3 处：

    0x00e019  下载示例动效 .anim
    0x011ac8  拉取远程 version.json（更新检测）
    0x011dce  下载 OTA tarball

    https://raw.githubusercontent.com/oopuuu/AqaraS1E-Custom-GUI/main/...

`fetch_https.sh` 里还有 2 处硬编码（jsdelivr 兜底路径）。

改 `version.json` 的 `tarball_url` **没有任何作用** —— OTA 根本不读它。

为什么能安全地改二进制
----------------------
`oopuuu` 与 `vvlang` 同为 6 字节，可以**等长原位替换**：
文件大小不变、`.rodata` 里所有偏移不变、不需要重新链接。
替换后必须验证 ELF 仍然可被内核加载。

用法
----
    python3 patch_ota_source.py --src <tarball> --out <patched.tar.gz> --owner vvlang
    python3 patch_ota_source.py --dir <解压目录>       # 就地打补丁

风险
----
若 fork 上没有对应文件，设备更新会失败。因此脚本会先校验
输出包里的 release 资产齐全（由调用方另行用 HTTP 验证）。
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile

# (文件名, 期望出现次数) —— 出现次数变化说明版本变了，需要重新核对
#
# fetch_https.sh 的 3 处在两行里：
#   第 38 行  grep 判断条件
#   第 39 行  jsdelivr 路径拼接 + ${PATH_URL#...} 前缀剥离（同一行两处）
EXPECTED = {
    "api.cgi": 3,
    "fetch_https.sh": 3,
}


def patch_bytes(data: bytes, old: bytes, new: bytes, where: str) -> bytes:
    if len(old) != len(new):
        raise SystemExit(f"[!] 长度不等，拒绝替换: {old!r}({len(old)}) -> {new!r}({len(new)})")
    n = data.count(old)
    if n == 0:
        raise SystemExit(f"[!] {where}: 未找到 {old.decode()!r}，版本可能已变，请人工核对")
    return data.replace(old, new)


def patch_file(path: str, owner: str) -> bool:
    """就地打补丁，返回是否发生了改动。"""
    name = os.path.basename(path)
    old = b"oopuuu"
    new = owner.encode()

    raw = open(path, "rb").read()
    n = raw.count(old)

    if n != EXPECTED.get(name, 0):
        raise SystemExit(
            f"[!] {name}: 期望 {EXPECTED.get(name)} 处 '{old.decode()}'，实际 {n} 处。\n"
            f"    二进制版本可能已变，不要盲目替换——请先 diff 确认。"
        )

    if n == 0:
        return False

    before_size = len(raw)
    before_sha = hashlib.sha256(raw).hexdigest()

    out = patch_bytes(raw, old, new, name)

    if len(out) != before_size:
        raise SystemExit(f"[!] {name}: 替换后大小变了 ({before_size} -> {len(out)})，拒绝写入")

    with open(path, "wb") as f:
        f.write(out)

    after_sha = hashlib.sha256(out).hexdigest()
    print(f"  ✓ {name}: {n} 处替换  {before_size} 字节不变")
    print(f"      sha256 {before_sha[:16]}… -> {after_sha[:16]}…")
    return True


def verify_elf(path: str) -> None:
    """确认改完仍然是合法的 ARM ELF。"""
    with open(path, "rb") as f:
        head = f.read(20)
    if head[:4] != b"\x7fELF":
        raise SystemExit(f"[!] {path}: ELF 魔数被破坏")
    if head[4] != 1:
        raise SystemExit(f"[!] {path}: 不是 32 位")
    e_machine = int.from_bytes(head[18:20], "little")
    if e_machine != 40:
        raise SystemExit(f"[!] {path}: e_machine={e_machine}，期望 40(ARM)")
    print(f"  ✓ {os.path.basename(path)}: ELF32 ARM 头完好")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", help="原始 tarball")
    ap.add_argument("--dir", help="已解压的目录，就地打补丁")
    ap.add_argument("--out", help="输出的补丁后 tarball")
    ap.add_argument("--owner", default="vvlang", help="目标仓库 owner")
    args = ap.parse_args()

    if not args.src and not args.dir:
        ap.error("需要 --src 或 --dir 之一")

    tmp = None
    if args.src:
        tmp = tempfile.mkdtemp(prefix="s1e_patch_")
        print(f"解包 {args.src} -> 临时目录")
        with tarfile.open(args.src, "r:gz") as tf:
            tf.extractall(tmp)
        workdir = tmp
    else:
        workdir = args.dir

    print(f"\n打补丁 (oopuuu -> {args.owner}):")
    changed = 0
    for name in EXPECTED:
        p = os.path.join(workdir, name)
        if not os.path.exists(p):
            print(f"  - {name} 不存在，跳过")
            continue
        if patch_file(p, args.owner):
            changed += 1

    verify_elf(os.path.join(workdir, "api.cgi"))

    # 确认改后的路径在脚本里自洽
    sh = open(os.path.join(workdir, "fetch_https.sh"), encoding="utf-8").read()
    if "oopuuu" in sh:
        raise SystemExit("[!] fetch_https.sh 仍残留 oopuuu")
    if args.owner not in sh:
        raise SystemExit(f"[!] fetch_https.sh 中找不到 {args.owner}")
    print(f"  ✓ fetch_https.sh 内路径自洽")

    if args.out:
        print(f"\n打包 -> {args.out}")
        # 保持与原包一致的扁平结构
        with tarfile.open(args.out, "w:gz") as tf:
            for root, _, files in os.walk(workdir):
                for fn in files:
                    full = os.path.join(root, fn)
                    tf.add(full, arcname=os.path.relpath(full, workdir))
        print(f"  ✓ {os.path.getsize(args.out)} 字节")

    if tmp:
        shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n完成，{changed} 个文件已打补丁。")
    print("提醒：推送后请确认 fork 的 release 资产齐全，否则设备 OTA 会失败。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
