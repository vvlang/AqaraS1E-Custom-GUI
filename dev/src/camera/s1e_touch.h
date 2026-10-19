/*
 * s1e_touch — 独立的触摸/翻页追踪
 *
 * 为什么需要自己解析触摸
 * ----------------------
 * 作者的引擎是闭源的，不提供「当前在第几屏」的状态文件（/tmp/current_page
 * 并不存在），也没有摄像头渲染能力。要让我们的程序知道用户翻到了哪一屏，
 * 只能自己读触摸设备并独立维护页码。
 *
 * 实测的设备行为（Magic-Switch-S1E-E420，见 touch_probe 采集）
 *   - 触摸屏是 /dev/input/event1，event0 没有事件
 *   - 单指协议，事件序列为：
 *       ABS_MT_TOUCH_MAJOR(57) = 1 / -1      按下 / 抬起
 *       ABS_MT_POSITION_X(53)               横向滑动时逐点上报
 *       ABS_MT_POSITION_Y(54)               仅在 Y 变化时上报
 *       EV_KEY BTN_TOUCH(330) = 1 / 0
 *       EV_SYN(0,0)                         一帧结束
 *   - 一次横扫位移约 -360，与引擎日志的 "Page switch dx=-378" 吻合
 *   - 判定阈值取 ~300：低于它多半是轻扫或误触；高于它引擎才会翻页
 *
 * 关键约束
 * --------
 * 引擎和我们的程序读的是同一个 evdev，各拿一份事件，互不影响。但两边的
 * 页码必须保持同步——所以这里严格复刻引擎的规则：只有「水平位移超过阈值
 * 且明显大于垂直位移」才算翻页，避免和下拉通知中心、上滑控制中心冲突。
 */

#ifndef S1E_TOUCH_H
#define S1E_TOUCH_H

typedef struct {
    int fd;                 /* /dev/input/event1 */
    int page;               /* 当前页，从 0 开始 */
    int total_pages;        /* 总页数，用于钳位 */
    int touching;           /* 是否处于一次手势中 */
    int start_x, start_y;
    int cur_x, cur_y;
    int swipes;             /* 累计识别到的翻页次数（诊断用） */
} s1e_touch;

/* 打开触摸屏。成功返回 0。 */
int  s1e_touch_open(s1e_touch *t, int total_pages);

/* 关闭 */
void s1e_touch_close(s1e_touch *t);

/*
 * 处理所有待读事件并更新页码。非阻塞，调用频率随意。
 * 返回本次调用中发生的翻页次数（0 或 1）。
 */
int s1e_touch_poll(s1e_touch *t);

#endif /* S1E_TOUCH_H */
