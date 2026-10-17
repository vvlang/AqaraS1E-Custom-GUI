#!/bin/sh
# 把编译好的二进制上传到 S1E
#
# 安全策略（重要）：
#   1. 默认 dry-run，只打印将要发生的事，不做任何修改
#   2. 只碰 /data/scripts/dev/ 这一个目录，绝不触碰作者的任何一个文件
#   3. 上传后先验证文件确实落到位且大小一致，验证通过才 chmod
#   4. 不 kill 任何进程，不改任何配置，不碰 /misc、/etc、/data/app
#
# 用法：
#   ./deploy.sh <IP>                      dry-run，只预览
#   ./deploy.sh <IP> hello_fb --run       真正执行
#   ./deploy.sh <IP> hello_fb snap_fast --run
#
# 卸载：./remove.sh <IP>

IP=""
TARGETS=""
RUN=0

for a in "$@"; do
    case "$a" in
        --run) RUN=1 ;;
        -*)    echo "未知选项: $a" >&2; exit 1 ;;
        *)     if [ -z "$IP" ]; then IP="$a"; else TARGETS="$TARGETS $a"; fi ;;
    esac
done

[ -z "$IP" ] && { echo "用法: $0 <设备IP> [程序名...] [--run]"; exit 1; }
[ -z "$TARGETS" ] && TARGETS="hello_fb"

DEV_DIR="/data/scripts/dev"
CURL="${CURL:-curl}"
API="http://$IP:8080/cgi-bin/api.cgi"

# 在设备上执行命令并回显。
#
# 必须用 POST + JSON body：api.cgi 对查询字符串里的 cmd 参数不做 URL 解码，
# 传给 sh -c 的是百分号编码原文（实测报 "test+-d+%2fdata...: not found"）。
# Web 控制台的终端也是走 POST JSON。
dexec() {
    payload=$(printf '{"cmd":%s}' \
        "$(printf '%s' "$1" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')")
    $CURL -s -X POST "$API?action=exec_cmd" \
        -H "Content-Type: application/json" \
        -d "$payload" 2>/dev/null
}

# 定位本地产物（兼容 dynamic/static 两种构建）
locate_bin() {
    for d in build/dynamic build/static build; do
        [ -f "$d/$1" ] && { echo "$d/$1"; return 0; }
    done
    return 1
}

echo "════════════════════════════════════════"
echo " 目标设备 : $IP"
echo " 隔离目录 : $DEV_DIR   (仅此一个，不触碰其他任何路径)"
echo " 目标程序 :$TARGETS"
if [ "$RUN" -eq 0 ]; then
    echo " 模式     : DRY-RUN（不会修改设备。加 --run 实际执行）"
else
    echo " 模式     : 实际执行"
fi
echo "════════════════════════════════════════"
echo

# --- 预检：dry-run 全程只读 ---
echo "[预检] 只读检查，不修改任何东西"
printf "  设备可达          : "
if $CURL -s -o /dev/null -w "%{http_code}" --connect-timeout 5 "http://$IP:8080/" 2>/dev/null | grep -q "^2"; then
    echo "OK"
else
    echo "失败——检查 IP 和网络"
    exit 1
fi

printf "  引擎是否在运行    : "
if dexec "pidof s1e_standalone_app" | grep -qE '(^|[^0-9])[0-9]+([^0-9]|$)'; then
    echo "在运行（我们不动它）"
else
    echo "未运行"
fi

printf "  隔离目录状态      : "
dirstate=$(dexec "test -d $DEV_DIR && echo __EXISTS__ || echo __MISSING__")
case "$dirstate" in
    *__EXISTS__*) echo "已存在（复用）" ;;
    *)            echo "不存在，执行时将创建" ;;
esac

# 冲突检查必须用唯一标记判断，不能 grep 文件名——
# `ls` 失败时错误信息里含有文件名，会造成假阳性。
printf "  同名文件冲突检查  : "
conflict=""
for name in $TARGETS; do
    if dexec "test -f $DEV_DIR/$name && echo __EXISTS__ || echo __MISSING__" \
        | grep -q "__EXISTS__"; then
        conflict="$conflict $name"
    fi
done
if [ -n "$conflict" ]; then
    echo "已存在将被覆盖:$conflict"
else
    echo "无冲突"
fi
echo

for name in $TARGETS; do
    src="$(locate_bin "$name")" || {
        echo "[!] 本地找不到产物 $name，先执行 make"
        exit 1
    }
    size=$(wc -c < "$src" | tr -d ' ')
    sum=$(shasum -a 256 "$src" 2>/dev/null | awk '{print $1}' || sha256sum "$src" | awk '{print $1}')
    echo "── $name"
    echo "    本地  : $src ($size 字节)"
    echo "    校验  : ${sum:0:16}…"

    if [ "$RUN" -eq 0 ]; then
        echo "    将执行:"
        echo "      1. POST 二进制到 api.cgi upload_file"
        echo "      2. 在设备上查找实际落盘位置"
        echo "      3. 校验大小一致后 chmod +x $DEV_DIR/$name"
        continue
    fi

    # 1) 上传
    $CURL -s -f -X POST --data-binary "@$src" \
        "$API?action=upload_file&name=dev%2F$name" -o /dev/null \
        -w "    上传 HTTP %{http_code}\n" || {
        echo "    [!] 上传失败，已中止"; exit 1; }

    # 2) 找到文件实际在哪（不猜，实测）
    found=""
    for cand in "$DEV_DIR/$name" "/data/scripts/dev/$name" "/data/scripts/$name" "/tmp/www/$name"; do
        if dexec "test -f $cand && echo __FOUND__" | grep -q "__FOUND__"; then
            found="$cand"; break
        fi
    done
    if [ -z "$found" ]; then
        echo "    [!] 上传后找不到文件，未执行 chmod。请 telnet 手动确认："
        echo "        find /data/scripts /tmp/www -name '$name' 2>/dev/null"
        exit 1
    fi
    echo "    落盘  : $found"

    # 3) 校验大小
    rsize=$(dexec "wc -c < $found" | tr -cd '0-9')
    if [ "$rsize" != "$size" ]; then
        echo "    [!] 大小不符（设备 $rsize vs 本地 $size），未 chmod。请手动检查。"
        exit 1
    fi
    echo "    大小校验通过 ($rsize 字节)"

    # 4) 才赋予权限
    dexec "chmod +x $found" >/dev/null
    echo "    ✓ 已就绪: $found"
    echo
done

if [ "$RUN" -eq 0 ]; then
    echo
    echo "这是预览。确认无误后加 --run 执行。"
else
    echo "全部完成。运行（只读，安全）："
    first=$(echo $TARGETS | awk '{print $1}')
    echo "  curl -s -G '$API' --data-urlencode 'action=exec_cmd' \\"
    echo "    --data-urlencode 'cmd=$DEV_DIR/$first probe'"
    echo
    echo "回滚：sh scripts/remove.sh $IP"
fi