/*
 * s1e_conf — 读取设备上的 ha_config.json
 *
 * 只做一件事：从作者的配置文件里取出 ha_url 和 ha_token。
 * 刻意不引入 JSON 解析库——这个文件结构固定且极简，
 * 用字符串扫描足够，少一个依赖就少一分在设备上出问题的可能。
 *
 * 安全：令牌只在本进程内存中使用，不写日志、不回显。
 */

#ifndef S1E_CONF_H
#define S1E_CONF_H

#define S1E_CONF_PATH  "/data/scripts/ha_config.json"
#define S1E_CAMCFG_PATH "/data/scripts/dev/cam_config.json"
#define S1E_HAURL_MAX 128
#define S1E_TOKEN_MAX 256

typedef struct {
    char url[S1E_HAURL_MAX];
    char token[S1E_TOKEN_MAX];
} s1e_ha_conf;

/* cam_view 的运行参数。全部可从 /data/scripts/dev/cam_config.json 读取，
 * 命令行参数可以覆盖其中任意一项。 */
typedef struct {
    char entity[128];      /* 要显示的 HA 摄像头实体 */
    int  fps;              /* 帧率上限 */
    int  width, height;    /* 向摄像头请求的分辨率 */
    int  camera_page;      /* 摄像头所在屏，0 起算 */
    int  total_pages;      /* 总屏数 */
} s1e_cam_cfg;

/*
 * 读取摄像头配置。文件不存在时填入默认值并返回 0（不是错误——
 * 用户可能只想用命令行参数跑）。
 */
int s1e_camcfg_load(s1e_cam_cfg *cfg, const char *path);

/* 把默认配置写盘，供用户在设备上直接编辑 */
int s1e_camcfg_write_default(const char *path, const char *entity);

/*
 * 读取配置。成功返回 0；文件不存在或字段缺失返回 -1。
 * 只在出错时 fprintf 到 stderr，不打印 token 内容。
 */
int s1e_conf_load(s1e_ha_conf *out, const char *path);

/*
 * 从 ha_url 拆出 host 与 port。
 * "http://10.0.0.4:8123" -> ("10.0.0.4", 8123)
 * 无端口时用 default_port。成功返回 0。
 */
int s1e_conf_split_url(const char *url, char *host, int host_len, int *port,
                       int default_port);

#endif /* S1E_CONF_H */
