# dev/ — S1E 自研程序工作区

在作者闭源程序基础上继续开发的工作区。所有代码原创、可自由修改。

## 背景：项目开源了什么

上游 `oopuuu/AqaraS1E-Custom-GUI` 只发布二进制，核心引擎无公开源码。本工作区的定位是
**用可读源码重新实现作者闭源的组件**，按投入产出排序逐步推进。

| 组件 | 大小 | 难度 | 说明 |
|---|---|---|---|
| `snap_fast` | 7.6 KB | ⭐ | framebuffer 截图 → BMP |
| `http_body_strip` | 5.8 KB | ⭐ | HTTP 响应剥头取 body |
| `safe_png_recv` | 7.2 KB | ⭐ | 通知图片安全接收校验 |
| `api.cgi` | 76 KB | ⭐⭐⭐ | Web 后端，约 80 个 action |
| `ha_daemon` | 46 KB | ⭐⭐ | HA WebSocket 桥接（协议公开，非逆向） |
| `s1e_standalone_app` | 521 KB | ⭐⭐⭐⭐⭐ | 60FPS 渲染引擎（约 2 万行 C） |

**建议顺序**：先做工具类验证工具链 → 再做 `api.cgi`（最有价值，可顺带补掉无认证
`exec_cmd` 漏洞）→ 再做 `ha_daemon` → 渲染引擎走"并存接管"路线，不整体重写。

## 设备接口清单（从作者二进制逆向得到）

写程序直接照这个来，不用再逆向：

```
显示    /dev/fb0，FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO
        FBIOPAN_DISPLAY / FBIOPUT_FSCREENINFO
        或 SigmaStar MI_DISP_SetPubAttr / MI_PANEL_Init（dlopen，回退到 fb0）
触摸    read() /dev/input/event0 和 /dev/input/event1
背光    /sys/class/pwm/pwmchip0/pwm0/{period,duty_cycle,enable}
蜂鸣器  /sys/devices/soc0/soc/soc:beeper/play
温度    /dev/sar，/sys/devices/system/cpu/cpufreq/temp_out
功率    /sys/devices/soc0/soc/soc:hlw8012/{power_mw,current_ua}
开关    /bin/ubus call switch set.state '{"id":1,"state":1}'
Wi-Fi   wpa_cli -i wlan0
IPC     /tmp/* 标志文件约定（作者整套设计建立在此之上）
```

## 硬约束：内存

设备空闲内存约 14 MB，作者程序常驻 1.2 MB，`post_init.sh` 在空闲内存低于 14 MB 时会
触发内存回收（清日志、清缓存、`drop_caches`）。

**新程序常驻应控制在 2 MB 以内。** 不要开 malloc arena 堆，不要多线程各持大缓冲。

## 编译目标

```
架构    armv7-a hard-float（设备是双核 Cortex-A7，带 NEON）
链接    动态（默认）或静态，见下
解释器  /lib/ld-linux-armhf.so.3  ← 与作者二进制完全一致
```

**为什么默认动态链接**：实测本工程产物和作者闭源二进制**都只引用 `GLIBC_2.4`**，
所以动态版本在设备上同样安全，而体积小 40 倍：

| 程序 | 动态 | 静态 | 作者原版 |
|---|---|---|---|
| `hello_fb` | 9.7 KB | 411 KB | — |
| `snap_fast` | 9.7 KB | 406 KB | 7.6 KB |
| `http_body_strip` | 5.5 KB | 406 KB | 5.8 KB |

静态链接仅在设备 libc 有异常时作为退路（`make static`）。
注意静态 glibc 会把每个程序撑到 400KB+，靠 `--gc-sections` 也压不下来。

## 已验证状态

| 项 | 状态 |
|---|---|
| 交叉编译产出 ARM ELF | ✅ 零警告，解释器 `/lib/ld-linux-armhf.so.3` |
| 设备兼容性 | ✅ 设备 glibc 2.30，产物仅需 2.4 |
| `http_body_strip` | ✅ 7/7 单元测试 + 端到端逐字节一致 |
| `safe_png_recv` | ✅ 22/22 单元测试 + 真机 5 条路径全对 |
| `hello_fb` | ✅ 真机 probe 成功 |
| `snap_fast` | ✅ 真机抓屏，与作者输出 **99.56% 逐像素一致**，差异仅时钟区域 (36×54) |

测试：`sh scripts/run_tests.sh`、`python3 scripts/test_png_recv.py`（均不需要设备）

## 设备实测参数（Magic-Switch-S1E-E420 实机）

```
Linux 4.9.84 armv7l · 双核 Cortex-A7 · glibc 2.30 · 内存 55.5MB(空闲 ~15MB)
fb0: 480x480 可见 / 480x960 虚拟(双缓冲) / yoffset=480
     32bpp BGRA (B@0 G@8 R@16 A@24) / line_length=1920 / smem=1843200
触控: /dev/input/event0, event1
```

**mmap 必须覆盖整个 smem_len**，不能只算 `line_length * yres`。
加上 yoffset 后单算可见区会越界一倍——这是 `snap_fast` 首个版本的真机 bug。

## 与作者版的差异

| 程序 | 本实现 | 相对作者 |
|---|---|---|
| `snap_fast` | 24bpp BMP，按内核位域逐像素转换 | 作者用 32bpp（同样合法），本实现兼容性和平更好 |
| `safe_png_recv` | 遍历 chunk 定位 IHDR + 校验 IEND | 作者只看前 24 字节，截断文件会被放行 |
| `http_body_strip` | 滑动窗口匹配 + chunked 解码 | 作者为 5 状态字节机，跨缓冲区有错位风险 |

退出码、静默行为、参数默认值全部与作者版一致，可直接替换。

## 目录结构

```
dev/
├── Dockerfile            交叉编译容器（当前网络下 Docker Hub 不可达，仅作备选）
├── Makefile              构建规则
├── build.sh              在容器内执行 make（Docker 备选路径）
├── src/
│   ├── hello_fb.c        framebuffer 能力探针 + 最小绘制验证
│   ├── snap_fast.c       截图工具（作者闭源版的替代实现）
│   └── http_body_strip.c HTTP 剥头工具（作者闭源版的替代实现）
├── scripts/
│   ├── probe.sh          只读探测设备环境
│   ├── deploy.sh         部署（默认 dry-run，--run 才执行）
│   ├── remove.sh         卸载/回滚（默认 dry-run）
│   └── run_tests.sh      本机单元测试，不需要设备
└── build/
    ├── dynamic/          默认产物
    └── static/           退路产物
```

**工具链**：Homebrew `messense/macos-cross-toolchains/armv7-unknown-linux-gnueabihf`（GCC 15.2.0）。
Docker 方案保留但当前网络连不上 Docker Hub。

## 安全模型（务必先读）

这台设备是作者的运行环境，**我们的代码绝不能破坏它**。为此设了硬性约束：

### 绝不触碰的路径

```
/data/scripts/ 下作者的任何一个文件（index.html、*.json、post_init.sh、*.cgi…）
/data/app/            原厂配置
/misc  /etc  /boot    系统分区
任何守护进程（s1e_standalone_app、ha_daemon、httpd、ubusd、telnetd…）
```

我们的所有文件只放在 **`/data/scripts/dev/`** 这一个隔离目录里。

### 脚本的三条铁律

1. **默认 dry-run。** `deploy.sh` 和 `remove.sh` 不加 `--run` 只打印计划，不修改任何东西
2. **先验证再 chmod。** 上传后实测文件落在哪（不猜路径）、比对大小一致，才赋予执行权限
3. **任何部署都有干净回滚。** `sh scripts/remove.sh <IP>` 只删自己的目录，不动其他

### 各操作的实际风险

| 操作 | 风险 | 说明 |
|---|---|---|
| `probe.sh` | **无** | 纯只读 |
| `hello_fb probe` | **无** | 只 ioctl 查询，不写显存 |
| `deploy.sh` | 极低 | 只在 dev/ 写文件 |
| `hello_fb paint/rect` | 低，瞬时可见 | 写 `/dev/fb0`。引擎每帧重绘，内容会立即被覆盖；即使引擎停了，看门狗 5 秒内也会拉起。**不会损坏系统** |
| `remove.sh` | 极低 | 精确路径删除，不用 glob |

**真正的变砖风险**（改配置、kill 守护进程、写 `/misc`、动 `/data/app`）本工作区一律不做。
若将来确实需要改 `post_init.sh`，必须先备份并在设备外留副本。

## 快速开始

```sh
# 1. 编译
make

# 2. 只读探测设备（安全，不修改任何东西）
sh scripts/probe.sh 192.168.1.100

# 3. 预览部署计划（dry-run）
sh scripts/deploy.sh 192.168.1.100 hello_fb

# 4. 确认无误后实际部署
sh scripts/deploy.sh 192.168.1.100 hello_fb --run

# 5. 运行只读探针
curl -s -G "http://192.168.1.100:8080/cgi-bin/api.cgi" \
     --data-urlencode "action=exec_cmd" \
     --data-urlencode "cmd=/data/scripts/dev/hello_fb probe"

# 6. 回滚
sh scripts/remove.sh 192.168.1.100 --run
```

## 与作者程序共存

引擎只是往 `/dev/fb0` 写像素的进程，不独占设备。接管方式：

```
平时   → 作者引擎正常运行
接管时 → kill 引擎 → 打开 /dev/fb0 → 画自己的界面 → 交还
```

注意 `post_init.sh` 每 5 秒检查一次 `s1e_standalone_app` 并自动重启，
要接管需先改 `post_init.sh`（该文件是开源的文本脚本，可自由修改）。