#!/bin/sh
# http_body_strip 的本机回归测试
#
# 用本机编译器构建一份 x86/macOS 版本，喂构造的 HTTP 响应验证解析逻辑。
# 不需要设备，也不需要 Docker。
#
# 用法：./scripts/run_tests.sh

set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

ok()   { echo "  ✓ $1"; PASS=$((PASS+1)); }
bad()  { echo "  ✗ $1"; echo "      期望: $2"; echo "      实际: $3"; FAIL=$((FAIL+1)); }

check() {
    name="$1"; expect="$2"; actual="$3"
    if [ "$expect" = "$actual" ]; then ok "$name"; else bad "$name" "$expect" "$actual"; fi
}

echo "══ 编译本机测试版 ══"
cc -O2 -Wall -Wextra -o "$TMP/hbs" "$DIR/src/http_body_strip.c"
echo "  ✓ 编译通过"
echo

echo "══ 解析逻辑测试 ══"

# 1. 标准响应：头 + body
printf 'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{"ok":true}' \
    | "$TMP/hbs" > "$TMP/o1"
check "标准 HTTP 响应" '{"ok":true}' "$(cat "$TMP/o1")"

# 2. 空 body（204）
printf 'HTTP/1.1 204 No Content\r\nX-A: b\r\n\r\n' | "$TMP/hbs" > "$TMP/o2"
check "空 body 无输出" "" "$(cat "$TMP/o2")"

# 3. body 内含 \r\n，不应被误当作头部结束
# 用 cmp 做字节比对：body 里含字面 \r，命令替换会残留 \r 导致误判
printf 'line1\r\nline2\r\n' > "$TMP/exp3"
printf 'HTTP/1.1 200 OK\r\n\r\nline1\r\nline2\r\n' | "$TMP/hbs" > "$TMP/o3"
if cmp -s "$TMP/exp3" "$TMP/o3"; then
    ok "body 内含 CRLF 不被误截断"
else
    bad "body 内含 CRLF 不被误截断" "逐字节一致" "$(od -c "$TMP/o3" | head -2)"
fi

# 4. 头部跨多次 read（管道场景）：分两批送入
{ printf 'HTTP/1.1 200 OK\r\nContent-Len'; sleep 0.3; printf 'gth: 5\r\n\r\nhello'; } \
    | "$TMP/hbs" > "$TMP/o4"
check "头部跨缓冲区边界" "hello" "$(cat "$TMP/o4")"

# 5. body 晚于头部到达（curl/openssl 常见行为）
{ printf 'HTTP/1.1 200 OK\r\n\r\n'; sleep 0.3; printf 'delayed-body'; } \
    | "$TMP/hbs" > "$TMP/o5"
check "body 延迟到达" "delayed-body" "$(cat "$TMP/o5")"

# 6. chunked 解码
printf 'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nHello\r\n6\r\n World\r\n0\r\n\r\n' \
    | "$TMP/hbs" -d > "$TMP/o6"
check "chunked 解码" "Hello World" "$(cat "$TMP/o6")"

# 7. 二进制 body 不被破坏（用 tarball 自身做样本）
head -c 5000 "$DIR/../release/s1e_custom_gui_latest.tar.gz" > "$TMP/payload.bin"
{ printf 'HTTP/1.1 200 OK\r\n\r\n'; cat "$TMP/payload.bin"; } | "$TMP/hbs" > "$TMP/o7"
if cmp -s "$TMP/payload.bin" "$TMP/o7"; then
    ok "5000 字节二进制 body 完整"
else
    bad "5000 字节二进制 body 完整" "逐字节一致" "$(cmp "$TMP/payload.bin" "$TMP/o7" 2>&1)"
fi

echo
echo "══ 结果 ══"
echo "  通过 $PASS / 失败 $FAIL"
[ "$FAIL" -eq 0 ] || exit 1