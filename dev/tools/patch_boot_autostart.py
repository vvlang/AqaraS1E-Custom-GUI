#!/usr/bin/env python3
"""
patch_boot_autostart — 给 post_init.sh 注入自研程序的启动与保活逻辑

背景
----
设备的 /tmp 是 tmpfs，重启后会被 post_init.sh 重建成只含作者自己的
index.html 和 api.cgi。我们放在 /tmp/www 的设置页与 CGI 会消失，
cam_view 也不会自动起来。

必须靠改 post_init.sh 才能解决——这是本项目第一次要动作者的启动脚本，
所以这个脚本刻意做得保守：
  - 锚点必须唯一命中，否则拒绝修改
  - 幂等：已打过补丁就直接退出，不会重复插入
  - 所有新增代码都用 `[ -f /data/scripts/dev/xxx ]` 兜底——
    文件不在就跳过，即使把自研目录整个删掉也不会影响设备启动
  - 插入的两段都是「已在运行就不重复拉起」，与作者自己的写法一致

插入点
------
A) 启动区：ha_daemon 启动块之后
     复制 cam_cgi / cam_settings.html 到 /tmp/www，启动 cam_view
B) 守护区：web_telnetd 保活块之后（缩进 4 空格的那处）
     cam_view 挂了自动拉起

用法
----
    python3 patch_boot_autostart.py --src <post_init.sh> --out <打补丁后的>
    python3 patch_boot_autostart.py --dir <已解压目录>   # 就地改
"""

import argparse
import os
import shutil
import sys
import tempfile

MARK = "# ==== S1E-DEV-EXT BEGIN"

# --- 插入 A：启动区 ---
BLOCK_BOOT = MARK + """ (boot)
# 自研扩展：把摄像头设置页与 CGI 部署到 web 根目录，并拉起 cam_view。
# /tmp 是 tmpfs，重启后会被下面的 httpd 段落重建，所以必须每次开机都做一遍。
if [ -f /data/scripts/dev/cam_cgi ]; then
    mkdir -p /tmp/www/cgi-bin
    cp -f /data/scripts/dev/cam_cgi /tmp/www/cgi-bin/cam_cgi 2>/dev/null
    cp -f /data/scripts/dev/cam_settings.html /tmp/www/cam_settings.html 2>/dev/null
    chmod +x /tmp/www/cgi-bin/cam_cgi 2>/dev/null
fi
if [ -f /data/scripts/dev/cam_view ]; then
    if ! pidof cam_view >/dev/null 2>&1; then
        nohup /data/scripts/dev/cam_view >/tmp/cam_view.log 2>&1 &
    fi
fi
# ==== S1E-DEV-EXT END ===="""

# --- 插入 B：守护区（缩进 4 空格，与作者风格一致） ---
BLOCK_KEEPALIVE = """    # ==== S1E-DEV-EXT BEGIN (keepalive)
    if [ -f /data/scripts/dev/cam_view ] && ! pidof cam_view >/dev/null 2>&1; then
        nohup /data/scripts/dev/cam_view >/tmp/cam_view.log 2>&1 &
    fi
    # ==== S1E-DEV-EXT END ===="""

# 锚点：必须唯一命中的整块（含缩进差异的区分）
ANCHOR_BOOT = """if ! pidof ha_daemon >/dev/null 2>&1; then
    nohup /data/scripts/ha_daemon >/tmp/ha_daemon.log 2>&1 &
fi"""

ANCHOR_KEEPALIVE = """    if [ -f /data/scripts/web_telnetd ] && ! pidof web_telnetd >/dev/null 2>&1; then
        nohup /data/scripts/web_telnetd >/tmp/web_telnetd.log 2>&1 &
    fi"""


def fail(msg: str):
    print(f"[!] {msg}", file=sys.stderr)
    sys.exit(1)


def patch(text: str) -> str:
    if MARK in text:
        fail("post_init.sh 已打过补丁，未做修改（如需重打请先恢复原版）")

    # 锚点唯一性检查
    for name, anchor in (("启动区 ha_daemon", ANCHOR_BOOT),
                         ("守护区 web_telnetd", ANCHOR_KEEPALIVE)):
        n = text.count(anchor)
        if n != 1:
            fail(f"锚点「{name}」命中 {n} 次，期望 1 次。\n"
                 f"    上游可能改过 post_init.sh，请人工核对后再决定是否注入。")

    text = text.replace(ANCHOR_BOOT, ANCHOR_BOOT + "\n\n" + BLOCK_BOOT, 1)
    text = text.replace(ANCHOR_KEEPALIVE,
                        ANCHOR_KEEPALIVE + "\n" + BLOCK_KEEPALIVE, 1)

    # 语法自检：用 sh -n 检查（若系统有 sh）
    if shutil.which("sh"):
        import subprocess
        import tempfile as tf
        with tf.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
            f.write(text)
            tmp = f.name
        r = subprocess.run(["sh", "-n", tmp], capture_output=True, text=True)
        os.unlink(tmp)
        if r.returncode != 0:
            fail(f"补丁后的脚本语法检查未通过：\n{r.stderr}")

    return text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src")
    ap.add_argument("--dir")
    ap.add_argument("--out")
    args = ap.parse_args()

    if not args.src and not args.dir:
        ap.error("需要 --src 或 --dir")

    tmpdir = None
    if args.src:
        tmpdir = tempfile.mkdtemp(prefix="s1e_boot_")
        src = os.path.join(tmpdir, "post_init.sh")
        with open(args.src, "r", encoding="utf-8", errors="surrogateescape") as f:
            data = f.read()
    else:
        src = os.path.join(args.dir, "post_init.sh")
        with open(src, "r", encoding="utf-8", errors="surrogateescape") as f:
            data = f.read()

    before = len(data)
    patched = patch(data)
    added = len(patched) - before

    if args.out:
        with open(args.out, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(patched)
        print(f"[+] 已写入 {args.out}（+{added} 字节）")
    else:
        with open(src, "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(patched)
        print(f"[+] 已就地修改 {src}（+{added} 字节）")

    if tmpdir:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print()
    print("注入内容：")
    print("  启动区  —— 部署 cam_cgi/cam_settings.html 到 /tmp/www，拉起 cam_view")
    print("  守护区  —— cam_view 掉线自动重启")
    print()
    print("所有新增逻辑都以 [ -f /data/scripts/dev/xxx ] 兜底，")
    print("删掉整个 dev 目录也不会影响设备正常启动。")
    print()
    print("部署提醒：修改 post_init.sh 属于改动作者的启动流程，")
    print("请确认设备当前没有运行关键任务，并保留一份原版备份。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
