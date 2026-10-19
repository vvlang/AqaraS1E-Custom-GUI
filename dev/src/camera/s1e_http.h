/*
 * s1e_http — 极简 HTTP/1.1 客户端
 *
 * 只做一件事：GET 一个 URL，把响应体读进内存。
 * 够用于拉 Home Assistant 的 REST 接口，不做重定向、不做 TLS、不做连接池。
 *
 * 设计约束来自设备实况：
 *   - 局域网内 HA 是明文 HTTP（http://10.0.0.4:8123），不需要 TLS
 *   - 设备上没有 libcurl，也不该为一个 GET 引入动态依赖
 *   - 请求用 Connection: close，服务器发完就断开，简化读取逻辑
 */

#ifndef S1E_HTTP_H
#define S1E_HTTP_H

#include <stddef.h>

typedef struct {
    unsigned char *body;    /* 响应体（malloc，可为 NULL 当体为空） */
    size_t         len;     /* 响应体长度 */
    int            status;  /* HTTP 状态码，0 表示连接失败 */
} s1e_http_resp;

/*
 * 发起一次 GET。
 *   host/path   仅主机名与路径，端口单列
 *   bearer      传 NULL 表示不带 Authorization 头
 *
 * 返回 0 成功（此时 resp.status 是 HTTP 码），-1 失败（errno 有意义）。
 * 失败或结束后调用 s1e_http_free 释放。
 */
int s1e_http_get(const char *host, int port, const char *path,
                 const char *bearer, s1e_http_resp *resp);

void s1e_http_free(s1e_http_resp *resp);

#endif /* S1E_HTTP_H */
