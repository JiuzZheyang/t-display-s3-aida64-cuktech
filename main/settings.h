#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

#define SETTINGS_NAMESPACE "aida64cfg"
#define MAX_WIFI_SSID_LEN  32
#define MAX_WIFI_PASS_LEN  64
#define MAX_SERVER_LEN     64

typedef struct {
    char wifi_ssid[MAX_WIFI_SSID_LEN + 1];
    char wifi_pass[MAX_WIFI_PASS_LEN + 1];
    char aida64_server[MAX_SERVER_LEN + 1];
    uint16_t aida64_port;               /* AIDA64 服务器端口，默认 7789 */
    bool configured;
} settings_t;

/* 从 NVS 加载设置（首次返回默认值） */
settings_t settings_load(void);

/* 保存设置到 NVS */
bool settings_save(const settings_t *s);

/* 重置为默认 */
bool settings_reset(void);

/* 获取默认 AIDA64 服务器地址 */
const char *settings_default_server(void);

/* 获取默认端口 */
uint16_t settings_default_port(void);

/* 检查是否已配置WiFi */
bool settings_is_configured(void);

#endif /* SETTINGS_H */
