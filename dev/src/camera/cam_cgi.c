/*
 * cam_cgi — 摄像头配置的 CGI 接口
 *
 * 装在 /tmp/www/cgi-bin/cam_cgi，由设备自带的 BusyBox httpd 提供服务。
 * 供 cam_settings.html 使用。
 *
 * 动作
 *   ?action=page       输出设置页 HTML，并通过 Set-Cookie 下发访问密钥
 *   ?action=get        读当前配置，返回 JSON
 *   ?action=list       从 HA 拉实体列表，返回摄像头
 *   ?action=snapshot   从 HA 拉一帧 JPEG，直接输出图像
 *   ?action=save       POST JSON 保存配置
 *
 * 安全
 *   设备整机的 Web 层本来就是无鉴权的（作者的 api.cgi 更是有无认证的
 *   exec_cmd），但我们不打算再扩大这个口子：
 *   - 不发送 Access-Control-Allow-Origin。缺了这个头，跨源页面即使
 *     发起请求也读不到响应体，拿不走数据。
 *   - 除 page 外的所有动作都要求访问密钥，密钥由 page 动作通过
 *     HttpOnly Cookie 下发。只有能直接访问这台设备的人能拿到。
 *
 * 为什么需要 snapshot
 *   作者的 push_notify 取图用的是不带认证的 wget，直接喂 HA 的
 *   camera_proxy 必然 403。这个动作从设备侧取图并提供无认证的本地地址，
 *   让作者原生的全屏弹窗也能显示 HA 摄像头画面。
 *   注意该地址同样受此处的密钥保护——若要把它喂给 push_notify，
 *   需要把密钥一并带上，否则等于对外开放你的门口影像。
 *
 * 令牌始终留在设备进程内，不出现在任何响应里。
 */

#include "s1e_conf.h"
#include "s1e_http.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_POST 4096
#define SECRET_PATH "/data/scripts/dev/cam_secret"

static void http_out(const char *ctype)
{
    printf("Content-Type: %s\r\n", ctype);
    printf("Cache-Control: no-store\r\n");
    /* 刻意不发 Access-Control-Allow-Origin：
     * 有了它，任意网页都能跨源读取本接口的响应。
     * 设置页与本接口同源，不需要这个头。 */
    printf("\r\n");
}

static void json_err(const char *msg)
{
    http_out("application/json; charset=utf-8");
    printf("{\"status\":\"error\",\"message\":\"%s\"}\n", msg);
}

/* ---------------- 访问密钥 ---------------- */

/* 读密钥；不存在则生成一个 */
static const char *get_secret(void)
{
    static char sec[40];
    FILE *f;
    int fd;
    size_t n;
    unsigned seed;

    f = fopen(SECRET_PATH, "r");
    if (f) {
        n = fread(sec, 1, sizeof(sec) - 1, f);
        fclose(f);
        sec[n] = '\0';
        if (n >= 8) return sec;
    }

    /* 首次运行：生成一个足够随机的密钥 */
    seed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 16)
         ^ (unsigned)(size_t)&sec;
    f = fopen("/dev/urandom", "r");
    if (f) {
        unsigned r1 = 0, r2 = 0;
        if (fread(&r1, sizeof(r1), 1, f) == 1 &&
            fread(&r2, sizeof(r2), 1, f) == 1) {
            seed ^= r1 * 2654435761u ^ r2;
        }
        fclose(f);
    }
    snprintf(sec, sizeof(sec), "%08x%08x", seed, (unsigned)getpid() * 2654435761u);

    fd = open(SECRET_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        ssize_t w = write(fd, sec, strlen(sec));
        (void)w;
        close(fd);
    }
    return sec;
}

/* 从 HTTP_COOKIE 里取出 cams=<secret> 并比对 */
static int check_secret(void)
{
    const char *ck = getenv("HTTP_COOKIE");
    const char *s;
    size_t n;

    if (!ck) return -1;
    s = strstr(ck, "cams=");
    if (!s) return -1;
    s += 5;
    n = strcspn(s, "; \r\n");
    if (n == 0 || n >= 40) return -1;

    {
        char got[40];
        memcpy(got, s, n);
        got[n] = '\0';
        if (strcmp(got, get_secret()) != 0)
            return -1;
    }
    return 0;
}

/* ---------------- 通用辅助 ---------------- */

/* 读 POST 体 */
static int read_post(char *buf, size_t cap)
{
    const char *clen = getenv("CONTENT_LENGTH");
    size_t want, got = 0;

    if (!clen) return -1;
    want = (size_t)atoi(clen);
    if (want == 0 || want >= cap) return -1;

    while (got < want) {
        ssize_t n = read(STDIN_FILENO, buf + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (got > 0) ? 0 : -1;
}

/* 从 /api/states 的 JSON 里挑出指定域的实体，输出 JSON 数组 */
static void emit_entities(const char *states, const char *domain)
{
    char needle[64];
    const char *p = states;
    int first = 1;
    int found = 0;

    http_out("application/json; charset=utf-8");
    printf("[");

    snprintf(needle, sizeof(needle), "\"entity_id\":\"%s.", domain);

    while ((p = strstr(p, needle)) != NULL) {
        char id[160], name[160];
        const char *q;
        size_t n;

        p += strlen(needle);
        q = strchr(p, '"');
        if (!q) break;
        n = (size_t)(q - p);
        if (n >= sizeof(id)) n = sizeof(id) - 1;
        memcpy(id, p, n);
        id[n] = '\0';

        /* 从这里往后找 friendly_name，限制在 1KB 内避免串到下一个实体 */
        {
            const char *w = strstr(p, "\"friendly_name\":\"");
            if (w && w - p < 1024) {
                w += strlen("\"friendly_name\":\"");
                const char *e = strchr(w, '"');
                if (e) {
                    size_t m = (size_t)(e - w);
                    if (m >= sizeof(name)) m = sizeof(name) - 1;
                    memcpy(name, w, m);
                    name[m] = '\0';
                } else {
                    snprintf(name, sizeof(name), "%s", id);
                }
            } else {
                snprintf(name, sizeof(name), "%s", id);
            }
        }

        {
            char esc_id[960], esc_name[960];
            s1e_json_escape(id, esc_id, sizeof(esc_id));
            s1e_json_escape(name, esc_name, sizeof(esc_name));
            printf("%s\n  {\"entity_id\":\"%s\",\"name\":\"%s\"}",
                   first ? "" : ",", esc_id, esc_name);
            first = 0;
            found++;
        }
    }

    printf("\n]\n");
    fprintf(stderr, "[cgi] %s 域共 %d 个实体\n", domain, found);
}

static int do_list(void)
{
    s1e_ha_conf conf;
    char host[128];
    int port = 8123;
    s1e_http_resp r;

    if (s1e_conf_load(&conf, NULL) != 0) {
        json_err("未配置 Home Assistant");
        return 1;
    }
    s1e_conf_split_url(conf.url, host, sizeof(host), &port, 8123);

    if (s1e_http_get(host, port, "/api/states", conf.token, &r) != 0) {
        json_err("无法连接 Home Assistant");
        return 1;
    }
    if (r.status != 200) {
        s1e_http_free(&r);
        json_err("Home Assistant 返回错误");
        return 1;
    }

    emit_entities((const char *)r.body, "camera");
    s1e_http_free(&r);
    return 0;
}

static int do_get(void)
{
    s1e_cam_cfg cfg;
    char esc[960];

    s1e_camcfg_load(&cfg, NULL);
    s1e_json_escape(cfg.entity, esc, sizeof(esc));

    http_out("application/json; charset=utf-8");
    printf("{\"entity\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":%d,"
           "\"camera_page\":%d,\"total_pages\":%d}\n",
           esc, cfg.width, cfg.height, cfg.fps,
           cfg.camera_page, cfg.total_pages);
    return 0;
}

static int do_save(void)
{
    static char body[MAX_POST];
    s1e_cam_cfg cfg;
    char esc[960];
    const char *entity;

    if (read_post(body, sizeof(body)) != 0) {
        json_err("未收到配置内容");
        return 1;
    }

    if (s1e_json_get_string(body, "entity", cfg.entity, sizeof(cfg.entity)) != 0 ||
        cfg.entity[0] == '\0') {
        json_err("缺少 entity 字段");
        return 1;
    }

    cfg.width       = s1e_json_get_int(body, "width", 640);
    cfg.height      = s1e_json_get_int(body, "height", 480);
    cfg.fps         = s1e_json_get_int(body, "fps", 3);
    cfg.camera_page = s1e_json_get_int(body, "camera_page", 2);
    cfg.total_pages = s1e_json_get_int(body, "total_pages", 3);

    /* 范围钳位，避免手滑填出荒谬值 */
    if (cfg.width  < 160 || cfg.width  > 1920) cfg.width = 640;
    if (cfg.height < 120 || cfg.height > 1080) cfg.height = 480;
    if (cfg.fps < 1 || cfg.fps > 15)           cfg.fps = 3;
    if (cfg.total_pages < 1 || cfg.total_pages > 16) cfg.total_pages = 3;
    if (cfg.camera_page < 0) cfg.camera_page = 0;
    if (cfg.camera_page > cfg.total_pages - 1) cfg.camera_page = cfg.total_pages - 1;

    entity = cfg.entity;
    if (s1e_camcfg_write_default(NULL, entity) != 0) {
        json_err("写入配置文件失败");
        return 1;
    }

    s1e_json_escape(cfg.entity, esc, sizeof(esc));
    http_out("application/json; charset=utf-8");
    printf("{\"status\":\"ok\",\"entity\":\"%s\",\"width\":%d,\"height\":%d,"
           "\"fps\":%d,\"camera_page\":%d,\"total_pages\":%d}\n",
           esc, cfg.width, cfg.height, cfg.fps,
           cfg.camera_page, cfg.total_pages);
    return 0;
}

/*
 * 从 HA 取一帧 JPEG 直接输出。
 * 输出的地址是无认证可访问的，因此可以喂给作者 push_notify 的 image_url，
 * 让它原生的全屏弹窗显示 HA 摄像头画面。
 */
static int do_snapshot(void)
{
    s1e_ha_conf conf;
    s1e_cam_cfg cfg;
    char host[128], path[320];
    int port = 8123;
    s1e_http_resp r;

    if (s1e_conf_load(&conf, NULL) != 0) {
        json_err("未配置 Home Assistant");
        return 1;
    }
    s1e_conf_split_url(conf.url, host, sizeof(host), &port, 8123);
    s1e_camcfg_load(&cfg, NULL);

    snprintf(path, sizeof(path), "/api/camera_proxy/%s?width=%d&height=%d",
             cfg.entity, cfg.width, cfg.height);

    if (s1e_http_get(host, port, path, conf.token, &r) != 0) {
        json_err("无法连接 Home Assistant");
        return 1;
    }
    if (r.status != 200 || r.len < 4 ||
        r.body[0] != 0xFF || r.body[1] != 0xD8) {
        s1e_http_free(&r);
        json_err("未取到 JPEG");
        return 1;
    }

    http_out("image/jpeg");
    fwrite(r.body, 1, r.len, stdout);
    fflush(stdout);

    fprintf(stderr, "[cgi] 输出快照 %zu 字节 (%s)\n", r.len, cfg.entity);
    s1e_http_free(&r);
    return 0;
}

/*
 * 输出设置页 HTML，并通过 Set-Cookie 把访问密钥交给浏览器。
 * 页面与本接口同源，浏览器会自动带上这个 Cookie；
 * 跨源站点既拿不到 Cookie，也读不到响应（无 CORS 头）。
 */
static int do_page(void)
{
    static char html[64 * 1024];
    FILE *f;
    size_t n;

    f = fopen("/tmp/www/cam_settings.html", "r");
    if (!f) {
        json_err("设置页文件不存在：/tmp/www/cam_settings.html");
        return 1;
    }
    n = fread(html, 1, sizeof(html) - 1, f);
    fclose(f);
    html[n] = '\0';

    printf("Content-Type: text/html; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("Set-Cookie: cams=%s; HttpOnly; Path=/; SameSite=Strict\r\n",
           get_secret());
    printf("\r\n");
    fwrite(html, 1, n, stdout);
    fflush(stdout);
    return 0;
}

int main(void)
{
    const char *qs = getenv("QUERY_STRING");
    char action[32] = "";

    if (qs) {
        const char *a = strstr(qs, "action=");
        if (a) {
            a += strlen("action=");
            size_t i = 0;
            while (*a && *a != '&' && i < sizeof(action) - 1)
                action[i++] = *a++;
            action[i] = '\0';
        }
    }

    /* 设置页本身不需要密钥——它是拿到密钥的入口 */
    if (!strcmp(action, "page"))
        return do_page();

    /* 其余动作一律要求密钥 */
    if (check_secret() != 0) {
        http_out("application/json; charset=utf-8");
        printf("{\"status\":\"error\",\"message\":\"访问密钥缺失或错误，请从设置页进入\"}\n");
        return 1;
    }

    if (!strcmp(action, "list"))     return do_list();
    if (!strcmp(action, "get"))      return do_get();
    if (!strcmp(action, "save"))     return do_save();
    if (!strcmp(action, "snapshot")) return do_snapshot();

    json_err("未知动作");
    return 1;
}
