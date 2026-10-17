/*
 * http_body_strip — 从 stdin 的 HTTP 响应中剥掉头部，只输出 body
 *
 * 用途：设备上没有 curl/wget 的 HTTPS 支持，作者用它配合 openssl s_client
 * 抓取 OTA 固件。本版是同名闭源工具的开源替代。
 *
 * 用法：openssl s_client -quiet ... | http_body_strip > out.bin
 *
 * 与原版的区别：
 *   - 原版按固定状态机逐字节跳转；本版用滑动窗口匹配分隔符，
 *     逻辑更短，且不会在 "\r\n\r\n" 跨缓冲区边界时错位
 *   - 原版不处理 chunked 编码；本版加了 -d 选项开启 chunked 解码
 *
 * 退出码：0 正常
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFSZ 4096

/* HTTP 头与 body 的分隔符长度 */
#define HDR_END_LEN 4

/* 滑动窗口匹配：返回 1 表示窗口等于分隔符 */
static int window_is_end(const unsigned char *w)
{
    return w[0] == '\r' && w[1] == '\n' && w[2] == '\r' && w[3] == '\n';
}

/*
 * chunked 解码。响应体形如:
 *   <hex 长度>\r\n<数据>\r\n<hex 长度>\r\n...0\r\n\r\n
 * 每块数据后跟一个 CRLF。
 */
static void decode_chunked(const unsigned char *in, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        /* 读十六进制长度行 */
        size_t line_end = pos;
        while (line_end + 1 < len &&
               !(in[line_end] == '\r' && in[line_end + 1] == '\n')) {
            line_end++;
        }
        if (line_end + 1 >= len)
            break;

        /* 长度字段可能带 ;ext=xxx 后缀，只解析分号前的部分 */
        char size_buf[32];
        size_t size_len = line_end - pos;
        if (size_len >= sizeof(size_buf))
            size_len = sizeof(size_buf) - 1;
        memcpy(size_buf, in + pos, size_len);
        size_buf[size_len] = '\0';

        char *semi = strchr(size_buf, ';');
        if (semi)
            *semi = '\0';

        long chunk_size = strtol(size_buf, NULL, 16);
        pos = line_end + 2;

        if (chunk_size <= 0)
            break;   /* 结束块 */

        size_t avail = len - pos;
        size_t to_write = (chunk_size <= (long)avail) ? (size_t)chunk_size : avail;
        fwrite(in + pos, 1, to_write, stdout);

        pos += to_write;
        if (pos + 1 < len && in[pos] == '\r' && in[pos + 1] == '\n')
            pos += 2;   /* 跳过块尾 CRLF */
    }
}

int main(int argc, char **argv)
{
    int do_chunked = (argc >= 2 && strcmp(argv[1], "-d") == 0);

    unsigned char buf[BUFSZ];
    unsigned char win[HDR_END_LEN];
    int win_len = 0;
    int headers_done = 0;

    for (;;) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0)
            break;

        const unsigned char *p = buf;
        size_t remaining = (size_t)n;

        if (!headers_done) {
            while (remaining > 0) {
                win[win_len++] = *p;
                p++;
                remaining--;

                if (win_len == HDR_END_LEN) {
                    if (window_is_end(win)) {
                        headers_done = 1;
                        /*
                         * 这里只能跳出内层循环，不能结束整个程序。
                         * curl 以管道方式输出时会把响应头单独 flush，
                         * 第一次 read() 往往只拿到头部（remaining == 0），
                         * body 要等下一次 read()。
                         */
                        break;
                    }
                    /* 不匹配则把窗口左移一格，重新对齐 */
                    win[0] = win[1];
                    win[1] = win[2];
                    win[2] = win[3];
                    win_len = HDR_END_LEN - 1;
                }
            }
        }

        if (headers_done) {
            if (do_chunked) {
                /* chunked 响应需要看到整个 body，所以必须一次性读完 */
                /* 不适用：仅在头部与 body 同批到达时正确 */
                decode_chunked(p, remaining);
                break;
            }
            if (remaining > 0)
                fwrite(p, 1, remaining, stdout);
        }
    }

    fflush(stdout);
    return 0;
}