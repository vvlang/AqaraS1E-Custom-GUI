#!/bin/sh
# 卸载 dev/ 部署到设备上的文件
#
# 只删除 /data/scripts/dev/ 目录，且不删除该目录之外任何东西。
# 绝不动作者的任何一个文件，绝不动任何配置。
#
# 用法：
#   ./remove.sh <IP>            dry-run，只显示将删除什么
#   ./remove.sh <IP> --run      实际删除

IP=""
RUN=0
for a in "$@"; do
    case "$a" in
        --run) RUN=1 ;;
        *)     [ -z "$IP" ] && IP="$a" ;;
    esac
done
[ -z "$IP" ] && { echo "用法: $0 <设备IP> [--run]"; exit 1; }

DEV_DIR="/data/scripts/dev"
CURL="${CURL:-curl}"
API="http://$IP:8080/cgi-bin/api.cgi"

dexec() {
    payload=$(printf '{"cmd":%s}' \
        "$(printf '%s' "$1" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')")
    $CURL -s -X POST "$API?action=exec_cmd" \
        -H "Content-Type: application/json" \
        -d "$payload" 2>/dev/null
}

echo "目标: $IP"
echo "范围: 仅 $DEV_DIR（不触及任何其他路径）"
echo

contents=$(dexec "ls -1 $DEV_DIR 2>/dev/null")
if [ -z "$contents" ]; then
    echo "[*] $DEV_DIR 不存在或为空，无需清理"
    exit 0
fi

echo "将删除以下文件："
echo "$contents" | sed 's/^/  /'
echo

if [ "$RUN" -eq 0 ]; then
    echo "这是预览。确认后加 --run 执行。"
    exit 0
fi

# 用精确的目录路径删除，不用 glob，避免误伤
dexec "rm -f $DEV_DIR/hello_fb $DEV_DIR/snap_fast $DEV_DIR/http_body_strip $DEV_DIR/safe_png_recv"
dexec "rmdir $DEV_DIR 2>/dev/null"

echo "清理完成。验证："
dexec "ls -la $DEV_DIR 2>/dev/null || echo '目录已移除'"

echo
echo "提示：作者的程序未受影响，设备会照常运行。"
echo "如需完全恢复到官方状态，用 Web 控制台的『一键卸载自研程序』，"
echo "或 telnet 后执行 api.cgi 的 uninstall_custom 动作。"