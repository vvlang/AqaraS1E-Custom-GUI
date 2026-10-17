#!/bin/sh
# 采集 S1E 的运行环境信息，用来判断编译产物能不能跑
#
# 用法：./probe.sh <设备IP>
#
# 通过 api.cgi 的 exec_cmd 执行诊断命令并回显结果。
# 全部只读，不修改设备状态。

set -e

IP="$1"
[ -z "$IP" ] && { echo "用法: $0 <设备IP>"; exit 1; }

CURL="${CURL:-curl}"
API="http://$IP:8080/cgi-bin/api.cgi"

run() {
    desc="$1"; cmd="$2"
    echo "── $desc"
    # POST + JSON：api.cgi 不解码查询串里的 cmd 参数
    payload=$(printf '{"cmd":%s}' \
        "$(printf '%s' "$cmd" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))')")
    $CURL -s -X POST "$API?action=exec_cmd" -H "Content-Type: application/json" \
        -d "$payload" 2>/dev/null \
        | sed -n 's/.*"output":"\(.*\)".*/\1/p' \
        | sed 's/\\n/\n/g; s/\\t/    /g; s/\\r//g; s/\\"/"/g'
    echo
}

echo "══════ S1E 环境探测: $IP ══════"
echo

run "系统信息" "uname -a"
run "glibc 版本（决定动态链接可行性）" "ls -l /lib/libc.so.6; /lib/libc.so.6 2>&1 | head -1"
run "CPU 架构" "cat /proc/cpuinfo | head -20"
run "内存状况" "free; echo; grep -E 'MemTotal|MemAvailable' /proc/meminfo"
run "存储分区" "df -h /data /tmp /misc 2>&1"
run "framebuffer 设备" "ls -l /dev/fb* 2>&1; echo; cat /proc/fb 2>&1"
run "触摸输入设备" "ls -l /dev/input/ 2>&1"
run "背光 PWM" "ls -l /sys/class/pwm/pwmchip0/ 2>&1"
run "已安装的自研 GUI" "ls -la /data/scripts/ 2>&1 | head -30"
run "正在运行的进程" "ps 2>&1 | grep -E 'standalone|ha_daemon|httpd|ubusd' "

echo "══════ 探测结束 ══════"
echo
echo "解读要点："
echo "  · glibc 若 >= 2.17，动态链接可行；更老则必须静态链接"
echo "  · MemAvailable 需留出余量，作者程序占用约 1.2MB"
echo "  · /dev/fb0 存在才有图形输出能力"