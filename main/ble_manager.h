#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

typedef enum {
    BLE_IDLE, BLE_SCANNING, BLE_CONNECTING, BLE_AUTHENTICATING,
    BLE_READY, BLE_RECONNECT
} BLEState;

typedef void (*StateCallback)(BLEState old_state, BLEState new_state);
typedef void (*PortDataCallback)(int piid);

// 来自通知的端口数据
typedef struct {
    float voltage, current, power;
    uint8_t protocol, status;
    bool active;
} PortInfo;

void ble_manager_init(const char *device_mac, const char *device_token, const char *device_ble_key);
void ble_manager_loop(void);
BLEState ble_manager_state(void);
bool ble_manager_is_ready(void);

void ble_manager_set_state_callback(StateCallback cb);
void ble_manager_set_port_data_callback(PortDataCallback cb);

// 异步：非阻塞，结果通过 result_queue 以 RES_GET / RES_SET 形式送达
bool ble_manager_send_get_async(uint8_t piid, uint32_t poll_seq);
bool ble_manager_send_set_async(uint8_t piid, uint32_t value, uint32_t poll_seq);
// 无 RES_SET 的异步 SET（调用方自行发送 — 由 CMD_PORT 使用）
bool ble_manager_send_set_nr_async(uint8_t piid, uint32_t value);
// 同步：阻塞包装器（用于 CMD_PORT 和认证）
bool ble_manager_miot_get(uint8_t piid, uint32_t* value);
bool ble_manager_miot_set(uint8_t piid, uint32_t value);
void ble_manager_keepalive(void);
void ble_manager_disconnect(void);
void ble_manager_set_enabled(bool enabled);
bool ble_manager_is_enabled(void);

const PortInfo* ble_manager_get_ports(void);
uint32_t ble_manager_get_setting(uint8_t piid);
bool ble_manager_has_setting(uint8_t piid);
void ble_manager_store_setting(uint8_t piid, uint32_t val);
int ble_manager_pending_count(void);
