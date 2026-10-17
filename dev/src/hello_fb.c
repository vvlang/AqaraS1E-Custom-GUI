/*
 * hello_fb — S1E framebuffer 能力探针与最小绘制验证
 *
 * 用途：在动真格之前，一次性回答三个问题——
 *   1. /dev/fb0 能不能打开？(权限/驱动是否就绪)
 *   2. 屏幕的真实参数是什么？(分辨率/色深/行跨度)
 *   3. 画上去能不能显示？(最小验证)
 *
 * 用法：
 *   hello_fb probe              只打印 fb 信息，不写显存（安全，默认）
 *   hello_fb paint R G B        用指定颜色填满屏幕
 *   hello_fb rect  X Y W H R G B 在指定位置画一个实心矩形
 *
 * 退出码：0 成功 / 1 用法错误 / 2 无法打开 fb / 3 参数查询失败 / 4 mmap 失败
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define FB_DEV "/dev/fb0"

struct fb_ctx {
    int fd;
    struct fb_var_screeninfo vsi;
    struct fb_fix_screeninfo fsi;
    unsigned char *base;
    size_t map_len;
};

/* 把 bpp 和 vsi 的位域换算成每像素的字节数 */
static inline size_t bytes_per_pixel(uint32_t bpp)
{
    return (bpp + 7u) / 8u;
}

/*
 * 按 Linux framebuffer 的标准位域布局写一个像素。
 * 各平台的红/绿/蓝/透明度偏移不同，所以全部从 vsi 读，不写死。
 * 这是跨设备移植时最容易出错的地方。
 *
 * idx — 行内第几个像素（x 坐标）
 * rgb — 颜色，0x00RRGGBB
 */
static inline void put_pixel(unsigned char *row, uint32_t idx, uint32_t rgb,
                             const struct fb_var_screeninfo *v)
{
    size_t bpp_bytes = bytes_per_pixel(v->bits_per_pixel);
    unsigned char *p = row + idx * bpp_bytes;

    switch (bpp_bytes) {
    case 2: {
        /* 常见为 RGB565：rrrrrggg gggbbbbb */
        uint16_t r = (uint16_t)((v->red.length   ? rgb >> v->red.offset   : rgb >> 16) & 0xff);
        uint16_t g = (uint16_t)((v->green.length ? rgb >> v->green.offset : rgb >> 8)  & 0xff);
        uint16_t b = (uint16_t)((v->blue.length  ? rgb >> v->blue.offset  : rgb)       & 0xff);
        uint16_t val = (uint16_t)((r >> (8 - v->red.length)   << v->red.offset) |
                                  (g >> (8 - v->green.length) << v->green.offset) |
                                  (b >> (8 - v->blue.length)  << v->blue.offset));
        memcpy(p, &val, 2);
        break;
    }
    case 3:
        p[0] = (unsigned char)(rgb & 0xff);                 /* B */
        p[1] = (unsigned char)((rgb >> 8) & 0xff);          /* G */
        p[2] = (unsigned char)((rgb >> 16) & 0xff);         /* R */
        break;
    case 4:
        memcpy(p, &rgb, 4);
        break;
    default:
        break;
    }
}

static int fb_open(struct fb_ctx *c)
{
    memset(c, 0, sizeof(*c));

    c->fd = open(FB_DEV, O_RDWR);
    if (c->fd < 0) {
        fprintf(stderr, "[!] open %s failed: %s\n", FB_DEV, strerror(errno));
        return 2;
    }

    if (ioctl(c->fd, FBIOGET_VSCREENINFO, &c->vsi) < 0) {
        fprintf(stderr, "[!] FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
        close(c->fd);
        return 3;
    }
    if (ioctl(c->fd, FBIOGET_FSCREENINFO, &c->fsi) < 0) {
        fprintf(stderr, "[!] FBIOGET_FSCREENINFO failed: %s\n", strerror(errno));
        close(c->fd);
        return 3;
    }

    size_t bpp_bytes = bytes_per_pixel(c->vsi.bits_per_pixel);
    if (bpp_bytes == 0) {
        fprintf(stderr, "[!] invalid bits_per_pixel=%u\n", c->vsi.bits_per_pixel);
        close(c->fd);
        return 3;
    }

    /*
     * 映射整个显存。本机实测可见区 480x480、虚拟 480x960（双缓冲）、
     * yoffset=480。只映射 line_length*yres 会在加 yoffset 后越界一倍。
     */
    c->map_len = c->fsi.smem_len;
    if (c->map_len < (size_t)c->fsi.line_length * (c->vsi.yres + c->vsi.yoffset))
        c->map_len = (size_t)c->fsi.line_length * (c->vsi.yres + c->vsi.yoffset);

    c->base = mmap(NULL, c->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
    if (c->base == MAP_FAILED) {
        fprintf(stderr, "[!] mmap %zu bytes failed: %s\n", c->map_len, strerror(errno));
        close(c->fd);
        return 4;
    }

    return 0;
}

static void fb_close(struct fb_ctx *c)
{
    if (c->base && c->base != MAP_FAILED)
        munmap(c->base, c->map_len);
    if (c->fd >= 0)
        close(c->fd);
}

static void fb_print_info(const struct fb_ctx *c)
{
    const struct fb_var_screeninfo *v = &c->vsi;
    const struct fb_fix_screeninfo *f = &c->fsi;

    printf("=== /dev/fb0 ===\n");
    printf("  分辨率      : %u x %u\n", v->xres, v->yres);
    printf("  虚拟分辨率  : %u x %u\n", v->xres_virtual, v->yres_virtual);
    printf("  偏移        : x=%u y=%u\n", v->xoffset, v->yoffset);
    printf("  色深        : %u bpp (%zu 字节/像素)\n",
           v->bits_per_pixel, bytes_per_pixel(v->bits_per_pixel));
    printf("  行跨度      : %u 字节 (理论值 %u)\n",
           f->line_length, v->xres * (unsigned)bytes_per_pixel(v->bits_per_pixel));
    printf("  显存总量    : %u 字节\n", f->smem_len);
    printf("  位域 R      : offset=%u length=%u msb_right=%u\n",
           v->red.offset, v->red.length, v->red.msb_right);
    printf("  位域 G      : offset=%u length=%u msb_right=%u\n",
           v->green.offset, v->green.length, v->green.msb_right);
    printf("  位域 B      : offset=%u length=%u msb_right=%u\n",
           v->blue.offset, v->blue.length, v->blue.msb_right);
    printf("  位域 A      : offset=%u length=%u transp=%u\n",
           v->transp.offset, v->transp.length, v->transp.length ? 1 : 0);
    printf("  驱动        : %s\n", f->id);
    printf("  类型        : %s\n",
           f->type == FB_TYPE_PACKED_PIXELS ? "PACKED_PIXELS" : "其他");
    printf("  visual      : %u (%s)\n", f->visual,
           f->visual == FB_VISUAL_TRUECOLOR ? "TRUECOLOR" :
           f->visual == FB_VISUAL_DIRECTCOLOR ? "DIRECTCOLOR" :
           f->visual == FB_VISUAL_PSEUDOCOLOR ? "PSEUDOCOLOR" : "其他");
    printf("  mmap 长度   : %zu 字节 (%.2f MB)\n",
           c->map_len, (double)c->map_len / 1024.0 / 1024.0);
    printf("  visual      : %u\n", f->visual);
}

/* 填满整个可视区域（写入当前可见的那个缓冲区） */
static void fb_fill(struct fb_ctx *c, uint32_t rgb)
{
    for (uint32_t y = 0; y < c->vsi.yres; y++) {
        unsigned char *row =
            c->base + (size_t)(y + c->vsi.yoffset) * c->fsi.line_length;
        for (uint32_t x = 0; x < c->vsi.xres; x++)
            put_pixel(row, x, rgb, &c->vsi);
    }
}

/* 画实心矩形，自动裁剪到屏幕范围内 */
static void fb_rect(struct fb_ctx *c, uint32_t x, uint32_t y,
                    uint32_t w, uint32_t h, uint32_t rgb)
{
    if (x >= c->vsi.xres || y >= c->vsi.yres)
        return;
    if (x + w > c->vsi.xres) w = c->vsi.xres - x;
    if (y + h > c->vsi.yres) h = c->vsi.yres - y;

    for (uint32_t j = 0; j < h; j++) {
        unsigned char *row =
            c->base + (size_t)(y + j + c->vsi.yoffset) * c->fsi.line_length;
        for (uint32_t i = 0; i < w; i++)
            put_pixel(row, x + i, rgb, &c->vsi);
    }
}

static uint32_t pack_rgb(unsigned r, unsigned g, unsigned b)
{
    return ((r & 0xffu) << 16) | ((g & 0xffu) << 8) | (b & 0xffu);
}

int main(int argc, char **argv)
{
    struct fb_ctx ctx;
    int rc = fb_open(&ctx);
    if (rc != 0)
        return rc;

    if (argc < 2 || strcmp(argv[1], "probe") == 0) {
        fb_print_info(&ctx);
        fb_close(&ctx);
        return 0;
    }

    if (strcmp(argv[1], "paint") == 0 && argc == 5) {
        unsigned r = (unsigned)strtoul(argv[2], NULL, 0);
        unsigned g = (unsigned)strtoul(argv[3], NULL, 0);
        unsigned b = (unsigned)strtoul(argv[4], NULL, 0);
        fb_print_info(&ctx);
        printf("[*] filling screen with RGB(%u,%u,%u)...\n", r, g, b);
        fb_fill(&ctx, pack_rgb(r, g, b));
        printf("[+] done\n");
        fb_close(&ctx);
        return 0;
    }

    if (strcmp(argv[1], "rect") == 0 && argc == 9) {
        unsigned x = (unsigned)strtoul(argv[2], NULL, 0);
        unsigned y = (unsigned)strtoul(argv[3], NULL, 0);
        unsigned w = (unsigned)strtoul(argv[4], NULL, 0);
        unsigned h = (unsigned)strtoul(argv[5], NULL, 0);
        unsigned r = (unsigned)strtoul(argv[6], NULL, 0);
        unsigned g = (unsigned)strtoul(argv[7], NULL, 0);
        unsigned b = (unsigned)strtoul(argv[8], NULL, 0);
        fb_print_info(&ctx);
        printf("[*] drawing rect %ux%u at (%u,%u) RGB(%u,%u,%u)...\n",
               w, h, x, y, r, g, b);
        fb_rect(&ctx, x, y, w, h, pack_rgb(r, g, b));
        printf("[+] done\n");
        fb_close(&ctx);
        return 0;
    }

    fprintf(stderr, "用法: %s probe | paint R G B | rect X Y W H R G B\n", argv[0]);
    fb_close(&ctx);
    return 1;
}