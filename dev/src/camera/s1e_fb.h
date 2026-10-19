/*
 * s1e_fb — framebuffer 打开、缩放绘制
 *
 * 与 snap_fast 共用同一套 device 事实（见 dev/README.md）：
 *   可见区 480x480，虚拟 480x960（双缓冲），yoffset=480
 *   32bpp，位域 B@0 G@8 R@16 A@24，line_length=1920
 *
 * mmap 必须覆盖完整 smem_len。只映射 line_length*yres 时，
 * 加上 yoffset 后最大访问会到 smem 的两倍处——越界一倍，必崩。
 */

#ifndef S1E_FB_H
#define S1E_FB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    uint32_t xres, yres;
    uint32_t yoffset;
    uint32_t bpp;
    uint32_t line_length;
    uint32_t smem_len;
    unsigned char *base;
    size_t map_len;
} s1e_fb;

int  s1e_fb_open(s1e_fb *fb);
void s1e_fb_close(s1e_fb *fb);

/* 把 src 的 RGB 图像缩放绘制到整个可见区。
 * src 为 RGB888 交织，src_w/src_h 为源尺寸。
 * 采用最近邻缩放——这台设备没有硬件缩放，且画面本身分辨率接近全屏，
 * 最近邻的画质损失可以接受，换来的是极低的 CPU 占用。 */
void s1e_fb_blit_rgb(s1e_fb *fb, const uint8_t *src, int src_w, int src_h);

/* 整屏填充指定 RGB 颜色 */
void s1e_fb_fill_rgb(s1e_fb *fb, uint8_t r, uint8_t g, uint8_t b);

#endif /* S1E_FB_H */
