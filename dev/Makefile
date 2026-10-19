# S1E 交叉编译
#
# 编译器自动探测，优先级：
#   1. 本机 Homebrew 交叉工具链（最快，无需 Docker）
#   2. 系统 PATH 里的 arm-linux-gnueabihf-gcc
#   3. 都没有则报错，提示用 ./build.sh 走 Docker
#
# 静态链接是刻意选择：设备 glibc 偏旧（作者二进制最高只引用 GLIBC_2.17），
# 动态链接会引用设备不存在的符号，导致 "version `GLIBC_2.xx' not found"。
# 静态链接彻底消除这个依赖问题，作者自带的 curl 工具也是静态的。

BREW_PREFIX := $(HOME)/opt/armv7-unknown-linux-gnueabihf/bin

CROSS_CC ?= $(shell \
    if [ -x "$(BREW_PREFIX)/armv7-linux-gnueabihf-gcc" ]; then \
        echo "$(BREW_PREFIX)/armv7-linux-gnueabihf-gcc"; \
    elif command -v arm-linux-gnueabihf-gcc >/dev/null 2>&1; then \
        echo arm-linux-gnueabihf-gcc; \
    elif command -v armv7-linux-gnueabihf-gcc >/dev/null 2>&1; then \
        echo armv7-linux-gnueabihf-gcc; \
    else echo ""; fi)

# strip / readelf 跟随编译器前缀推导
CROSS_BIN    := $(basename $(CROSS_CC))
CROSS_STRIP  ?= $(patsubst %gcc,%strip,$(CROSS_CC))
CROSS_READELF?= $(patsubst %gcc,%readelf,$(CROSS_CC))

# Cortex-A7 带 NEON，但用 -mfpu=vfpv3-d16 更保守；确定设备支持 NEON 后可改回 neon
TARGET_ARCH := -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard

# 体积优化：函数/数据分段 + 链接期回收未引用段 + 剥符号。
# 静态链接 glibc 本体会让每个程序膨胀到 400KB+，靠 gc-sections 能砍掉大部分。
SIZE_FLAGS := -Os -ffunction-sections -fdata-sections
SIZE_LDF   := -Wl,--gc-sections -Wl,--as-needed

WARN := -Wall -Wextra -Wno-unused-parameter

# LINK_MODE=dynamic（默认）或 static
#
# 实测两者都只引用 GLIBC_2.4，与作者闭源二进制完全一致，
# 所以动态链接在设备上同样安全，且体积小 40 倍（5-10KB vs 410KB）。
# 静态链接仅在设备 libc 有异常时作为退路。
LINK_MODE ?= dynamic

ifeq ($(LINK_MODE),static)
  CFLAGS  := $(TARGET_ARCH) -O2 $(SIZE_FLAGS) -std=c99 -D_GNU_SOURCE $(WARN) -static
  LDFLAGS := -static $(SIZE_LDF)
else
  CFLAGS  := $(TARGET_ARCH) -O2 $(SIZE_FLAGS) -std=c99 -D_GNU_SOURCE $(WARN)
  LDFLAGS := $(SIZE_LDF)
endif

SRCDIR   := src
BUILDDIR := build/$(LINK_MODE)

PROGS := hello_fb snap_fast http_body_strip safe_png_recv

# 摄像头程序由多个编译单元组成，需要单独规则
CAM_DIR   := $(SRCDIR)/camera
CAM_PROGS := cam_view touch_probe cam_probe

.PHONY: all clean check-toolchain inspect static dynamic test

all: check-toolchain dynamic

static:
	@$(MAKE) --no-print-directory LINK_MODE=static all-progs

dynamic:
	@$(MAKE) --no-print-directory LINK_MODE=dynamic all-progs

.PHONY: all-progs
all-progs: $(addprefix $(BUILDDIR)/,$(PROGS)) $(addprefix $(BUILDDIR)/,$(CAM_PROGS))

check-toolchain:
	@if [ -z "$(CROSS_CC)" ]; then \
	  echo "找不到 ARM 交叉编译器。"; \
	  echo "  macOS:  brew install messense/macos-cross-toolchains/armv7-unknown-linux-gnueabihf"; \
	  echo "  或走 Docker: ./build.sh"; \
	  exit 1; fi
	@$(CROSS_CC) --version | head -1

$(BUILDDIR)/%: $(SRCDIR)/%.c
	@mkdir -p $(BUILDDIR)
	$(CROSS_CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@$(CROSS_STRIP) $@ 2>/dev/null || true
	@echo "==> $@"
	@file $@ | sed 's/^/    /'
	@ls -l $@ | awk '{print "    大小: " $$5 " 字节"}'

# cam_view = 主程序 + HTTP + 配置解析 + framebuffer + 触摸翻页 + stb JPEG 解码
$(BUILDDIR)/cam_view: $(CAM_DIR)/cam_view.c $(CAM_DIR)/s1e_http.c \
                      $(CAM_DIR)/s1e_conf.c $(CAM_DIR)/s1e_fb.c \
                      $(CAM_DIR)/s1e_touch.c $(CAM_DIR)/stb_jpeg_impl.c
	@mkdir -p $(BUILDDIR)
	$(CROSS_CC) $(CFLAGS) -I$(CAM_DIR) \
	    $(CAM_DIR)/cam_view.c $(CAM_DIR)/s1e_http.c $(CAM_DIR)/s1e_conf.c \
	    $(CAM_DIR)/s1e_fb.c $(CAM_DIR)/s1e_touch.c $(CAM_DIR)/stb_jpeg_impl.c \
	    -o $@ $(LDFLAGS)
	@$(CROSS_STRIP) $@ 2>/dev/null || true
	@echo "==> $@"
	@file $@ | sed 's/^/    /'
	@ls -l $@ | awk '{print "    大小: " $$5 " 字节"}'

# 触摸探针 / 摄像头探针：不涉及 stb，单独规则
$(BUILDDIR)/touch_probe: $(CAM_DIR)/touch_probe.c
	@mkdir -p $(BUILDDIR)
	$(CROSS_CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@$(CROSS_STRIP) $@ 2>/dev/null || true
	@echo "==> $@"
	@ls -l $@ | awk '{print "    大小: " $$5 " 字节"}'

$(BUILDDIR)/cam_probe: $(CAM_DIR)/cam_probe.c $(CAM_DIR)/s1e_http.c $(CAM_DIR)/s1e_conf.c
	@mkdir -p $(BUILDDIR)
	$(CROSS_CC) $(CFLAGS) -I$(CAM_DIR) \
	    $(CAM_DIR)/cam_probe.c $(CAM_DIR)/s1e_http.c $(CAM_DIR)/s1e_conf.c \
	    -o $@ $(LDFLAGS)
	@$(CROSS_STRIP) $@ 2>/dev/null || true
	@echo "==> $@"
	@ls -l $@ | awk '{print "    大小: " $$5 " 字节"}'

# 本机测试（不需要设备）
test:
	@sh scripts/run_tests.sh
	@python3 scripts/test_png_recv.py
	@python3 scripts/test_http.py

# 打印产物的架构与链接信息，用于确认能跑在设备上
# 用 readelf（交叉工具链自带，Linux/CI 上可用），不依赖 macOS 的 otool
inspect: $(addprefix $(BUILDDIR)/,$(PROGS))
	@for p in $(addprefix $(BUILDDIR)/,$(PROGS)); do \
	  echo "=== $$p ==="; \
	  file $$p | sed 's/^/  /'; \
	  if command -v $(CROSS_READELF) >/dev/null 2>&1; then \
	    printf '  解释器: '; \
	    $(CROSS_READELF) -l $$p 2>/dev/null \
	      | sed -n 's/.*program interpreter: \(.*\)\]/\1/p'; \
	    printf '  依赖  : '; \
	    $(CROSS_READELF) -d $$p 2>/dev/null \
	      | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' '; echo; \
	  fi; \
	done

clean:
	rm -rf build