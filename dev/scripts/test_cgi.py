#!/usr/bin/env python3
"""
cam_cgi 的集成测试。

做法是把 cam_cgi 编成本机可执行版本，通过真实 CGI 接口驱动它：
设环境变量（QUERY_STRING / HTTP_COOKIE / CONTENT_LENGTH）、
喂 stdin、检查输出。这样测的是真程序，不是复制一份逻辑。

覆盖的用例每一条都对应一个真实踩过的坑：

  实体列表带完整域前缀        ← 漏 "camera." 前缀，下拉框存了无效实体
  中文与特殊字符的 JSON 转义  ← 友好名破坏 JSON
  保存后所有字段都能读回      ← write_default 只写 entity，其余写死
  总页数按布局推导            ← 引擎页数 = 屏数 + 1，填错导致花屏
  鉴权：无 cookie / 错 cookie 都拒绝
  鉴权失败返回 401 而非 200
  不发 CORS 头                ← 跨源页面能读走门口影像
  snapshot 输出 JPEG
  范围钳位

用法：./scripts/test_cgi.py
"""

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.join(HERE, "..", "src", "camera")
BIN = "/tmp/cam_cgi_test"

passed = failed = 0
FAKE_HA_PORT = 18123


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ✓ {name}")
    else:
        failed += 1
        print(f"  ✗ {name}")
        if detail:
            print(f"      {detail}")


def eq(name, expect, actual):
    check(name, expect == actual, f"期望 {expect!r}，实际 {actual!r}")


# --------------------------------------------------------------- 假 HA 服务

JPEG = b"\xff\xd8\xff\xe0" + b"\x00" * 200 + b"\xff\xd9"

STATES = json.dumps([
    {"entity_id": "camera.chuangmi_ipc018_acd6_video_doorbell",
     "state": "idle",
     "attributes": {"friendly_name": "智能猫眼 Video Doorbell"}},
    {"entity_id": "camera.isa_hlc7_448d_camera_control",
     "state": "idle",
     "attributes": {"friendly_name": '带"引号"的猫眼'}},
    # 非 camera 域，必须被排除
    {"entity_id": "light.living_room",
     "state": "on",
     "attributes": {"friendly_name": "客厅灯"}},
], ensure_ascii=False).encode("utf-8")


class FakeHA(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        auth = self.headers.get("Authorization", "")
        if self.path == "/api/states":
            body = STATES
            ctype = "application/json"
        elif self.path.startswith("/api/camera_proxy/"):
            body = JPEG
            ctype = "image/jpeg"
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def start_fake_ha():
    srv = HTTPServer(("127.0.0.1", FAKE_HA_PORT), FakeHA)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    time.sleep(0.3)
    return srv


# --------------------------------------------------------------- 测试夹具

def layout(screens):
    """造一个 layout_p0.json：每块屏恰好一个 components 键。"""
    return json.dumps({
        "version": 2,
        "screens": [
            {"id": f"s{i+1}", "name": f"屏{i+1}",
             "grid": {"cols": 3, "rows": 3},
             "components": [{"id": f"c{i}", "type": "clock"}]}
            for i in range(screens)
        ],
    }).encode("utf-8")


class Env:
    def __init__(self, tmp):
        self.tmp = tmp
        self.ha = os.path.join(tmp, "ha_config.json")
        self.cam = os.path.join(tmp, "cam_config.json")
        self.layout = os.path.join(tmp, "layout_p0.json")
        self.secret = os.path.join(tmp, "cam_secret")
        self.page = os.path.join(tmp, "cam_settings.html")

        with open(self.ha, "w") as f:
            json.dump({"ha_url": f"http://127.0.0.1:{FAKE_HA_PORT}",
                       "ha_token": "TESTTOKEN"}, f)
        with open(self.layout, "wb") as f:
            f.write(layout(4))
        with open(self.secret, "w") as f:
            f.write("TESTSECRETKEY1234")
        with open(self.page, "w") as f:
            f.write("<html>settings</html>")

    def env(self, **extra):
        e = dict(os.environ)
        e.update({
            "S1E_T_HA": self.ha,
            "S1E_T_CAMCFG": self.cam,
            "S1E_T_LAYOUT": self.layout,
            "S1E_T_SECRET": self.secret,
            "S1E_T_PAGE": self.page,
        })
        e.update({k: str(v) for k, v in extra.items()})
        return e

    def cookie(self):
        return f"cams=TESTSECRETKEY1234; Path=/"


def cgi(env, query="", cookie=None, post=None):
    """按 CGI 协议跑一次 cam_cgi，返回 (原始输出, 状态码, body)。"""
    e = env.env()
    e["QUERY_STRING"] = query
    if cookie:
        e["HTTP_COOKIE"] = cookie
    if post is not None:
        e["CONTENT_LENGTH"] = str(len(post.encode()))
    p = subprocess.run([BIN], input=(post or "").encode(), env=e,
                       capture_output=True, timeout=30)
    raw = p.stdout
    status = 200
    head, _, body = raw.partition(b"\r\n\r\n")
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"status:"):
            status = int(line.split(b":", 1)[1].split()[0])
    return raw, status, body


def main() -> int:
    print("══ 编译本机测试版 ══")
    r = subprocess.run(
        ["cc", "-O1", "-Wall", "-Wextra", "-I", SRC_DIR, "-o", BIN,
         os.path.join(SRC_DIR, "cam_cgi.c"),
         os.path.join(SRC_DIR, "s1e_http.c"),
         os.path.join(SRC_DIR, "s1e_conf.c")],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("  ✗ 编译失败:")
        print(r.stderr)
        return 1
    print("  ✓ 编译通过")

    start_fake_ha()

    with tempfile.TemporaryDirectory() as tmp:
        env = Env(tmp)
        print()
        print("══ 鉴权 ══")
        _, st, body = cgi(env, "action=get")
        eq("无 cookie 时返回 401", 401, st)
        check("无 cookie 时响应体是错误 JSON",
              json.loads(body).get("status") == "error", body[:120])

        _, st, _ = cgi(env, "action=snapshot", cookie="cams=WRONGKEY")
        eq("错误 cookie 返回 401", 401, st)

        _, st, _ = cgi(env, "action=snapshot",
                       cookie="other=1; cams=TESTSECRETKEY1234; x=2")
        eq("cookie 在中间也能识别", 200, st)

        raw, st, body = cgi(env, "action=page")
        eq("page 动作无需密钥", 200, st)
        head = raw.split(b"\r\n\r\n", 1)[0]
        check("page 下发 HttpOnly Cookie",
              b"Set-Cookie: cams=" in head and b"HttpOnly" in head)
        check("page 不发 CORS 头", b"Access-Control-Allow-Origin" not in head)

        print()
        print("══ 实体列表（曾经漏掉域前缀）══")
        _, st, body = cgi(env, "action=list", cookie=env.cookie())
        eq("返回 200", 200, st)
        try:
            items = json.loads(body)
        except Exception as e:
            print(f"  ✗ 响应不是合法 JSON: {e}")
            print(f"      {body[:300]}")
            return 1
        ids = [i["entity_id"] for i in items]
        eq("camera 域实体数", 2, len(items))
        check("实体带完整 camera. 前缀",
              all(i.startswith("camera.") for i in ids), ids)
        check("包含 chuangmi 猫眼",
              "camera.chuangmi_ipc018_acd6_video_doorbell" in ids, ids)
        check("包含 isa_hlc7 猫眼",
              "camera.isa_hlc7_448d_camera_control" in ids, ids)
        check("非 camera 域被排除", "light.living_room" not in ids, ids)
        names = {i["entity_id"]: i["name"] for i in items}
        eq("中文友好名正确",
           "智能猫眼 Video Doorbell",
           names.get("camera.chuangmi_ipc018_acd6_video_doorbell"))
        eq("含引号的名字转义后能还原",
           '带"引号"的猫眼',
           names.get("camera.isa_hlc7_448d_camera_control"))

        print()
        print("══ 保存配置（曾经丢掉除 entity 外的字段）══")
        payload = json.dumps({
            "entity": "camera.isa_hlc7_448d_camera_control",
            "width": 960, "height": 540, "fps": 5,
            "camera_page": 3, "total_pages": 99,   # 故意填错，应被推导覆盖
        })
        _, st, body = cgi(env, "action=save", cookie=env.cookie(),
                          post=payload)
        eq("保存返回 200", 200, st)
        r = json.loads(body)
        eq("entity 写入正确", "camera.isa_hlc7_448d_camera_control", r["entity"])
        eq("width 写入正确", 960, r["width"])
        eq("height 写入正确", 540, r["height"])
        eq("fps 写入正确", 5, r["fps"])
        eq("camera_page 写入正确", 3, r["camera_page"])
        eq("total_pages 按布局推导(4屏+1)", 5, r["total_pages"])

        # 再读回来，确认真的落到文件里了（而不是只在响应里好看）
        _, st, body = cgi(env, "action=get", cookie=env.cookie())
        back = json.loads(body)
        eq("重新读取 entity 一致", r["entity"], back["entity"])
        eq("重新读取 width 一致", 960, back["width"])
        eq("重新读取 height 一致", 540, back["height"])
        eq("重新读取 fps 一致", 5, back["fps"])
        eq("重新读取 camera_page 一致", 3, back["camera_page"])
        eq("重新读取 total_pages 一致", 5, back["total_pages"])

        # 直接看文件内容，确认字段真的在里面
        raw = open(env.cam, encoding="utf-8").read()
        check("配置文件里含 camera_page 3", '"camera_page": 3' in raw, raw)
        check("配置文件里含 fps 5", '"fps": 5' in raw, raw)

        print()
        print("══ 总页数推导（引擎页数 = 屏数 + 1）══")
        for screens, expect in [(1, 2), (3, 4), (4, 5), (8, 9)]:
            with open(env.layout, "wb") as f:
                f.write(layout(screens))
            _, _, body = cgi(env, "action=get", cookie=env.cookie())
            eq(f"{screens} 屏 -> {expect} 页", expect,
               json.loads(body)["total_pages"])

        with open(env.layout, "wb") as f:
            f.write(layout(4))
        os.remove(env.layout)
        _, _, body = cgi(env, "action=get", cookie=env.cookie())
        got = json.loads(body)
        eq("布局缺失时标为非自动", False, got["auto_pages"])
        eq("布局缺失时 screens 报 -1", -1, got["screens"])

        print()
        print("══ 范围钳位 ══")
        _, _, body = cgi(env, "action=save", cookie=env.cookie(),
                         post=json.dumps({"entity": "camera.x",
                                          "width": 99999, "height": 1,
                                          "fps": 500, "camera_page": 99}))
        r2 = json.loads(body)
        # 越界时回落到默认值而不是钳到边界——默认值是已知可用的组合
        eq("width 超上限回落到默认", 640, r2["width"])
        eq("height 过小回落到默认", 480, r2["height"])
        eq("fps 超上限回落到默认", 3, r2["fps"])
        eq("camera_page 超过总页数被钳", r2["total_pages"] - 1, r2["camera_page"])

        print()
        print("══ snapshot ══")
        _, st, body = cgi(env, "action=snapshot", cookie=env.cookie())
        eq("返回 200", 200, st)
        check("输出是 JPEG", body[:3] == b"\xff\xd8\xff", body[:8].hex())

        print()
        print("══ 未知动作 ══")
        _, st, _ = cgi(env, "action=bogus", cookie=env.cookie())
        check("未知动作报错", st in (200, 400))

    print()
    print("══ 结果 ══")
    print(f"  通过 {passed} / 失败 {failed}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
