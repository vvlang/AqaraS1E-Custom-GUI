#!/bin/sh
# rollback — 用备份还原被补丁改过的作者文件
#
# 涉及 api.cgi / fetch_https.sh / post_init.sh 三个文件，
# 全部还原为 /data/scripts/dev/backup/*.orig
#
# 用法：
#   ./rollback.sh <IP>              dry-run，只显示将还原什么
#   ./rollback.sh <IP> --run        实际还原
#
# 还原后建议重启设备，让 post_init.sh 的改动真正撤销。

set -e

IP=""
RUN=0
for a in "$@"; do
    case "$a" in
        --run) RUN=1 ;;
        -*) echo "未知选项: $a" >&2; exit 1 ;;
        *)   [ -z "$IP" ] && IP="$a" ;;
    esac
done
[ -z "$IP" ] && { echo "用法: $0 <IP> [--run]"; exit 1; }

API="http://$IP:8080/cgi-bin/api.cgi"
CURL="${CURL:-curl}"
BACKUP="/data/scripts/dev/backup"

# exec_cmd 返回的是 JSON（{"status":..,"output":".."}），
# 这里只把 output 字段取出来，否则下游拿到的是带转义的原始 JSON。
dexec() {
    payload=$(printf '{"cmd":%s}' \
        "$(printf '%s' "$1" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')")
    $CURL -s -X POST "$API?action=exec_cmd" \
        -H "Content-Type: application/json" -d "$payload" 2>/dev/null \
    | python3 -c '
import json, sys
raw = sys.stdin.read()
try:
    print(json.loads(raw).get("output", ""), end="")
except Exception:
    print(raw, end="")
'
}

echo "目标设备 : $IP"
echo "备份目录 : $BACKUP"
[ "$RUN" -eq 1 ] && echo "模式     : 实际还原" || echo "模式     : DRY-RUN"
echo

echo "=== 备份中可用的文件 ==="
avail=$(dexec "ls -1 $BACKUP/*.orig 2>/dev/null" | tr -d '\r')
if [ -z "$avail" ]; then
    echo "  没有找到备份，无需还原。"
    exit 0
fi
echo "$avail" | sed 's/^/  /'
echo

if [ "$RUN" -eq 0 ]; then
    echo "将还原："
    for f in $avail; do
        base=$(basename "$f" .orig)
        echo "  /data/scripts/$base  <-  $f"
    done
    echo
    echo "确认后加 --run 执行。还原后建议重启设备。"
    exit 0
fi

echo "=== 还原 ==="
for f in $avail; do
    base=$(basename "$f" .orig)
    printf "  %-20s " "$base"
    dexec "cp -f $f /data/scripts/$base && chmod +x /data/scripts/$base && echo __OK__" \
        | grep -q "__OK__" && echo "已还原" || echo "失败"
done

echo
echo "=== 还原后校验（应与备份一致）==="
for f in $avail; do
    base=$(basename "$f" .orig)
    printf "  %-20s " "$base"
    a=$(dexec "sha256sum $f | cut -d' ' -f1")
    b=$(dexec "sha256sum /data/scripts/$base | cut -d' ' -f1")
    [ "$a" = "$b" ] && echo "一致 ✓" || echo "不一致 ✗"
done

echo
echo "建议重启设备，让 post_init.sh 的改动彻底撤销："
echo "  dexec \"reboot -f\"   （或用 Web 控制台的『重启』）"
