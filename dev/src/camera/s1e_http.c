#include "s1e_http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RX_CHUNK   16384
#define RX_MAX     (8 * 1024 * 1024)   /* 单帧上限，防止异常响应吃光内存 */
#define TIMEOUT_S  8

/* 设置环境变量 S1E_DEBUG=1 可打印协议层细节，排查现场问题时很有用 */
static int debug_on(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("S1E_DEBUG");
        cached = (e && *e && *e != '0') ? 1 : 0;
    }
    return cached;
}

#define DBG(...) do { if (debug_on()) fprintf(stderr, __VA_ARGS__); } while (0)

/* 大小写不敏感的子串查找。不用 strcasestr，避免依赖 GNU 扩展。 */
static int ci_contains(const char *hay, const char *needle)
{
    size_t nl;
    if (!hay || !needle) return 0;
    nl = strlen(needle);
    if (nl == 0) return 1;
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < nl; i++) {
            if (hay[i] == '\0') return 0;
            if (tolower((unsigned char)hay[i]) !=
                tolower((unsigned char)needle[i]))
                break;
        }
        if (i == nl) return 1;
    }
    return 0;
}

void s1e_http_free(s1e_http_resp *r)
{
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

static int connect_to(const char *host, int port, int timeout_s)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portbuf[16];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portbuf, sizeof(portbuf), "%d", port);

    if (getaddrinfo(host, portbuf, &hints, &res) != 0)
        return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        struct timeval tv = { timeout_s, 0 };

        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* 在缓冲区里找子串，返回首次出现的位置，找不到返回 -1 */
static long find_bytes(const unsigned char *buf, size_t n,
                       const char *needle, size_t nl)
{
    if (n < nl) return -1;
    for (size_t i = 0; i + nl <= n; i++) {
        if (buf[i] == (unsigned char)needle[0] &&
            memcmp(buf + i, needle, nl) == 0)
            return (long)i;
    }
    return -1;
}

/* 取某个响应头的值；hdr_found 指向 "\r\n\r\n" 的起始位置。找不到返回 -1。
 *
 * 注意边界：最后一个响应头的 CRLF 恰好落在 hdr_found 处，不在 [0, hdr_found)
 * 区域内。所以不能简单地把 find_bytes 的上界设成 hdr_found——那需要额外
 * 2 字节余量才能匹配 "\r\n"，结果就是最后一个头永远找不到（实测中
 * Transfer-Encoding 恰好是最后一个头，导致 chunked 分支从不进入）。
 * 这里改为逐行扫描，末行没有 CRLF 时用 hdr_found 作为行尾。
 */
static long header_value(const unsigned char *buf, long hdr_found,
                         const char *name)
{
    size_t nl = strlen(name);
    long pos, line_end;

    /* 跳过状态行 */
    line_end = find_bytes(buf, (size_t)hdr_found, "\r\n", 2);
    if (line_end < 0) return -1;
    pos = line_end + 2;

    while (pos < hdr_found) {
        long e = find_bytes(buf + pos, (size_t)(hdr_found - pos), "\r\n", 2);
        long line_len = (e < 0) ? (hdr_found - pos) : e;
        const unsigned char *line = buf + pos;

        if (line_len >= (long)nl + 1 &&
            strncasecmp((const char *)line, name, nl) == 0 &&
            line[nl] == ':')
            return pos + (long)nl + 1;     /* 指向值 */

        if (e < 0) break;
        pos += e + 2;
    }
    return -1;
}

static size_t read_all(int fd, unsigned char **out, size_t *out_len)
{
    size_t cap = RX_CHUNK * 2, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) return 0;

    for (;;) {
        ssize_t n;

        if (len + RX_CHUNK + 1 > cap) {
            size_t ncap = cap * 2;
            unsigned char *nb;
            if (ncap > RX_MAX) ncap = RX_MAX;
            if (ncap <= cap) break;              /* 已达上限 */
            nb = realloc(buf, ncap);
            if (!nb) { free(buf); return 0; }
            buf = nb;
            cap = ncap;
        }

        n = read(fd, buf + len, RX_CHUNK);
        if (n <= 0) break;
        len += (size_t)n;

        if (len >= RX_MAX) break;
    }

    *out = buf;
    *out_len = len;
    return len;
}

static int decode_chunked(const unsigned char *in, size_t n,
                          unsigned char **out, size_t *out_len)
{
    size_t pos = 0, cap = n, len = 0;
    unsigned char *buf = malloc(cap ? cap : 1);
    if (!buf) return -1;

    while (pos < n) {
        /* 读十六进制长度行 */
        size_t le = pos;
        char sz[32];
        size_t sl;
        char *semi;
        long chunk;

        while (le + 1 < n && !(in[le] == '\r' && in[le + 1] == '\n')) le++;
        if (le + 1 >= n) break;

        sl = le - pos;
        if (sl >= sizeof(sz)) sl = sizeof(sz) - 1;
        memcpy(sz, in + pos, sl);
        sz[sl] = '\0';

        semi = strchr(sz, ';');
        if (semi) *semi = '\0';

        chunk = strtol(sz, NULL, 16);
        pos = le + 2;
        if (chunk <= 0) break;

        if ((size_t)chunk > n - pos) chunk = (long)(n - pos);
        if (len + (size_t)chunk > cap) {
            unsigned char *nb = realloc(buf, len + (size_t)chunk + 1);
            if (!nb) { free(buf); return -1; }
            buf = nb;
            cap = len + (size_t)chunk + 1;
        }
        memcpy(buf + len, in + pos, (size_t)chunk);
        len += (size_t)chunk;
        pos += (size_t)chunk;

        if (pos + 1 < n && in[pos] == '\r' && in[pos + 1] == '\n')
            pos += 2;
    }

    *out = buf;
    *out_len = len;
    return 0;
}

int s1e_http_get(const char *host, int port, const char *path,
                 const char *bearer, s1e_http_resp *resp)
{
    unsigned char *raw = NULL;
    size_t raw_len = 0;
    char req[1024];
    int fd, req_len;
    long hdr_end;

    memset(resp, 0, sizeof(*resp));

    fd = connect_to(host, port, TIMEOUT_S);
    if (fd < 0)
        return -1;

    /*
     * 注意 Authorization 行的拼接方式：格式串末尾的 \r\n 是唯一的行结束符，
     * 第三个参数不能再带 \r\n。否则会产生 "...TOKEN\r\n\r\n"——
     * 一个提前的空行，服务器会认为请求已结束，把下一行 Accept 当成新请求的
     * 请求行。HA 的 aiohttp 会直接报 "Invalid method encountered"。
     */
    req_len = snprintf(req, sizeof(req),
                       "GET %s HTTP/1.1\r\n"
                       "Host: %s:%d\r\n"
                       "User-Agent: s1e-cam/1.0\r\n"
                       "%s%s%s"
                       "Accept: */*\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       path, host, port,
                       bearer ? "Authorization: Bearer " : "",
                       bearer ? bearer : "",
                       bearer ? "\r\n" : "");
    if (req_len <= 0 || req_len >= (int)sizeof(req)) {
        close(fd);
        return -1;
    }

    if (write(fd, req, (size_t)req_len) != req_len) {
        close(fd);
        return -1;
    }

    if (read_all(fd, &raw, &raw_len) == 0 || raw_len < 12) {
        free(raw);
        close(fd);
        return -1;
    }
    close(fd);

    /* 状态码 */
    if (strncmp((char *)raw, "HTTP/", 5) != 0) {
        free(raw);
        return -1;
    }
    resp->status = atoi((char *)raw + 9);

    hdr_end = find_bytes(raw, raw_len, "\r\n\r\n", 4);
    if (hdr_end < 0) {
        DBG("[http] 未找到头部结束标记，收到 %zu 字节\n", raw_len);
        free(raw);
        resp->status = 0;
        return -1;
    }

    DBG("[http] %s:%d%s -> %d, 收到 %zu 字节, 头结束于 %ld\n",
        host, port, path, resp->status, raw_len, hdr_end);

    {
        const unsigned char *body = raw + hdr_end + 4;
        size_t body_len = raw_len - (size_t)hdr_end - 4;

        /* Transfer-Encoding: chunked */
        long te = header_value(raw, hdr_end, "Transfer-Encoding");
        DBG("[http] Transfer-Encoding 位于 %ld, 命中=%d\n",
            te, te >= 0 && ci_contains((const char *)raw + te, "chunked"));
        if (te >= 0 && ci_contains((const char *)raw + te, "chunked")) {
            if (decode_chunked(body, body_len, &resp->body, &resp->len) != 0) {
                free(raw);
                return -1;
            }
            free(raw);
            return 0;
        }

        /* Content-Length 截断，避免把连接关闭后的噪声算进去 */
        long cl = header_value(raw, hdr_end, "Content-Length");
        if (cl >= 0) {
            long want = atol((char *)raw + cl);
            if (want >= 0 && (size_t)want < body_len)
                body_len = (size_t)want;
        }

        if (body_len == 0) {
            resp->body = NULL;
            resp->len = 0;
        } else {
            resp->body = malloc(body_len);
            if (!resp->body) {
                free(raw);
                return -1;
            }
            memcpy(resp->body, body, body_len);
            resp->len = body_len;
        }
    }

    free(raw);
    return 0;
}
