#include "s1e_touch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 32 位 ARM 上 struct input_event 为 16 字节：
 *   struct timeval time (8) + __u16 type + __u16 code + __s32 value
 * 这里按字节解析，不依赖本地内核头的结构体布局。 */
#define EV_SIZE  16

#define EV_SYN     0
#define EV_KEY     1
#define EV_ABS     3

#define BTN_TOUCH  330      /* 0x14A */
#define ABS_X      53       /* MT_POSITION_X */
#define ABS_Y      54       /* MT_POSITION_Y */

/*
 * 翻页阈值。实测一次完整横扫位移约 360，引擎日志里 357~443 都会翻页。
 * 取 300 略低于引擎的实际门槛——宁可我们比引擎灵敏一点，也不要迟钝，
 * 否则会出现「引擎已经翻页、我们还停在旧页」的错位。
 */
#define SWIPE_THRESHOLD 300

/*
 * 水平必须明显压过垂直才算翻页。
 * 下拉通知中心、上滑控制中心都是大幅纵向手势，靠这条排除。
 */
#define HORIZONTAL_RATIO 2

int s1e_touch_open(s1e_touch *t, int total_pages)
{
    memset(t, 0, sizeof(*t));
    t->total_pages = total_pages > 0 ? total_pages : 1;

    /* 实测触摸屏是 event1；event0 上没有任何事件 */
    t->fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (t->fd < 0) {
        fprintf(stderr, "[touch] 打开 /dev/input/event1 失败: %s\n",
                strerror(errno));
        return -1;
    }

    fprintf(stderr, "[touch] 已接管触摸，共 %d 屏，阈值 %d\n",
            t->total_pages, SWIPE_THRESHOLD);
    return 0;
}

void s1e_touch_close(s1e_touch *t)
{
    if (t->fd >= 0)
        close(t->fd);
    t->fd = -1;
}

int s1e_touch_poll(s1e_touch *t)
{
    unsigned char buf[4096];
    ssize_t n;
    int page_changed = 0;

    if (t->fd < 0)
        return 0;

    for (;;) {
        n = read(t->fd, buf, sizeof(buf));
        if (n <= 0)
            break;          /* EAGAIN：没有更多事件了 */

        for (ssize_t i = 0; i + EV_SIZE <= n; i += EV_SIZE) {
            const unsigned char *e = buf + i;
            int type = e[8] | (e[9] << 8);
            int code = e[10] | (e[11] << 8);
            int value = (int)((unsigned)e[12] | ((unsigned)e[13] << 8) |
                              ((unsigned)e[14] << 16) |
                              ((unsigned)e[15] << 24));

            if (type == EV_ABS && code == ABS_X) {
                t->cur_x = value;
            } else if (type == EV_ABS && code == ABS_Y) {
                t->cur_y = value;
            } else if (type == EV_KEY && code == BTN_TOUCH) {
                if (value == 1) {
                    /* 按下：记录起点 */
                    t->touching = 1;
                    t->start_x = t->cur_x;
                    t->start_y = t->cur_y;
                } else {
                    /* 抬起：判定这次手势 */
                    if (!t->touching)
                        continue;
                    t->touching = 0;

                    int dx = t->cur_x - t->start_x;
                    int dy = t->cur_y - t->start_y;
                    int adx = dx < 0 ? -dx : dx;
                    int ady = dy < 0 ? -dy : dy;

                    if (adx >= SWIPE_THRESHOLD &&
                        adx > ady * HORIZONTAL_RATIO) {
                        int old = t->page;
                        if (dx < 0)
                            t->page++;        /* 左滑：下一页 */
                        else
                            t->page--;        /* 右滑：上一页 */

                        if (t->page < 0)
                            t->page = 0;
                        if (t->page > t->total_pages - 1)
                            t->page = t->total_pages - 1;

                        if (t->page != old) {
                            t->swipes++;
                            page_changed++;
                            fprintf(stderr, "[touch] 翻页 dx=%d -> 第 %d 屏\n",
                                    dx, t->page);
                        }
                    }
                }
            }
        }
    }

    return page_changed;
}
