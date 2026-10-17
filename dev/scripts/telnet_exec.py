#!/usr/bin/env python3
"""
telnet_exec — 通过 telnet(23) 在 S1E 上执行命令并回显输出。

macOS 新版本已移除 telnet 客户端，此脚本用裸 socket 顶替。

登录序列可配置，因为该设备的握手顺序因固件而异：

    python3 scripts/telnet_exec.py <IP> "<命令>"
    python3 scripts/telnet_exec.py <IP> --user root --pass "" --rounds 2 "<命令>"

--rounds 表示最多应付几轮 login/password 提示（部分固件登录后还要再输一次用户名）。

密码通过环境变量 TELNET_PASS 传入，避免出现在 shell 历史和进程列表里：
    TELNET_PASS='你的密码' python3 scripts/telnet_exec.py <IP> "<命令>"

注意：命令会被设备以 root 执行。调用方需自行确保是只读操作。
"""

import os
import socket
import sys
import time

TELNET_PORT = 23
DEFAULT_USER = "root"
PROMPT_SETTLE = 0.8


def strip_telnet(data: bytes) -> str:
    """去掉 telnet 协议控制字符（IAC 协商等），保留可打印内容。"""
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b == 0xFF:                      # IAC
            if i + 1 < len(data):
                cmd = data[i + 1]
                if cmd == 0xFF:            # 转义的 0xFF
                    out.append(0xFF)
                    i += 2
                    continue
                if cmd in (0xFB, 0xFC, 0xFD, 0xFE):   # WILL/WONT/DO/DONT
                    i += 3
                    continue
            i += 2
            continue
        if b in (0x00, 0x0A, 0x0D):       # 保留换行，丢掉 NUL
            if b != 0x00:
                out.append(b)
            i += 1
            continue
        if 0x20 <= b < 0x7F or b > 0x7F:  # ASCII 可打印 + UTF-8 高位
            out.append(b)
        i += 1
    return out.decode("utf-8", errors="replace")


def telnet_exec(ip: str, commands: list[str], user: str = DEFAULT_USER,
                 password: str = "", rounds: int = 2, timeout: float = 30.0) -> str:
    collected = []

    sock = socket.create_connection((ip, TELNET_PORT), timeout=10)
    sock.settimeout(timeout)

    # 故意不发 IAC 协商包。刚连上就发协商会让设备把此时的输入丢掉，
    # 表现为每次登录头几个字符被吞（"root" 变成 "ot"）。
    # 这里先静默读完对方的 banner 和协商请求。

    def drain(seconds: float) -> str:
        """读取若干秒内的输出，返回本次读到的文本。"""
        buf = []
        end = time.time() + seconds
        while time.time() < end:
            sock.settimeout(max(0.1, end - time.time()))
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                text = strip_telnet(chunk)
                buf.append(text)
            except socket.timeout:
                break
            except OSError:
                break
        out = "".join(buf)
        collected.append(out)
        return out

    def send_line(text: str, settle: float = PROMPT_SETTLE) -> None:
        """发一行。用 CRLF —— telnet 规范模式认 \\r 作为行结束符。"""
        sock.sendall(text.encode() + b"\r\n")
        drain(settle)

    # --- 登录握手 ---
    # 该设备的实测序列（用户提供并验证过）：
    #   Magic-Switch-S1E-E420 login:  -> 输入用户名
    #   Password:                     -> 空密码，直接回车
    #   ~ #                           -> shell 提示符
    #
    # 不用提示符状态机：设备推送 banner 与提示符的时序不稳，解析提示符会
    # 和设备抢时序，导致把空用户名当成真用户名提交。改为按已知序列定时发送。
    drain(2.5)
    send_line(user, settle=2.0)      # 用户名
    send_line(password, settle=2.5)  # 密码（空）

    # 验证是否真的进去了
    mark = len("".join(collected))
    send_line("echo __OK__", settle=2.5)
    tail = "".join(collected)[mark:]

    if "incorrect" in tail.lower() or "denied" in tail.lower():
        raise OSError("登录被拒绝——用户名或密码不对")
    if "__OK__" not in tail:
        raise OSError(
            f"未能进入 shell（探测标记未回显）。原始回显：{tail[-200:]!r}")

    send_line("clear")

    for cmd in commands:
        send_line(cmd)

    sock.sendall(b"exit\r\n")
    drain(0.8)

    sock.close()
    return "".join(collected)


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1

    ip = args.pop(0)
    user = os.environ.get("TELNET_USER", DEFAULT_USER)
    password = os.environ.get("TELNET_PASS", "")
    rounds = 2
    script_file = None
    cmd_words = []

    i = 0
    while i < len(args):
        a = args[i]
        if a == "--user" and i + 1 < len(args):
            user = args[i + 1]; i += 2
        elif a == "--pass" and i + 1 < len(args):
            password = args[i + 1]; i += 2
        elif a == "--rounds" and i + 1 < len(args):
            rounds = int(args[i + 1]); i += 2
        elif a == "--script" and i + 1 < len(args):
            script_file = args[i + 1]; i += 2
        else:
            cmd_words.append(a); i += 1

    if script_file:
        with open(script_file, encoding="utf-8") as f:
            cmds = [ln.strip() for ln in f
                    if ln.strip() and not ln.lstrip().startswith("#")]
    elif cmd_words:
        cmds = [" ".join(cmd_words)]
    else:
        print(__doc__)
        return 1

    try:
        print(telnet_exec(ip, cmds, user, password, rounds))
    except OSError as e:
        print(f"[!] {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
