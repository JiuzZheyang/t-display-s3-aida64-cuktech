#pragma once

#include "config_store.h"
#include "cJSON.h"
#include <stdbool.h>

typedef void (*http_config_cb)(void);

// 供 main.c 提供数据 / 处理命令的回调
typedef cJSON *(*port_data_cb)(void);
typedef cJSON *(*settings_cb)(void);
typedef bool (*port_control_cb)(const char *port, const char *action);
typedef bool (*setting_set_cb)(int piid, int value);
typedef bool (*protocol_toggle_cb)(const char *port, const char *protocol, bool on);
typedef bool (*ble_control_cb)(bool enable);

/* 启动仪表盘服务器（STA 模式，端口 80） */
void http_server_start(DeviceConfig *cfg, http_config_cb on_save);

/* 启动配网服务器（AP 模式，端口 80 + DNS 劫持 + captive portal） */
void http_server_start_provisioning(void);

/* 停止 HTTP 服务器 */
void http_server_stop(void);

/* 设置仪表盘回调（仅 STA 模式） */
void http_server_set_callbacks(port_data_cb ports, settings_cb settings,
                               port_control_cb port_ctl, setting_set_cb setting_set,
                               protocol_toggle_cb proto_toggle,
                               ble_control_cb ble_ctl);
