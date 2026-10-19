#include "s1e_fb.h"

#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define FB_DEV "/dev/fb0"

static inline size_t bpp_bytes(uint32_t bpp) { return (bpp + 7u) / 8u; }

/* 按内核给出的位域写一个像素。
 * 本机实测 B@0 G@8 R@16，但这里不写死——各平台布局不同。 */
static inline void put_px(unsigned char *row, uint32_t idx, uint32_t px,
                          const struct fb_var_screeninfo *v)
{
    size_t nb = bpp_bytes(v->bits_per_pixel);
    unsigned char *p = row + idx * nb;

    switch (nb) {
    case 2: {
        uint16_t val = (uint16_t)(((px >> 16) >> (8 - v->red.length)   << v->red.offset) |
                                   ((px >>  8) >> (8 - v->green.length) << v->green.offset) |
                                   ((px      ) >> (8 - v->blue.length)  << v->blue.offset));
        memcpy(p, &val, 2);
        break;
    }
    case 3:
        p[0] = (unsigned char)(px & 0xff);
        p[1] = (unsigned char)((px >> 8) & 0xff);
        p[2] = (unsigned char)((px >> 16) & 0xff);
        break;
    case 4:
        memcpy(p, &px, 4);
        break;
    default:
        break;
    }
}

int s1e_fb_open(s1e_fb *fb)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;

    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    fb->fd = open(FB_DEV, O_RDWR);
    if (fb->fd < 0) {
        perror("[fb] open " FB_DEV);
        return -1;
    }

    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &f) < 0) {
        perror("[fb] ioctl");
        close(fb->fd);
        return -1;
    }

    fb->xres        = v.xres;
    fb->yres        = v.yres;
    fb->yoffset     = v.yoffset;
    fb->bpp         = v.bits_per_pixel;
    fb->line_length = f.line_length;
    fb->smem_len    = f.smem_len;

    /* 必须覆盖整个显存，否则加 yoffset 后越界 */
    fb->map_len = f.smem_len;
    if (fb->map_len < (size_t)f.line_length * (v.yres + v.yoffset))
        fb->map_len = (size_t)f.line_length * (v.yres + v.yoffset);

    fb->base = mmap(NULL, fb->map_len, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fb->fd, 0);
    if (fb->base == MAP_FAILED) {
        perror("[fb] mmap");
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    fprintf(stderr,
            "[fb] %ux%u 虚拟=%ux%u yoffset=%u %ubpp line=%u smem=%u map=%zu\n",
            v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.yoffset,
            v.bits_per_pixel, f.line_length, f.smem_len, fb->map_len);
    return 0;
}

void s1e_fb_close(s1e_fb *fb)
{
    if (fb->base && fb->base != MAP_FAILED)
        munmap(fb->base, fb->map_len);
    if (fb->fd >= 0)
        close(fb->fd);
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;
}

/* 取一次 ioctl 参数，绘制时每像素重查太慢 */
static struct fb_var_screeninfo g_vsi;

void s1e_fb_blit_rgb(s1e_fb *fb, const uint8_t *src, int src_w, int src_h)
{
    int x, y;

    if (!fb->base || src_w <= 0 || src_h <= 0)
        return;

    /* 位域只需取一次 */
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &g_vsi) < 0)
        return;

    for (y = 0; y < (int)fb->yres; y++) {
        /* 目标行 = 屏幕行 + yoffset（写入当前可见的那个缓冲区） */
        unsigned char *row =
            fb->base + (size_t)(y + fb->yoffset) * fb->line_length;
        int sy = (int)((int64_t)y * src_h / fb->yres);
        const uint8_t *srow = src + (size_t)sy * src_w * 3;

        for (x = 0; x < (int)fb->xres; x++) {
            int sx = (int)((int64_t)x * src_w / fb->xres);
            const uint8_t *sp = srow + (size_t)sx * 3;
            uint32_t px = ((uint32_t)sp[0] << 16) | ((uint32_t)sp[1] << 8) | sp[2];
            put_px(row, (uint32_t)x, px, &g_vsi);
        }
    }
}

void s1e_fb_fill_rgb(s1e_fb *fb, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t px = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    int x, y;

    if (!fb->base)
        return;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &g_vsi) < 0)
        return;

    for (y = 0; y < (int)fb->yres; y++) {
        unsigned char *row =
            fb->base + (size_t)(y + fb->yoffset) * fb->line_length;
        for (x = 0; x < (int)fb->xres; x++)
            put_px(row, (uint32_t)x, px, &g_vsi);
    }
}
