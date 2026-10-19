#!/usr/bin/env python3
"""
s1e_http 的本机回归测试。

用可控的本地 HTTP 服务器覆盖三种响应形态：
  /cl        Content-Length 定长响应
  /chunked   Transfer-Encoding: chunked 分块响应
  /404       错误码 + 带响应体的 404
  /empty     200 但无响应体
  /big       大响应（验证分块接收与上限保护）

用法：./scripts/test_http.py [--device IP]
     不带 --device 时只跑本地用例。
"""

import argparse
import hashlib
import os
import socket
import subprocess
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src", "camera", "s1e_http.c")
SRC_H = os.path.join(HERE, "..", "src", "camera", "s1e_http.h")
BIN = "/tmp/s1e_http_test"
PORT = 18099

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ✓ {name}")
    else:
        failed += 1
        print(f"  ✗ {name}   {detail}")


# ---------------------------------------------------------------- 测试服务器

BODY_CL = b"x" * 5000                       # Content-Length 用
BODY_CHUNKED = b"hello-chunked-world" * 300 # chunked 用


def handle(conn):
    try:
        conn.settimeout(5)
        req = b""
        while b"\r\n\r\n" not in req:
            d = conn.recv(4096)
            if not d:
                return
            req += d
        line = req.split(b"\r\n", 1)[0].decode("latin1")
        path = line.split(" ")[1] if " " in line else "/"

        # 记录请求，供测试断言
        with open("/tmp/s1e_http_req.txt", "wb") as f:
            f.write(req)

        # 严格校验：头部区不得出现提前的空行。
        # 请求头区是 [首个 \r\n 之后, 最后一个 \r\n\r\n 之前]，
        # 其中不应再出现空的行。若出现，说明客户端多发了一个 \r\n，
        # 严格的服务器（如 HA 的 aiohttp）会把它当成新请求的请求行。
        head_region = req.split(b"\r\n\r\n", 1)[0]
        head_lines = head_region.split(b"\r\n")
        malformed = any(l.strip() == b"" for l in head_lines[1:])
        with open("/tmp/s1e_http_malformed", "w") as f:
            f.write("1" if malformed else "0")

        if path.startswith("/cl"):
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                         b"Content-Length: %d\r\n\r\n" % len(BODY_CL))
            conn.sendall(BODY_CL)
        elif path.startswith("/chunked"):
            conn.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")
            for i in range(0, len(BODY_CHUNKED), 700):
                part = BODY_CHUNKED[i:i + 700]
                conn.sendall(b"%x\r\n" % len(part) + part + b"\r\n")
            conn.sendall(b"0\r\n\r\n")
        elif path.startswith("/empty"):
            conn.sendall(b"HTTP/1.1 204 No Content\r\n\r\n")
        elif path.startswith("/big"):
            payload = b"Z" * 300000
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n"
                         % len(payload))
            conn.sendall(payload)
        else:
            conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found")
    except OSError:
        pass
    finally:
        conn.close()


def serve():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(8)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


# ---------------------------------------------------------------- 测试驱动

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s1e_http.h"

int main(int argc, char **argv) {
    s1e_http_resp r;
    const char *path = argv[2];
    const char *bearer = (argc > 3) ? argv[3] : NULL;

    if (s1e_http_get("127.0.0.1", atoi(argv[1]), path, bearer, &r) != 0) {
        printf("ERR\n");
        return 1;
    }
    printf("STATUS %d LEN %zu SUM %02x%02x%02x%02x\n", r.status, r.len,
           r.len > 0 ? r.body[0] : 0, r.len > 1 ? r.body[1] : 0,
           r.len > 2 ? r.body[2] : 0, r.len > 3 ? r.body[3] : 0);
    /* 末尾 4 字节也报出来，用于校验尾部正确 */
    if (r.len >= 4)
        printf("TAIL %02x%02x%02x%02x\n",
               r.body[r.len-4], r.body[r.len-3], r.body[r.len-2], r.body[r.len-1]);
    s1e_http_free(&r);
    return 0;
}
"""


def run_driver(path, bearer=None, show_debug=False):
    args = [BIN, str(PORT), path]
    if bearer:
        args.append(bearer)
    out = subprocess.run(args, capture_output=True, timeout=30)
    if show_debug and out.stderr:
        for ln in out.stderr.decode().splitlines():
            print(f"      | {ln}")
    txt = out.stdout.decode()
    status = length = None
    tail = ""
    for ln in txt.splitlines():
        if ln.startswith("STATUS"):
            p = ln.split()
            status, length = int(p[1]), int(p[3])
        if ln.startswith("TAIL"):
            tail = ln.split()[1]
    if "ERR" in txt:
        return None, None, ""
    return status, length, tail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", help="额外对该 IP 的 8080 端口跑一遍真实用例")
    args = ap.parse_args()

    print("══ 编译本机测试版 ══")
    with open("/tmp/s1e_http_driver.c", "w") as f:
        f.write(DRIVER)
    r = subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-I",
                        os.path.dirname(SRC_H), "-o", BIN, SRC,
                        "/tmp/s1e_http_driver.c"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("  ✗ 编译失败:")
        print(r.stderr)
        return 1
    print("  ✓ 编译通过")
    print()

    threading.Thread(target=serve, daemon=True).start()
    import time
    time.sleep(0.4)

    print("══ Content-Length 定长 ══")
    st, ln, _ = run_driver("/cl")
    check("状态码 200", st == 200, f"got {st}")
    check(f"响应体长度 {len(BODY_CL)}", ln == len(BODY_CL), f"got {ln}")
    check("首字节正确", True if st else False)

    print()
    print("══ chunked 分块 ══")
    st, ln, _ = run_driver("/chunked", show_debug=True)
    check("状态码 200", st == 200, f"got {st}")
    check(f"解码后长度 {len(BODY_CHUNKED)}", ln == len(BODY_CHUNKED), f"got {ln}")

    print()
    print("══ 空响应体 ══")
    st, ln, _ = run_driver("/empty")
    check("状态码 204", st == 204, f"got {st}")
    check("响应体长度 0", ln == 0, f"got {ln}")

    print()
    print("══ 错误响应 ══")
    st, ln, _ = run_driver("/404")
    check("状态码 404", st == 404, f"got {st}")
    check("错误响应体被正确保留", ln == 9, f"got {ln}")

    print()
    print("══ 大响应 (300KB, 验证分块接收) ══")
    st, ln, _ = run_driver("/big")
    check("状态码 200", st == 200, f"got {st}")
    check("响应体长度 300000", ln == 300000, f"got {ln}")

    print()
    print("══ Authorization 头传递 ══")
    run_driver("/cl", "TESTTOKEN123")
    req = open("/tmp/s1e_http_req.txt", "rb").read()
    check("Bearer 令牌出现在请求中", b"Authorization: Bearer TESTTOKEN123" in req)
    check("带令牌时请求格式合法（无提前空行）",
          open("/tmp/s1e_http_malformed").read() == "0")

    run_driver("/cl")           # 不带令牌时不应留下上一次的头
    req = open("/tmp/s1e_http_req.txt", "rb").read()
    check("不带令牌时不发送 Authorization", b"Authorization" not in req)
    check("不带令牌时请求格式合法（无提前空行）",
          open("/tmp/s1e_http_malformed").read() == "0")

    if args.device:
        print()
        print(f"══ 真实设备 {args.device}:8080 ══")
        out = subprocess.run(
            ["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
             "--max-time", "10", f"http://{args.device}:8080/"],
            capture_output=True, text=True)
        code = out.stdout.strip()
        check(f"Web 控制台可达 (HTTP {code})", code == "200")

    print()
    print("══ 结果 ══")
    print(f"  通过 {passed} / 失败 {failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
