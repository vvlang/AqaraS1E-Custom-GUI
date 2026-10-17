/*
 * snap_fast — framebuffer 截图，输出 24 位 BMP
 *
 * 这是作者闭源 snap_fast 的开源替代实现。与原版的差异：
 *   - 原版固定 mmap 0x1C0000 字节并假定像素布局，不符则输出错色
 *   - 本版从内核读取真实 bpp / line_length / 位域，按需做 RGB565→BGR888 转换
 *
 * 用法：snap_fast [输出路径]   默认 /tmp/www/snap_hd.bmp
 * 退出码：0 成功 / 1 用法错 / 2 打不开 fb / 3 ioctl 失败 / 4 mmap 失败 / 5 写文件失败
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

#define FB_DEV   "/dev/fb0"
#define DEFAULT_OUT "/tmp/www/snap_hd.bmp"

#define BMP_HEADER_SIZE 54
#define BMP_BPP         24

struct snap_ctx {
    int fd;
    struct fb_var_screeninfo vsi;
    struct fb_fix_screeninfo fsi;
    unsigned char *base;
    size_t map_len;
};

/* BMP 行数据必须 4 字节对齐 */
static inline size_t bmp_row_bytes(uint32_t width)
{
    return (((size_t)width * BMP_BPP + 31u) / 32u) * 4u;
}

/*
 * 从 framebuffer 取一个像素，按内核给出的位域还原成 8 位 r/g/b。
 * 不假设 RGB565 或 RGB888——位域布局各平台不同，设备可能是 16/24/32 bpp。
 */
static inline void fb_get_rgb(const unsigned char *row, uint32_t px,
                              const struct fb_var_screeninfo *v,
                              uint8_t *out_rgb)
{
    size_t bpp_bytes = (v->bits_per_pixel + 7u) / 8u;
    uint32_t raw = 0;

    switch (bpp_bytes) {
    case 2: {
        uint16_t tmp;
        memcpy(&tmp, row + px * 2, 2);
        raw = tmp;
        break;
    }
    case 3:
        raw = (uint32_t)row[px * 3 + 0] |
              ((uint32_t)row[px * 3 + 1] << 8) |
              ((uint32_t)row[px * 3 + 2] << 16);
        break;
    case 4: {
        uint32_t tmp;
        memcpy(&tmp, row + px * 4, 4);
        raw = tmp;
        break;
    }
    default:
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
        return;
    }

    /* 按位域长度截取，再按位域长度放大回 8 位 */
    uint32_t r = (v->red.length   ? (raw >> v->red.offset)   : (raw >> 16)) & 0xff;
    uint32_t g = (v->green.length ? (raw >> v->green.offset) : (raw >> 8))  & 0xff;
    uint32_t b = (v->blue.length  ? (raw >> v->blue.offset)  : raw)        & 0xff;

    if (v->red.length && v->red.length < 8)
        r = (r * 255u) / ((1u << v->red.length) - 1u);
    if (v->green.length && v->green.length < 8)
        g = (g * 255u) / ((1u << v->green.length) - 1u);
    if (v->blue.length && v->blue.length < 8)
        b = (b * 255u) / ((1u << v->blue.length) - 1u);

    out_rgb[0] = (uint8_t)r;
    out_rgb[1] = (uint8_t)g;
    out_rgb[2] = (uint8_t)b;
}

static void put_u16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void put_u32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }

static int write_bmp(const char *path, const struct snap_ctx *c)
{
    uint32_t w = c->vsi.xres;
    uint32_t h = c->vsi.yres;
    size_t row_bytes = bmp_row_bytes(w);
    size_t pixel_data_size = row_bytes * h;
    size_t file_size = BMP_HEADER_SIZE + pixel_data_size;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[!] fopen %s failed: %s\n", path, strerror(errno));
        return 5;
    }

    unsigned char hdr[BMP_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));

    hdr[0] = 'B'; hdr[1] = 'M';
    put_u32(hdr + 2,  (uint32_t)file_size);
    put_u32(hdr + 10, BMP_HEADER_SIZE);       /* 像素数据偏移 */
    put_u32(hdr + 14, 40);                    /* BITMAPINFOHEADER */
    put_u32(hdr + 18, w);
    put_u32(hdr + 22, (uint32_t)(-(int32_t)h)); /* 负高度 = 自上而下 */
    put_u16(hdr + 26, 1);                     /* planes */
    put_u16(hdr + 28, BMP_BPP);
    put_u32(hdr + 30, 0);                     /* 不压缩 */
    put_u32(hdr + 34, (uint32_t)pixel_data_size);
    put_u32(hdr + 38, 2835);                  /* 72 DPI */
    put_u32(hdr + 42, 2835);

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fprintf(stderr, "[!] write BMP header failed\n");
        fclose(f);
        return 5;
    }

    unsigned char *row_out = malloc(row_bytes);
    if (!row_out) {
        fprintf(stderr, "[!] out of memory\n");
        fclose(f);
        return 5;
    }

    for (uint32_t y = 0; y < h; y++) {
        const unsigned char *row_in =
            c->base + (size_t)(y + c->vsi.yoffset) * c->fsi.line_length;
        memset(row_out, 0, row_bytes);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t rgb[3];
            fb_get_rgb(row_in, x, &c->vsi, rgb);
            /* BMP 24 位是 BGR 顺序 */
            row_out[x * 3 + 0] = rgb[2];
            row_out[x * 3 + 1] = rgb[1];
            row_out[x * 3 + 2] = rgb[0];
        }
        if (fwrite(row_out, 1, row_bytes, f) != row_bytes) {
            fprintf(stderr, "[!] write row %u failed\n", y);
            free(row_out);
            fclose(f);
            return 5;
        }
    }

    free(row_out);
    if (fclose(f) != 0) {
        fprintf(stderr, "[!] fclose failed\n");
        return 5;
    }

    printf("[+] SUCCESS SNAP! %s (%ux%u, %zu bytes)\n", path, w, h, file_size);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = (argc >= 2) ? argv[1] : DEFAULT_OUT;

    struct snap_ctx c;
    memset(&c, 0, sizeof(c));

    c.fd = open(FB_DEV, O_RDONLY);
    if (c.fd < 0) {
        fprintf(stderr, "[!] open %s = %d\n", FB_DEV, errno);
        return 2;
    }
    printf("[+] open /dev/fb0 = %d\n", c.fd);

    if (ioctl(c.fd, FBIOGET_VSCREENINFO, &c.vsi) < 0 ||
        ioctl(c.fd, FBIOGET_FSCREENINFO, &c.fsi) < 0) {
        fprintf(stderr, "[!] ioctl failed: %s\n", strerror(errno));
        close(c.fd);
        return 3;
    }

    /*
     * 映射整个显存，不能只映射可见区域。
     *
     * 本机实测：可见区 480x480，但虚拟分辨率 480x960（上下双缓冲），
     * yoffset=480 表示当前可见的是第二个缓冲区。
     * 若只映射 line_length * yres（921600），访问时再加 yoffset，
     * 最大偏移会到 (479+480)*1920 = 1843200 —— 越界一倍，段错误。
     * 作者那版读 FBIOGET_VSCREENINFO 取 yoffset 正是为此。
     */
    c.map_len = c.fsi.smem_len;
    if (c.map_len < (size_t)c.fsi.line_length * (c.vsi.yres + c.vsi.yoffset))
        c.map_len = (size_t)c.fsi.line_length * (c.vsi.yres + c.vsi.yoffset);

    c.base = mmap(NULL, c.map_len, PROT_READ, MAP_SHARED, c.fd, 0);
    if (c.base == MAP_FAILED) {
        fprintf(stderr, "[!] mmap %zu bytes failed: %s\n", c.map_len, strerror(errno));
        close(c.fd);
        return 4;
    }
    printf("[+] mmap = %p (%zu bytes)\n", (void *)c.base, c.map_len);
    printf("[+] fb: %ux%u @ %u bpp, line_length=%u\n",
           c.vsi.xres, c.vsi.yres, c.vsi.bits_per_pixel, c.fsi.line_length);

    int rc = write_bmp(out, &c);

    munmap(c.base, c.map_len);
    close(c.fd);
    return rc;
}