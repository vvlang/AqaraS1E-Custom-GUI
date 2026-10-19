#include "s1e_conf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* 从 JSON 文本里取 "key": "value" 的 value（不含引号，不做转义还原）。
 * 这是有意为之的简化：HA 令牌是 base64 风格，不含 \" 和 \\。 */
int s1e_json_get_string(const char *json, const char *key,
                        char *out, size_t out_len)
{
    char pat[64];
    const char *p, *v, *end;
    size_t n;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return -1;

    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;

    v = p;
    end = strchr(v, '"');
    if (!end) return -1;

    n = (size_t)(end - v);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, v, n);
    out[n] = '\0';
    return 0;
}

/* 从 JSON 文本里取 "key": <数字> 的值。找不到返回 def。 */
int s1e_json_get_int(const char *json, const char *key, int def)
{
    char pat[64];
    const char *p;
    char *end;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return def;

    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return def;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    long v = strtol(p, &end, 10);
    if (end == p) return def;
    return (int)v;
}

/* JSON 字符串转义：引号、反斜杠、控制字符必须转义，否则中文名会破坏 JSON */
void s1e_json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!out_len) return;

    for (; *in && o + 7 < out_len; in++) {
        unsigned char c = (unsigned char)*in;
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) {
                o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", c);
            } else {
                out[o++] = (char)c;   /* UTF-8 多字节原样保留 */
            }
        }
    }
    out[o] = '\0';
}

int s1e_camcfg_load(s1e_cam_cfg *cfg, const char *path)
{
    static char buf[1024];
    FILE *f;
    size_t n;
    const char *p = path ? path : S1E_CAMCFG_PATH;

    /* 先填默认值 */
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->entity, sizeof(cfg->entity), "%s",
             "camera.chuangmi_ipc018_acd6_video_doorbell");
    cfg->fps = 3;
    cfg->width = 640;
    cfg->height = 480;
    cfg->camera_page = 2;
    cfg->total_pages = 3;

    f = fopen(p, "r");
    if (!f) {
        fprintf(stderr, "[cfg] 未找到 %s，使用默认配置\n", p);
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    s1e_json_get_string(buf, "entity", cfg->entity, sizeof(cfg->entity));
    cfg->fps         = s1e_json_get_int(buf, "fps", cfg->fps);
    cfg->width       = s1e_json_get_int(buf, "width", cfg->width);
    cfg->height      = s1e_json_get_int(buf, "height", cfg->height);
    cfg->camera_page = s1e_json_get_int(buf, "camera_page", cfg->camera_page);
    cfg->total_pages = s1e_json_get_int(buf, "total_pages", cfg->total_pages);

    if (cfg->fps < 1)  cfg->fps = 1;
    if (cfg->fps > 15) cfg->fps = 15;
    if (cfg->camera_page < 0) cfg->camera_page = 0;
    if (cfg->camera_page > cfg->total_pages - 1)
        cfg->camera_page = cfg->total_pages - 1;

    fprintf(stderr, "[cfg] %s: %s %dx%d @%dfps 第%d屏/共%d屏\n",
            p, cfg->entity, cfg->width, cfg->height, cfg->fps,
            cfg->camera_page, cfg->total_pages);
    return 0;
}

int s1e_camcfg_write_default(const char *path, const char *entity)
{
    FILE *f = fopen(path ? path : S1E_CAMCFG_PATH, "w");
    if (!f) return -1;
    fprintf(f,
        "{\n"
        "  \"_说明\": \"改完这个文件后重启 cam_view 生效\",\n"
        "  \"entity\": \"%s\",\n"
        "  \"width\": 640,\n"
        "  \"height\": 480,\n"
        "  \"fps\": 3,\n"
        "  \"camera_page\": 2,\n"
        "  \"total_pages\": 3\n"
        "}\n",
        entity ? entity : "camera.chuangmi_ipc018_acd6_video_doorbell");
    fclose(f);
    return 0;
}

int s1e_conf_load(s1e_ha_conf *out, const char *path)
{
    static char buf[2048];
    FILE *f;
    size_t n;

    memset(out, 0, sizeof(*out));

    f = fopen(path ? path : S1E_CONF_PATH, "r");
    if (!f) {
        fprintf(stderr, "[conf] 无法打开 %s\n", path ? path : S1E_CONF_PATH);
        return -1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (s1e_json_get_string(buf, "ha_url", out->url, sizeof(out->url)) != 0 ||
        out->url[0] == '\0') {
        fprintf(stderr, "[conf] 未找到 ha_url，请在 Web 控制台配置 Home Assistant\n");
        return -1;
    }

    if (s1e_json_get_string(buf, "ha_token", out->token, sizeof(out->token)) != 0 ||
        out->token[0] == '\0') {
        fprintf(stderr, "[conf] 未找到 ha_token\n");
        return -1;
    }

    /* token 长度提示即可，绝不打印内容 */
    fprintf(stderr, "[conf] HA=%s, token 长度=%zu\n", out->url,
            strlen(out->token));
    return 0;
}

int s1e_conf_split_url(const char *url, char *host, int host_len, int *port,
                       int default_port)
{
    const char *p, *colon;
    size_t n;

    *port = default_port;

    p = strstr(url, "://");
    p = p ? p + 3 : url;

    /* 去掉路径部分 */
    {
        const char *slash = strchr(p, '/');
        n = slash ? (size_t)(slash - p) : strlen(p);
    }

    colon = memchr(p, ':', n);
    if (colon) {
        size_t hl = (size_t)(colon - p);
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) *port = default_port;
        if (hl >= (size_t)host_len) hl = (size_t)host_len - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
    } else {
        if (n >= (size_t)host_len) n = (size_t)host_len - 1;
        memcpy(host, p, n);
        host[n] = '\0';
    }

    return host[0] ? 0 : -1;
}
