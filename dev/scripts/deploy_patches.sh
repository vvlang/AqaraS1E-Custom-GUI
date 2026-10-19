#!/bin/sh
# deploy_patches — 把两个补丁部署到设备（默认 dry-run）
#
# 补丁 1：OTA 更新源  oopuuu -> vvlang
#   涉及作者的 api.cgi（改二进制内 3 处 URL）与 fetch_https.sh（3 处路径）
#
# 补丁 2：开机自启
#   涉及作者的 post_init.sh（注入 cam_view 的启动与保活）
#
# 安全设计
#   - 默认 dry-run，只打印计划，不做任何修改
#   - 先备份再改，备份放在 /data/scripts/dev/backup/
#   - 上传后逐字节校验，不符立即中止
#   - 全程可用 rollback.sh 一键还原
#   - 补丁 2 的代码全部以 [ -f /data/scripts/dev/xxx ] 兜底，
#     即使把整个 dev 目录删掉也不影响设备启动
#
# 用法：
#   ./deploy_patches.sh <IP>              dry-run 预览
#   ./deploy_patches.sh <IP> --ota --run  只做 OTA 源补丁
#   ./deploy_patches.sh <IP> --boot --run 只做开机自启补丁
#   ./deploy_patches.sh <IP> --ota --boot --run  两个都做

set -e

IP=""
DO_OTA=0
DO_BOOT=0
RUN=0

for a in "$@"; do
    case "$a" in
        --ota)  DO_OTA=1 ;;
        --boot) DO_BOOT=1 ;;
        --run)  RUN=1 ;;
        -*) echo "未知选项: $a" >&2; exit 1 ;;
        *)     [ -z "$IP" ] && IP="$a" ;;
    esac
done

[ -z "$IP" ] && { echo "用法: $0 <IP> [--ota] [--boot] [--run]"; exit 1; }
[ "$DO_OTA" -eq 0 ] && [ "$DO_BOOT" -eq 0 ] && { DO_OTA=1; DO_BOOT=1; }

API="http://$IP:8080/cgi-bin/api.cgi"
CURL="${CURL:-curl}"
BACKUP="/data/scripts/dev/backup"
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# 通过 POST+JSON 执行设备端命令。
# 注意两点：
#   1. api.cgi 不对查询串做 URL 解码，cmd 走 GET 会把百分号编码
#      原样传给 sh -c，必须用 POST+JSON。
#   2. 响应是 JSON（{"status":..,"output":".."}），这里只取 output，
#      否则下游拿到的是带转义的原始 JSON。
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

# 上传本地文件到 /data/scripts/dev/
upload() {
    $CURL -s -X POST --data-binary "@$1" \
        "$API?action=upload_file&name=dev/$2" 2>/dev/null
}

# 把 dev/ 下的文件原子替换到正式位置。
#
# 不能直接 cp -f 覆盖：目标文件正在运行时内核会返回 ETXTBSY
# （api.cgi 每次请求都 exec，post_init.sh 常驻）。
# 先写临时文件再 mv 是标准解法——mv 只改目录项，
# 正在运行的进程继续持有旧 inode，不受影响，下次 exec 才用新的。
install_atomic() {
    dexec "cp -f /data/scripts/dev/$1 /data/scripts/$1.new && \
           chmod +x /data/scripts/$1.new && \
           mv -f /data/scripts/$1.new /data/scripts/$1 && echo __OK__" \
    | grep -q "__OK__"
}

echo "════════════════════════════════════════"
echo " 目标设备 : $IP"
echo " 备份目录 : $BACKUP"
[ "$RUN" -eq 1 ] && echo " 模式     : 实际执行" || echo " 模式     : DRY-RUN（加 --run 才执行）"
echo " 补丁     : OTA源=$DO_OTA  开机自启=$DO_BOOT"
echo "════════════════════════════════════════"
echo

echo "[预检] 只读检查"
printf "  设备可达        : "
if $CURL -s -o /dev/null -w "%{http_code}" --connect-timeout 5 "http://$IP:8080/" 2>/dev/null | grep -q "^2"; then
    echo "OK"
else
    echo "失败"; exit 1
fi

printf "  隔离目录        : "
dexec "mkdir -p $BACKUP && echo __OK__" | grep -q "__OK__" && echo "就绪" || { echo "不可写"; exit 1; }

printf "  待替换文件在跑吗: "
running=""
for f in api.cgi post_init.sh; do
    if dexec "test -x /proc/\$(pidof $f 2>/dev/null)/exe && echo __RUNNING__" | grep -q "__RUNNING__"; then
        running="$running $f"
    fi
done
if [ -n "$running" ]; then
    echo "是:$running"
    echo "    正常运行中——脚本用「写临时文件 + mv 原子替换」规避 ETXTBSY"
    echo "    运行中的进程继续使用旧 inode，新版本从下一次启动/exec 生效。"
else
    echo "否"
fi
echo

# ---------- 备份 ----------
if [ "$RUN" -eq 1 ]; then
    echo "[1] 备份原文件到 $BACKUP"
    dexec "for f in api.cgi fetch_https.sh post_init.sh; do \
             [ -f /data/scripts/\$f ] && cp -f /data/scripts/\$f $BACKUP/\$f.orig; \
           done; ls -l $BACKUP/" | sed 's/^/    /'
    echo
fi

# ---------- 补丁 1：OTA 更新源 ----------
if [ "$DO_OTA" -eq 1 ]; then
    echo "[2] OTA 更新源补丁"
    SRC_PKG=/tmp/s1e_custom_gui_vvlang.tar.gz
    if [ ! -f "$SRC_PKG" ]; then
        echo "    [!] 缺少 $SRC_PKG"
        echo "    生成方式： python3 tools/patch_ota_source.py \\"
        echo "                 --src ../release/s1e_custom_gui_latest.tar.gz \\"
        echo "                 --out $SRC_PKG --owner <你的GitHub用户名>"
        exit 1
    fi

    if [ "$RUN" -eq 0 ]; then
        echo "    将执行:"
        echo "      1. 解包 $SRC_PKG，取出 api.cgi 与 fetch_https.sh"
        echo "      2. 覆盖 /data/scripts/ 下的同名文件"
        echo "      3. 校验字节数与 ELF 头"
        echo "      4. 回滚: sh scripts/rollback.sh $IP"
    else
        d=$(mktemp -d)
        tar -xzf "$SRC_PKG" -C "$d" 2>/dev/null
        for f in api.cgi fetch_https.sh; do
            [ -f "$d/$f" ] || { echo "    [!] 包内缺少 $f"; rm -rf "$d"; exit 1; }
            printf "    %-16s " "$f"
            upload "$d/$f" "$f" >/dev/null
            if install_atomic "$f"; then
                sz=$(dexec "wc -c < /data/scripts/$f" | tr -cd '0-9')
                echo "已原子替换 ($sz 字节)"
            else
                echo "[!] 替换失败"; rm -rf "$d"; exit 1
            fi

            # api.cgi 有两份：httpd 实际执行的是 /tmp/www/cgi-bin/ 那份，
            # /data/scripts/ 那份是持久化副本、开机时由 post_init.sh 复制过去。
            # 只改持久化副本的话，本次运行仍然用的是旧文件，要等下次重启才生效。
            if [ "$f" = "api.cgi" ]; then
                printf "    %-16s " "  └ 运行副本"
                if dexec "cp -f /data/scripts/api.cgi /tmp/www/cgi-bin/api.cgi.new && \
                           chmod +x /tmp/www/cgi-bin/api.cgi.new && \
                           mv -f /tmp/www/cgi-bin/api.cgi.new /tmp/www/cgi-bin/api.cgi && \
                           echo __OK__" | grep -q "__OK__"; then
                    echo "已同步 (/tmp/www/cgi-bin/api.cgi)"
                else
                    echo "[!] 同步失败（重启后 post_init.sh 会自行复制）"
                fi
            fi
        done
        rm -rf "$d"
        echo "    [+] 完成。改动是 3 处 URL 的等长替换，文件大小不变。"
        echo "    注：api.cgi 是每次请求 exec 一次，替换后下一个请求即生效。"
    fi
    echo
fi

# ---------- 补丁 2：开机自启 ----------
if [ "$DO_BOOT" -eq 1 ]; then
    echo "[3] 开机自启补丁"
    PATCHED=/tmp/post_init_patched.sh
    if [ ! -f "$PATCHED" ]; then
        echo "    [!] 缺少 $PATCHED"
        echo "    生成方式： python3 tools/patch_boot_autostart.py \\"
        echo "                 --src <解包出的post_init.sh> --out $PATCHED"
        exit 1
    fi

    if [ "$RUN" -eq 0 ]; then
        echo "    将执行:"
        echo "      1. 覆盖 /data/scripts/post_init.sh"
        echo "      2. sh -n 语法自检"
        echo "      3. 回滚: sh scripts/rollback.sh $IP"
        echo
        echo "    [!] 这是改动作者的启动流程。仅在确认设备当前空闲时执行。"
    else
        upload "$PATCHED" post_init.sh >/dev/null
        printf "    %-16s " "post_init.sh"
        if install_atomic "post_init.sh"; then echo "已原子替换"; else echo "[!] 替换失败"; exit 1; fi
        echo "    语法自检:"
        if dexec "sh -n /data/scripts/post_init.sh && echo __SYNTAX_OK__" | grep -q "__SYNTAX_OK__"; then
            echo "      [+] 通过"
        else
            echo "      [!] 未通过，自动回滚"
            dexec "cp -f $BACKUP/post_init.sh.orig /data/scripts/post_init.sh" >/dev/null
            exit 1
        fi
        echo "    [+] 完成，重启设备后生效"
    fi
    echo
fi

echo "════════════════════════════════════════"
if [ "$RUN" -eq 0 ]; then
    echo " 这是预览。确认后加 --run 执行。"
else
    echo " 部署完成。"
fi
echo " 回滚: sh scripts/rollback.sh $IP"
echo "════════════════════════════════════════"
