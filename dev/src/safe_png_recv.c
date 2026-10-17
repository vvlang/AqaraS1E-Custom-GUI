/*
 * safe_png_recv — 通知图片接收与安全校验
 *
 * 用法：wget -q -T 4 --no-check-certificate -O - "<url>" | safe_png_recv
 *
 * 从 stdin 接收 PNG，写入 /tmp/notify_image.png，
 * 通过全部校验后原子改名并创建 /tmp/notify_image.ready 标志文件，
 * 供 s1e_standalone_app 轮询取图。
 *
 * 这是作者闭源同名工具的开源替代实现。契约保持一致：
 *   - 全程静默，无任何 stdout/stderr 输出（用于管道中间环节）
 *   - 退出码：0 落盘 / 1 打不开临时文件 / 2 签名非法 / 4 超限 / 5 过短
 *
 * 与原版的差异（原版只比对前 24 字节就判定）：
 *   - 按 PNG 规范遍历 chunk 定位 IHDR，而不是硬编码偏移 16
 *   - 校验 IEND 存在，确认文件完整而非被截断
 *   - 流式累计大小并在超限时立即中止，不写满整个限额
 */

#include <fcntl.h>
#include <stdio.h>      /* rename */
#include <string.h>
#include <unistd.h>

#define FINAL_PATH    "/tmp/notify_image.png"
#define TMP_PATH      "/tmp/notify_image.png.tmp"
#define READY_PATH    "/tmp/notify_image.ready"

#define MAX_SIZE      524288u    /* 512 KiB 上限，与原版一致 */
#define MAX_DIM       480u       /* 每边 1..480，与原版一致 */
#define READ_BUF      4096

/* PNG 文件签名：89 50 4E 47 0D 0A 1A 0A */
static const unsigned char PNG_SIG[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

static unsigned read_u32_be(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8)  |  (unsigned)p[3];
}

/*
 * 遍历 PNG chunk，返回 0 表示结构合法且见到 IEND。
 * 顺带取出 IHDR 的宽高。
 */
static int walk_png(int fd, unsigned *out_w, unsigned *out_h)
{
    unsigned char hdr[8];          /* 4 字节长度 + 4 字节类型 */
    int have_ihdr = 0;
    int have_iend = 0;
    unsigned char ihdr[13];

    if (lseek(fd, 8, SEEK_SET) != 8)
        return -1;

    while (read(fd, hdr, 8) == 8) {
        unsigned len = read_u32_be(hdr);

        if (memcmp(hdr + 4, "IHDR", 4) == 0) {
            if (len != 13 || read(fd, ihdr, 13) != 13)
                return -1;
            /* IHDR 数据区：width(4) height(4) bitDepth(1) colorType(1)
             * compression(1) filter(1) interlace(1) —— 共 13 字节 */
            *out_w = read_u32_be(ihdr + 0);
            *out_h = read_u32_be(ihdr + 4);
            have_ihdr = 1;
            /* 跳过 IHDR 的 CRC(4 字节) */
            if (lseek(fd, 4, SEEK_CUR) < 0)
                return -1;
            continue;
        }

        if (memcmp(hdr + 4, "IEND", 4) == 0) {
            have_iend = 1;
            break;
        }

        /* 跳过 data(len) + crc(4) */
        if (lseek(fd, (off_t)len + 4, SEEK_CUR) < 0)
            return -1;
    }

    if (!have_ihdr || !have_iend)
        return -1;
    return 0;
}

int main(void)
{
    static unsigned char buf[READ_BUF];
    unsigned char head[24];
    unsigned total = 0;
    int have_head = 0;
    ssize_t n;

    /* 1. 清掉上一次的残留，避免被误认为新图 */
    unlink(FINAL_PATH);
    unlink(TMP_PATH);
    unlink(READY_PATH);

    int fd = open(TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        _exit(1);

    /* 2. 流式接收，边写边校验 */
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        total += (unsigned)n;

        if (total > MAX_SIZE) {
            close(fd);
            unlink(TMP_PATH);
            _exit(4);
        }

        /* 头部攒够 24 字节后立即校验签名，不等收完整个文件 */
        if (!have_head && total >= sizeof(head)) {
            memcpy(head, buf, sizeof(head));
            have_head = 1;
            if (memcmp(head, PNG_SIG, 8) != 0) {
                close(fd);
                unlink(TMP_PATH);
                _exit(2);
            }
        }

        if (write(fd, buf, (size_t)n) != n) {
            close(fd);
            unlink(TMP_PATH);
            _exit(1);
        }
    }

    close(fd);

    /* 3. 过短：连 24 字节都没有，不可能是合法 PNG */
    if (total < sizeof(head)) {
        unlink(TMP_PATH);
        _exit(5);
    }
    if (!have_head) {
        unlink(TMP_PATH);
        _exit(2);
    }

    /* 4. 按 chunk 规范复核，并取宽高 */
    unsigned w = 0, h = 0;
    fd = open(TMP_PATH, O_RDONLY);
    if (fd < 0) {
        unlink(TMP_PATH);
        _exit(1);
    }

    if (walk_png(fd, &w, &h) != 0) {
        close(fd);
        unlink(TMP_PATH);
        _exit(2);
    }
    close(fd);

    /* 原版的判定：width-1 <= 479 && height-1 <= 479，即 1..480 */
    if (w == 0 || h == 0 || w > MAX_DIM || h > MAX_DIM) {
        unlink(TMP_PATH);
        _exit(2);
    }

    /* 5. 原子改名，随后创建就绪标志 */
    if (rename(TMP_PATH, FINAL_PATH) != 0) {
        unlink(TMP_PATH);
        _exit(1);
    }

    fd = open(READY_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
        close(fd);

    _exit(0);
}
