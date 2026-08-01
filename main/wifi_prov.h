#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdint.h>
#include <stdbool.h>

/* 配网状态机 */
typedef enum {
    WP_STATE_IDLE = 0,
    WP_STATE_TRY_SAVED,     /* 尝试已保存的WiFi */
    WP_STATE_START_AP,     /* 启动热点等待配网 */
    WP_STATE_AP_RUNNING,   /* 热点运行中 */
    WP_STATE_CONNECTED,    /* WiFi连接成功 */
    WP_STATE_FAIL,
} wifi_prov_state_t;

/* 配网模块初始化（启动配网流程） */
void wifi_prov_init(void);

/* 查询当前状态 */
wifi_prov_state_t wifi_prov_get_state(void);

/* 获取当前连接的SSID */
const char *wifi_prov_get_connected_ssid(void);

/* 配网任务入口（内部创建） */
void wifi_prov_task(void *param);

/* 标记WiFi连接成功，触发切换到主屏 */
void wifi_prov_signal_connected(void);

/* 请求进入配网模式（保留 WiFi 配置）：设置 RTC 标志后重启 */
void wifi_prov_request_portal(void);

#endif /* WIFI_PROV_H */
