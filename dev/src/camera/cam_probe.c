/*
 * cam_probe — 摄像头链路探针（不依赖 JPEG 解码器）
 *
 * 用途：在引入完整解码器之前，先把「设备 -> HA -> 拿回 JPEG」这段
 * 网络链路单独验证掉。它只做：
 *   1. 读 ha_config.json
 *   2. 向 HA 请求一帧摄像头图像
 *   3. 校验 JPEG 魔数，并从 SOFn 标记里解析出图像尺寸
 *
 * 解析尺寸不需要解码——JPEG 的 SOF 段头部就存着宽高，
 * 所以这个探针不链接任何解码器，能独立验证网络与协议部分。
 *
 * 用法：
 *   cam_probe                          用默认实体探测
 *   cam_probe --entity camera.xxx      指定实体
 *   cam_probe --save /tmp/f.jpg        同时把原始 JPEG 存盘
 */

#include "s1e_conf.h"
#include "s1e_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_ENTITY "camera.chuangmi_ipc018_acd6_video_doorbell"

/*
 * 从 JPEG 数据里找 SOFn 标记并读出宽高。
 *
 * 结构：FFD8 (SOI) 后跟若干段，每段形如
 *   FF Cx | 长度(2B BE) | 数据
 * SOF0..SOF15（除 C4/C8/CC）里首字节是精度，紧随 2 字节高、2 字节宽。
 * 遇到 SOS(FFDA) 或 EOI(FFD9) 仍未找到则失败。
 */
static int jpeg_dims(const unsigned char *d, size_t n, int *w, int *h)
{
    size_t i = 2;   /* 跳过 FFD8 */

    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return -1;

    while (i + 3 < n) {
        if (d[i] != 0xFF) { i++; continue; }

        unsigned char marker = d[i + 1];
        if (marker == 0xFF) { i++; continue; }          /* 填充字节 */
        if (marker == 0xD8 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }

        if (marker == 0xD9)                            /* EOI */
            return -1;
        if (marker == 0xDA)                            /* SOS: 图像数据开始 */
            return -1;

        if (i + 3 >= n) return -1;
        size_t seglen = ((size_t)d[i + 2] << 8) | d[i + 3];

        /* SOF0-SOF15，排除 DHT(C4)/JPG(C8)/DAC(CC) */
        if (marker >= 0xC0 && marker <= 0xCF &&
            marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (i + 9 >= n) return -1;
            *h = (d[i + 5] << 8) | d[i + 6];
            *w = (d[i + 7] << 8) | d[i + 8];
            return (*w > 0 && *h > 0) ? 0 : -1;
        }

        i += 2 + seglen;   /* 标记已算过，长度不含标记本身 */
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *entity = DEFAULT_ENTITY;
    const char *save_path = NULL;
    int req_w = 0, req_h = 0;
    s1e_ha_conf conf;
    char host[128];
    int port = 8123;
    char path[256];
    s1e_http_resp r;
    int w = 0, h = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--entity") && i + 1 < argc)     entity = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) { save_path = argv[++i]; }
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)  req_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) req_h = atoi(argv[++i]);
        else {
            fprintf(stderr, "用法: %s [--entity ID] [--save PATH] "
                            "[--width N] [--height N]\n", argv[0]);
            return 1;
        }
    }

    if (s1e_conf_load(&conf, NULL) != 0) {
        fprintf(stderr, "[probe] 读取 HA 配置失败\n");
        return 2;
    }
    if (s1e_conf_split_url(conf.url, host, sizeof(host), &port, 8123) != 0) {
        fprintf(stderr, "[probe] 无法解析 HA 地址\n");
        return 2;
    }

    /*
     * HA 的 camera_proxy 支持 width/height 参数，实测有效：
     *   默认              -> 160x90,  3.0 KB
     *   ?width=640&...    -> 分辨率显著提高, 12.7 KB
     * 摄像头会按请求的上限返回它能给的最大尺寸。
     */
    if (req_w > 0 && req_h > 0)
        snprintf(path, sizeof(path), "/api/camera_proxy/%s?width=%d&height=%d",
                 entity, req_w, req_h);
    else
        snprintf(path, sizeof(path), "/api/camera_proxy/%s", entity);

    fprintf(stderr, "[probe] 请求 %s:%d%s\n", host, port, path);

    if (s1e_http_get(host, port, path, conf.token, &r) != 0) {
        fprintf(stderr, "[probe] 连接或请求失败\n");
        return 3;
    }

    printf("HTTP_STATUS=%d\n", r.status);
    printf("BODY_BYTES=%zu\n", r.len);

    if (r.status != 200) {
        /* 打印错误正文，HA 通常会说明原因（如 401/400/404） */
        fprintf(stderr, "[probe] 错误正文(%zu 字节): %.*s\n",
                r.len, (int)(r.len > 300 ? 300 : r.len), (const char *)r.body);
        printf("RESULT=HTTP_ERROR\n");
        s1e_http_free(&r);
        return 3;
    }

    if (r.len < 4 || r.body[0] != 0xFF || r.body[1] != 0xD8) {
        printf("RESULT=NOT_JPEG\n");
        s1e_http_free(&r);
        return 3;
    }
    printf("JPEG_MAGIC=OK\n");

    if (jpeg_dims(r.body, r.len, &w, &h) == 0) {
        printf("IMAGE_WIDTH=%d\n", w);
        printf("IMAGE_HEIGHT=%d\n", h);
        printf("RESULT=OK\n");
    } else {
        printf("RESULT=DIMS_PARSE_FAILED\n");
    }

    if (save_path) {
        FILE *f = fopen(save_path, "wb");
        if (f) {
            fwrite(r.body, 1, r.len, f);
            fclose(f);
            fprintf(stderr, "[probe] 已保存 %zu 字节到 %s\n", r.len, save_path);
        } else {
            perror("[probe] 保存失败");
        }
    }

    s1e_http_free(&r);
    return 0;
}
