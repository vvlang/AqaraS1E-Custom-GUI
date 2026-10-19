/*
 * cam_view — 在 S1E 屏幕上显示 Home Assistant 的摄像头画面
 *
 * 工作流程：
 *   1. 从 /data/scripts/dev/cam_config.json 读取用哪个摄像头、分辨率、
 *      帧率和放在第几屏（命令行参数可临时覆盖）
 *   2. 从 /data/scripts/ha_config.json 读 HA 地址与令牌（令牌不出本进程）
 *   3. 只在用户翻到摄像头所在屏时，向 HA 拉一帧 JPEG
 *   4. 用 stb_image 解码，缩放到 480x480 画进 framebuffer
 *   5. 限速循环；失败时指数退避
 *
 * 与作者引擎的关系：
 *   引擎是事件驱动的，只在触摸时重绘。所以我们只在摄像头屏画，
 *   其余屏一个像素都不碰，两边互不干扰。
 *
 * 常用命令：
 *   cam_view                          按配置文件常驻运行
 *   cam_view --probe                  只解码一帧并打印尺寸，不碰屏幕
 *   cam_view --once                   画一帧就退出
 *   cam_view --entity camera.xxx      临时换一个摄像头
 *   cam_view --fps 5                  临时改帧率
 *   cam_view --write-config camera.x  把选定的摄像头写进配置文件
 *
 * 调试：设 S1E_DEBUG=1 可看到协议层细节。
 */

#include "s1e_conf.h"
#include "s1e_fb.h"
#include "s1e_http.h"
#include "s1e_touch.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "third_party/stb_image.h"

#define MAX_BACKOFF_S  30
#define DEFAULT_ENTITY "camera.chuangmi_ipc018_acd6_video_doorbell"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "用法: %s [选项]\n"
        "  --entity ID      临时指定摄像头实体（覆盖配置文件）\n"
        "  --fps N          临时指定帧率\n"
        "  --width N        临时指定宽度\n"
        "  --height N       临时指定高度\n"
        "  --pages N        临时指定总屏数\n"
        "  --camera-page N  临时指定摄像头所在屏（0 起算）\n"
        "  --write-config ID  把 ID 写进配置文件后退出\n"
        "  --probe          只解码一帧并打印尺寸，不碰屏幕\n"
        "  --once           画一帧就退出\n"
        "  --save PATH      把原始 JPEG 存盘\n"
        "\n"
        "常驻配置在 %s\n", p, S1E_CAMCFG_PATH);
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/*
 * 拉一帧并解码。
 * 成功返回 0，*out_rgb 指向 RGB888 缓冲（调用者 free）。
 * jpeg_out / jpeg_len 若非 NULL，会拿到原始 JPEG 字节（调用者 free）。
 */
static int fetch_frame(const char *host, int port, const char *token,
                       const char *entity, int req_w, int req_h,
                       uint8_t **out_rgb, int *out_w, int *out_h,
                       unsigned char **jpeg_out, size_t *jpeg_len,
                       int *http_status)
{
    char path[320];
    s1e_http_resp r;
    uint8_t *rgb;
    int w;   /* stb 要求的目标通道数，固定 3（RGB888） */

    *out_rgb = NULL;
    if (jpeg_out) *jpeg_out = NULL;
    if (http_status) *http_status = 0;

    /*
     * HA 的 camera_proxy 支持 width/height。实测这台摄像头：
     *   不带参数      -> 160x90    (3 KB)   太小
     *   640x480       -> 640x480  (12.7 KB) 推荐
     *   1280x720      -> 1280x720 (26 KB)   解码后 2.8MB，设备内存吃紧
     * RGB 缓冲 = 宽×高×3，设备只剩约 17MB，640x480 是画质与内存的平衡点。
     */
    if (req_w > 0 && req_h > 0)
        snprintf(path, sizeof(path), "/api/camera_proxy/%s?width=%d&height=%d",
                 entity, req_w, req_h);
    else
        snprintf(path, sizeof(path), "/api/camera_proxy/%s", entity);

    if (s1e_http_get(host, port, path, token, &r) != 0) {
        fprintf(stderr, "[cam] 请求失败: %s%s\n", host, path);
        return -1;
    }

    if (http_status) *http_status = r.status;

    if (r.status != 200) {
        fprintf(stderr, "[cam] HTTP %d，丢弃 %zu 字节\n", r.status, r.len);
        s1e_http_free(&r);
        return -1;
    }

    if (r.len < 4) {
        s1e_http_free(&r);
        return -1;
    }

    /* JPEG 魔数校验，快速失败 */
    if (r.body[0] != 0xFF || r.body[1] != 0xD8) {
        fprintf(stderr, "[cam] 响应不是 JPEG（前两字节 %02x %02x）\n",
                r.body[0], r.body[1]);
        s1e_http_free(&r);
        return -1;
    }

    /* 3 通道 RGB888，stb 负责分配 */
    rgb = stbi_load_from_memory(r.body, (int)r.len, out_w, out_h, &w, 3);
    if (!rgb) {
        fprintf(stderr, "[cam] JPEG 解码失败: %s\n", stbi_failure_reason());
        s1e_http_free(&r);
        return -1;
    }

    if (jpeg_out) {
        *jpeg_out = malloc(r.len);
        if (*jpeg_out) {
            memcpy(*jpeg_out, r.body, r.len);
            *jpeg_len = r.len;
        }
    }

    *out_rgb = rgb;
    s1e_http_free(&r);
    return 0;
}

int main(int argc, char **argv)
{
    s1e_cam_cfg cfg;
    s1e_ha_conf conf;
    const char *save_path = NULL;
    const char *write_cfg_entity = NULL;
    char host[128];
    int port = 8123;
    int once = 0, probe = 0;
    int backoff = 1, frames = 0;
    s1e_fb fb;
    s1e_touch touch;

    /* 先读配置文件，用户日常只改那个文件即可 */
    s1e_camcfg_load(&cfg, NULL);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--entity") && i + 1 < argc)
            snprintf(cfg.entity, sizeof(cfg.entity), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)
            cfg.fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)
            cfg.width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)
            cfg.height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pages") && i + 1 < argc)
            cfg.total_pages = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--camera-page") && i + 1 < argc)
            cfg.camera_page = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--write-config") && i + 1 < argc)
            write_cfg_entity = argv[++i];
        else if (!strcmp(argv[i], "--once"))
            once = 1;
        else if (!strcmp(argv[i], "--probe"))
            probe = 1;
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) {
            save_path = argv[++i];
            once = 1;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (cfg.fps < 1)  cfg.fps = 1;
    if (cfg.fps > 15) cfg.fps = 15;      /* 设备算力有限，别把 CPU 吃满 */
    if (cfg.total_pages < 1) cfg.total_pages = 1;
    if (cfg.camera_page < 0) cfg.camera_page = 0;
    if (cfg.camera_page > cfg.total_pages - 1)
        cfg.camera_page = cfg.total_pages - 1;

    if (write_cfg_entity) {
        s1e_camcfg_write_default(NULL, write_cfg_entity);
        fprintf(stderr, "[cfg] 已把 %s 写入 %s\n",
                write_cfg_entity, S1E_CAMCFG_PATH);
        return 0;
    }

    fprintf(stderr, "[cam] 实体=%s 分辨率=%dx%d 帧率=%d\n",
            cfg.entity, cfg.width, cfg.height, cfg.fps);
    fprintf(stderr, "[cam] 共 %d 屏，摄像头在第 %d 屏（0 起算）\n",
            cfg.total_pages, cfg.camera_page);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (s1e_conf_load(&conf, NULL) != 0) {
        fprintf(stderr, "[cam] 无法读取 HA 配置，退出\n");
        return 2;
    }
    if (s1e_conf_split_url(conf.url, host, sizeof(host), &port, 8123) != 0) {
        fprintf(stderr, "[cam] 无法解析 HA 地址\n");
        return 2;
    }
    fprintf(stderr, "[cam] HA=%s:%d\n", host, port);

    /* ---- 只解码不显示 ---- */
    if (save_path || probe) {
        uint8_t *rgb = NULL;
        int w = 0, h = 0, st = 0;
        unsigned char *jp = NULL;
        size_t jl = 0;

        if (fetch_frame(host, port, conf.token, cfg.entity,
                        cfg.width, cfg.height,
                        &rgb, &w, &h, &jp, &jl, &st) != 0)
            return 3;

        if (save_path) {
            FILE *f = fopen(save_path, "wb");
            if (f) {
                fwrite(jp, 1, jl, f);
                fclose(f);
                fprintf(stderr, "[cam] 已保存 %zu 字节到 %s\n", jl, save_path);
            } else {
                perror("[cam] 保存失败");
            }
        }
        if (probe)
            printf("HTTP=%d WIDTH=%d HEIGHT=%d JPEG_BYTES=%zu\n", st, w, h, jl);

        free(rgb);
        free(jp);
        return 0;
    }

    /* ---- 显示模式 ---- */
    if (s1e_fb_open(&fb) != 0) {
        fprintf(stderr, "[cam] 打不开 framebuffer，退出\n");
        return 4;
    }
    if (s1e_touch_open(&touch, cfg.total_pages) != 0) {
        fprintf(stderr, "[cam] 打开触摸屏失败，退出\n");
        s1e_fb_close(&fb);
        return 5;
    }
    /*
     * 这里刻意不刷底色。开机自启时引擎可能还没画完，
     * 我们抢先铺一层深色会造成开机闪屏。只在真正切进摄像头屏时才动像素。
     */

    while (!g_stop) {
        uint8_t *rgb = NULL;
        int w = 0, h = 0, st = 0;
        long t0, spent, wait;
        int was_on_camera_page = 0;

        s1e_touch_poll(&touch);

        /*
         * 只有停在摄像头屏时才画。其余屏一个像素都不碰——
         * 让引擎画它自己的页面。引擎是事件驱动的、只在触摸时重绘，
         * 所以我们不画的时候它画的内容会一直保持。
         */
        if (touch.page != cfg.camera_page) {
            was_on_camera_page = 0;
            usleep(60000);
            continue;
        }

        /* 刚切进摄像头屏：先铺底色，避免残留上一屏的内容 */
        if (!was_on_camera_page) {
            s1e_fb_fill_rgb(&fb, 16, 20, 28);
            was_on_camera_page = 1;
            fprintf(stderr, "[cam] 进入第 %d 屏，开始绘制\n", cfg.camera_page);
        }

        t0 = now_ms();

        if (fetch_frame(host, port, conf.token, cfg.entity,
                        cfg.width, cfg.height,
                        &rgb, &w, &h, NULL, NULL, &st) == 0) {
            s1e_fb_blit_rgb(&fb, rgb, w, h);
            free(rgb);
            frames++;
            backoff = 1;
            fprintf(stderr, "[cam] 第 %d 帧 %dx%d (HTTP %d, %ld ms)\n",
                    frames, w, h, st, now_ms() - t0);
        } else {
            fprintf(stderr, "[cam] 取帧失败，%d 秒后重试\n", backoff);
            sleep((unsigned)backoff);
            if (backoff < MAX_BACKOFF_S) backoff *= 2;
            continue;
        }

        if (once) break;

        /* 限速：扣掉本帧耗时 */
        spent = now_ms() - t0;
        wait  = 1000L / cfg.fps - spent;
        if (wait > 0) usleep((useconds_t)(wait * 1000));
    }

    fprintf(stderr, "[cam] 退出，共显示 %d 帧，识别翻页 %d 次\n",
            frames, touch.swipes);
    s1e_touch_close(&touch);
    s1e_fb_close(&fb);
    return 0;
}
