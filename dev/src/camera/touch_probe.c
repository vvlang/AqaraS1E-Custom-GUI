/*
 * touch_probe — 转储触摸设备的原始事件
 *
 * 用途：实现横扫切页之前，先搞清楚这台设备的输入设备到底报什么。
 * 事件格式、坐标范围、多点触控槽位数量都因设备而异，不能靠猜。
 *
 * 用法：
 *   touch_probe            监听 10 秒，打印全部事件
 *   touch_probe 30         监听 30 秒
 *   touch_probe 10 ev0     只监听 /dev/input/event0
 *
 * 32 位 ARM 上 struct input_event 是 16 字节：
 *   struct timeval time (8) + __u16 type + __u16 code + __s32 value
 * 直接按字节解析，不依赖本地内核头的结构体布局。
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EV_SIZE 16

/* 常见事件类型 */
static const char *ev_type(int t)
{
    switch (t) {
    case 0:  return "SYN";
    case 1:  return "KEY";
    case 2:  return "REL";
    case 3:  return "ABS";
    case 4:  return "MSC";
    default: return "?";
    }
}

static const char *abs_code(int c)
{
    switch (c) {
    case 0x00: return "ABS_X(0)";
    case 0x01: return "ABS_Y(1)";
    case 0x2f: return "MT_SLOT(47)";
    case 0x35: return "MT_POSITION_X(53)";
    case 0x36: return "MT_POSITION_Y(54)";
    case 0x37: return "MT_TRACKING_ID(55)";
    case 0x38: return "MT_PRESSURE(56)";
    case 0x39: return "MT_TOUCH_MAJOR(57)";
    case 0x3a: return "MT_TOUCH_MINOR(58)";
    default:   return NULL;
    }
}

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 10;
    const char *dev = (argc > 2) ? argv[2] : "/dev/input/event0";
    unsigned char buf[4096];
    ssize_t n;
    int count = 0;

    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("[touch] open");
        return 1;
    }
    fprintf(stderr, "[touch] 监听 %s，%d 秒。请触摸屏幕。\n", dev, secs);
    fprintf(stderr, "[touch] 格式: <type:u16> <code:u16> <value:s32>\n\n");

    {
        /* 按真实时间退出，而不是按循环次数——
         * 之前用 secs 次迭代 × 每次 100ms，实际只监听了 secs/10 秒。 */
        time_t deadline = time(NULL) + secs;

        while (time(NULL) < deadline) {
            n = read(fd, buf, sizeof(buf));
            if (n <= 0) { usleep(20000); continue; }

            for (ssize_t i = 0; i + EV_SIZE <= n; i += EV_SIZE) {
                unsigned char *e = buf + i;
                int type = e[8] | (e[9] << 8);
                int code = e[10] | (e[11] << 8);
                int value = (int)((unsigned)e[12] | ((unsigned)e[13] << 8) |
                                  ((unsigned)e[14] << 16) |
                                  ((unsigned)e[15] << 24));

                const char *tn = ev_type(type);
                const char *an = (type == 3) ? abs_code(code) : NULL;

                if (an)
                    printf("ABS  %-16s value=%d\n", an, value);
                else if (type == 0 && code == 0)
                    printf("SYN_REPORT\n");
                else
                    printf("%-4s code=0x%02x(%d) value=%d\n", tn, code, code, value);
                count++;
            }
            fflush(stdout);
        }
    }

    close(fd);
    fprintf(stderr, "[touch] 共 %d 个事件\n", count);
    return 0;
}
