#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"

#include "lcd_i80.h"
#include "lv_port.h"
#include "lvgl.h"
#include "aida64.h"
#include "ui.h"
#include "boot_anim.h"
#include "backlight.h"
#include "button.h"
#include "settings.h"
#include "wifi_prov.h"
#include "http_server.h"
#include "bemfa.h"
#include "battery.h"
#include "mbedtls/md.h"
#include "mbedtls/ccm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/hkdf.h"

#include "ble_manager.h"
#include "config_store.h"
#include "queue_msg.h"

static const char *TAG = "main";

static bool aida64_started = false;
static bool ble_started = false;
static char s_server_ip[64] = {0};
static bool s_buttons_locked = true;  /* 启动完成前禁用按钮（除长按KEY） */

/* 屏幕息屏控制标志位，由 on_key_brightness 设置，app_task 消费 */
static bool s_screen_off = false;
static volatile bool s_aida64_need_stop = false;
static volatile bool s_aida64_need_start = false;

// ============================================================
// 消息队列（由 queue_msg.h 引用）
// ============================================================
QueueHandle_t cmd_queue = NULL;
QueueHandle_t urgent_queue = NULL;
QueueHandle_t result_queue = NULL;

// ============================================================
// 共享状态（来自仓库 main.c）
// ============================================================
static SemaphoreHandle_t state_mutex = NULL;

typedef struct {
    float voltage, current, power;
    uint8_t protocol, status;
    bool active, valid;
} PortData;

static PortData port_data[4] = {};
static uint32_t settings[32] = {};
static bool settings_valid[32] = {};

#define PIID21_ALL_ON  0x03030F0F
static uint32_t protocol_extend_val = PIID21_ALL_ON;
static bool protocol_extend_valid = true;
static uint32_t port_ctrl_val = 0xFF;
static bool port_ctrl_valid = true;
static bool ble_enabled = true;
static bool ble_ready_flag = false;

/* 倒计时追踪：记录 PIID 9/10/11/12 设置时的时间戳（秒） */
static uint32_t cd_set_time[4] = {0, 0, 0, 0};
static uint32_t cd_set_val[4]  = {0, 0, 0, 0};  /* 设置时的倒计时值（分钟） */
static int      cd_action[4]   = {0, 0, 0, 0};  /* 倒计时到点后的目标动作：1=开启, -1=关闭, 0=无 */

bool ble_get_ready_flag(void) {
    return ble_ready_flag;
}


#define LOCK_STATE()   do { if (state_mutex) xSemaphoreTake(state_mutex, portMAX_DELAY); } while(0)
#define UNLOCK_STATE() do { if (state_mutex) xSemaphoreGive(state_mutex); } while(0)

static const char* PROTO_NAMES[] = {"idle","5V","5V","QC","AFC","FCP","SCP","PD","PPS","PPS","UFCS"};
static const int PROTO_NAMES_LEN = sizeof(PROTO_NAMES)/sizeof(PROTO_NAMES[0]);
static const char* get_proto_name(uint8_t code) {
    return (code < PROTO_NAMES_LEN) ? PROTO_NAMES[code] : "?";
}

// ============================================================
// HTTP 服务器回调（来自仓库 main.c）
// ============================================================
static cJSON* get_port_data_json(void) {
    cJSON *arr = cJSON_CreateArray();
    const char *names[] = {"C1", "C2", "C3", "USB-A"};
    LOCK_STATE();
    for (int i = 0; i < 4; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "port", names[i]);
        cJSON_AddNumberToObject(obj, "voltage", port_data[i].voltage);
        cJSON_AddNumberToObject(obj, "current", port_data[i].current);
        cJSON_AddNumberToObject(obj, "power", port_data[i].power);
        const char *proto = port_data[i].active ? get_proto_name(port_data[i].protocol) : "idle";
        cJSON_AddStringToObject(obj, "protocol", proto);
        cJSON_AddBoolToObject(obj, "active", port_data[i].active);
        cJSON_AddNumberToObject(obj, "status_raw", port_data[i].status);
        cJSON_AddBoolToObject(obj, "enabled", (port_ctrl_val >> i) & 1);
        cJSON_AddItemToArray(arr, obj);
    }
    UNLOCK_STATE();
    return arr;
}

static cJSON* get_settings_json(void) {
    cJSON *root = cJSON_CreateObject();
    LOCK_STATE();
    for (int i = 0; i < 32; i++) {
        if (settings_valid[i]) {
            char key[8]; snprintf(key, sizeof(key), "%d", i);
            cJSON_AddNumberToObject(root, key, settings[i]);
        }
    }
    if (port_ctrl_valid) {
        char key[8]; snprintf(key, sizeof(key), "%d", 16);
        cJSON_AddNumberToObject(root, key, port_ctrl_val);
    }
    if (protocol_extend_valid) {
        char key[8]; snprintf(key, sizeof(key), "%d", 21);
        cJSON_AddNumberToObject(root, key, protocol_extend_val);
    }
    UNLOCK_STATE();
    cJSON_AddBoolToObject(root, "ble_enabled", ble_manager_is_enabled());

    /* 实时倒计时剩余秒数 */
    uint32_t now = esp_timer_get_time() / 1000000;
    cJSON *cd = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON *item = cJSON_CreateObject();
        if (cd_set_time[i] > 0 && cd_set_val[i] > 0) {
            uint32_t elapsed = now - cd_set_time[i];
            int32_t remain = (int32_t)(cd_set_val[i] * 60) - (int32_t)elapsed;
            if (remain < 0) remain = 0;
            cJSON_AddNumberToObject(item, "remain", remain);      /* 剩余秒 */
            cJSON_AddNumberToObject(item, "total",  cd_set_val[i] * 60); /* 总秒 */
            cJSON_AddNumberToObject(item, "action", cd_action[i]); /* 1=到时开启, -1=到时关闭, 0=无 */
        } else {
            cJSON_AddNumberToObject(item, "remain", 0);
            cJSON_AddNumberToObject(item, "total",  0);
            cJSON_AddNumberToObject(item, "action", 0);
        }
        cJSON_AddItemToArray(cd, item);
    }
    cJSON_AddItemToObject(root, "cd_remains", cd);
    return root;
}

static bool handle_port_control(const char *port, const char *action) {
    if (!port || !action) return false;
    int bit = -1;
    if (strcmp(port, "c1") == 0) bit = 0;
    else if (strcmp(port, "c2") == 0) bit = 1;
    else if (strcmp(port, "c3") == 0) bit = 2;
    else if (strcmp(port, "a") == 0) bit = 3;
    if (bit < 0) return false;
    bool on = (strcmp(action, "on") == 0);
    BleCommand cmd = {CMD_PORT, (uint8_t)bit, on ? 1 : 0, 0};
    if (xQueueSend(urgent_queue, &cmd, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "HTTP PORT %s %s dropped (queue full)", port, action);
        return false;
    }
    ESP_LOGI(TAG, "HTTP PORT %s %s (bit=%d)", port, action, bit);
    return true;
}

static bool handle_setting_set(int piid, int value) {
    if (piid <= 0 || piid >= 32) return false;
    BleCommand cmd = {CMD_SET, (uint8_t)piid, (uint32_t)value, 0};
    if (xQueueSend(urgent_queue, &cmd, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "HTTP SET piid=%d dropped (queue full)", piid);
        return false;
    }
    ESP_LOGI(TAG, "HTTP SET piid=%d value=%d", piid, value);
    return true;
}

static bool handle_protocol_toggle(const char *port, const char *protocol, bool on) {
    if (!port || !protocol) return false;
    static const struct { const char *port; const char *proto; int bit; } map[] = {
        {"c1","pd",0},{"c1","pps",1},{"c1","ufcs",2},
        {"c2","pd",8},{"c2","pps",9},{"c2","ufcs",10},
        {"c3","ufcs",16},{"c3","scp",17},
        {"a","ufcs",24},{"a","scp",25},
    };
    for (int i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (strcmp(port, map[i].port) == 0 && strcasecmp(protocol, map[i].proto) == 0) {
            uint32_t val = protocol_extend_val;
            if (on) val |= (1 << map[i].bit);
            else val &= ~(1 << map[i].bit);
            BleCommand cmd = {CMD_SET, 21, val, 0};
            if (xQueueSend(urgent_queue, &cmd, pdMS_TO_TICKS(2000)) != pdTRUE) {
                ESP_LOGW(TAG, "HTTP PROTO %s %s dropped (queue full)", port, protocol);
                return false;
            }
            ESP_LOGI(TAG, "HTTP PROTO %s %s %s (PIID21=0x%lX)", port, protocol, on?"ON":"OFF", (unsigned long)val);
            return true;
        }
    }
    return false;
}

static bool handle_ble_control(bool enable) {
    ble_enabled = enable;
    ble_manager_set_enabled(enable);
    ESP_LOGI(TAG, "HTTP BLE %s", enable ? "enable" : "disable");
    return true;
}

// ============================================================
// BLE 状态回调
// ============================================================
static void on_ble_state_change(BLEState old_state, BLEState new_state) {
    BleResult res = {RES_BLE_STATUS, true, 0, (uint32_t)(new_state == BLE_READY), 0, 0,0,0, 0, 0, false};
    xQueueSend(result_queue, &res, portMAX_DELAY);
}

// ============================================================
// 充电器端口数据回调 -> 更新 UI
// ============================================================
static void on_charger_port_data(int piid) {
    const PortInfo *ports = ble_manager_get_ports();
    float total = 0;
    bool any_active = false;
    for (int i = 0; i < 4; i++) {
        if (ports[i].active && ports[i].power > 0) {
            total += ports[i].power;
            any_active = true;
        }
    }
    ESP_LOGI(TAG, "Charger port %d updated, total=%.1fW", piid, total);
    ui_update_charger(total, any_active);
}

// ============================================================
// BLE 任务（来自仓库 main.c - 带命令处理的正式循环）
// ============================================================
static void ble_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "BLE task started");

    ble_manager_set_state_callback(on_ble_state_change);
    ble_manager_set_port_data_callback(on_charger_port_data);

    DeviceConfig dev_cfg;
    config_store_load(&dev_cfg);
    ble_manager_init(dev_cfg.device_mac, dev_cfg.device_token, dev_cfg.device_ble_key);

    BleCommand cmd;
    BleResult res;
    bool was_ready = false;

    while (1) {
        bool did_work = false;
        do {
            cmd.type = CMD_NOP;
            if (urgent_queue && xQueueReceive(urgent_queue, &cmd, 0) == pdTRUE) {
                did_work = true;
            } else if (xQueueReceive(cmd_queue, &cmd, 0) == pdTRUE) {
                did_work = true;
            }
            if (cmd.type != CMD_NOP) {
                switch (cmd.type) {
                case CMD_GET: {
                    uint32_t val = 0;
                    bool ok = ble_manager_miot_get(cmd.piid, &val);
                    if (ok) {
                        LOCK_STATE();
                        if (cmd.piid < 32) { settings[cmd.piid] = val; settings_valid[cmd.piid] = true; }
                        if (cmd.piid == 16) {
                            uint32_t old_val = port_ctrl_val;
                            port_ctrl_val = val; port_ctrl_valid = true;
                            ui_set_port_mask(val);
                            /* 端口被关闭时，同步清除对应倒计时（否则 Web 一直显示倒计时值） */
                            for (int i = 0; i < 4; i++) {
                                bool was_on = (old_val >> i) & 1;
                                bool now_on  = (val >> i) & 1;
                                if (was_on && !now_on && i < 4) {
                                    settings[9 + i] = 0;
                                    settings_valid[9 + i] = true;
                                    cd_set_time[i] = 0; cd_set_val[i] = 0; cd_action[i] = 0;
                                    ESP_LOGI(TAG, "Port %d off, cleared countdown timer PIID %d", i, 9 + i);
                                }
                            }
                        }
                        if (cmd.piid == 21) { protocol_extend_val = val; ble_manager_store_setting(21, val); }
                        UNLOCK_STATE();
                        ESP_LOGI(TAG, "GET piid=%d = %lu", cmd.piid, (unsigned long)val);
                    } else {
                        ESP_LOGW(TAG, "GET piid=%d FAILED", cmd.piid);
                    }
                    ble_manager_loop();
                    break;
                }
                case CMD_SET: {
                    ESP_LOGI(TAG, "SET piid=%d val=%lu", cmd.piid, (unsigned long)cmd.value);
                    bool ok = ble_manager_miot_set(cmd.piid, cmd.value);
                    if (ok) {
                        LOCK_STATE();
                        if (cmd.piid == 16) {
                            uint32_t old_val = port_ctrl_val;
                            port_ctrl_val = cmd.value; port_ctrl_valid = true; ui_set_port_mask(cmd.value);
                            for (int i = 0; i < 4; i++) {
                                bool was_on = (old_val >> i) & 1;
                                bool now_on  = (cmd.value >> i) & 1;
                                if (was_on && !now_on && i < 4) {
                                    settings[9 + i] = 0;
                                    settings_valid[9 + i] = true;
                                    cd_set_time[i] = 0; cd_set_val[i] = 0; cd_action[i] = 0;
                                }
                            }
                        }
                        else if (cmd.piid < 32) { 
                            settings[cmd.piid] = cmd.value; settings_valid[cmd.piid] = true;
                            /* 倒计时 PIID 9/10/11/12：记录设置时间，根据端口状态决定目标动作 */
                            if (cmd.piid >= 9 && cmd.piid <= 12) {
                                int idx = cmd.piid - 9;
                                if (cmd.value > 0) {
                                    cd_set_time[idx] = esp_timer_get_time() / 1000000; /* 秒 */
                                    cd_set_val[idx]  = cmd.value;                   /* 分钟 */
                                    /* 端口当前开启→到时关闭，端口当前关闭→到时开启 */
                                    int port_on = (port_ctrl_val >> idx) & 1;
                                    cd_action[idx] = port_on ? -1 : 1;
                                    ESP_LOGI(TAG, "Countdown set: PIID %d = %lu min, action=%d (port=%s)",
                                             cmd.piid, (unsigned long)cmd.value, cd_action[idx], port_on ? "on" : "off");
                                } else {
                                    cd_set_time[idx] = 0; cd_set_val[idx] = 0; cd_action[idx] = 0;
                                }
                            }
                        }
                        if (cmd.piid == 21) { protocol_extend_val = cmd.value; ble_manager_store_setting(21, cmd.value); }
                        UNLOCK_STATE();
                        ESP_LOGI(TAG, "SET piid=%d val=%lu OK", cmd.piid, (unsigned long)cmd.value);
                    } else {
                        ESP_LOGW(TAG, "SET piid=%d val=%lu FAILED", cmd.piid, (unsigned long)cmd.value);
                    }
                    res = (BleResult){RES_SET, ok, cmd.piid, cmd.value, 0, 0,0,0, 0, 0, false};
                    xQueueSend(result_queue, &res, 0);
                    break;
                }
                case CMD_PORT: {
                    LOCK_STATE();
                    uint32_t current = port_ctrl_val;
                    if (cmd.value) current |= (1 << cmd.piid);
                    else current &= ~(1 << cmd.piid);
                    port_ctrl_val = current;                                                      
                    port_ctrl_valid = true;                                                       
                    ui_set_port_mask(current);
                    UNLOCK_STATE();
                    if (ble_manager_miot_set(16, current)) {
                        ESP_LOGI(TAG, "CMD_PORT: SET16=0x%02lX sent", (unsigned long)current);
                    } else {
                        ESP_LOGW(TAG, "CMD_PORT: SET16=0x%02lX FAILED", (unsigned long)current);
                    }
                    res = (BleResult){RES_SET, true, 16, current, 0, 0,0,0, 0, 0, false};
                    xQueueSend(result_queue, &res, 0);
                    break;
                }
                case CMD_RECONNECT:
                case CMD_DISCONNECT:
                    ble_manager_disconnect();
                    break;
                default: break;
                }
            }
        } while (cmd.type != CMD_NOP);

        ble_manager_loop();

        {
            bool ready = ble_manager_is_ready();
            if (ready != was_ready) {
                was_ready = ready;
                res = (BleResult){RES_BLE_STATUS, true, 0, (uint32_t)ready, 0, 0,0,0, 0, 0, false};
                xQueueSend(result_queue, &res, portMAX_DELAY);
            }
        }

        if (!did_work) vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================
// 应用任务（处理 BLE 结果，更新 UI）
// ============================================================
static volatile bool s_bemfa_need_restart = false;
static void app_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(100));
    while (1) {
        BleResult res;
        while (xQueueReceive(result_queue, &res, 0) == pdTRUE) {
            switch (res.type) {
            case RES_PORT_PUSH: {
                int idx = res.piid - 1;
                if (idx >= 0 && idx < 4) {
                    LOCK_STATE();
                    port_data[idx] = (PortData){
                        res.voltage, res.current, res.power,
                        res.protocol, res.status,
                        res.status != 0 || res.voltage > 0.5f, true
                    };
                    UNLOCK_STATE();
                    // 更新 UI
                    float total = 0;
                    bool any_active = false;
                    for (int i = 0; i < 4; i++) {
                        if (port_data[i].active && port_data[i].power > 0) {
                            total += port_data[i].power;
                            any_active = true;
                        }
                    }
                    ui_update_charger(total, any_active);
                    /* 同时更新详情页 */
                    float pw[4], vo[4], cu[4];
                    const char *pr[4];
                    bool ac[4];
                    bool en[4];
                    uint8_t sr[4];
                    for (int j = 0; j < 4; j++) {
                        pw[j] = port_data[j].power;
                        vo[j] = port_data[j].voltage;
                        cu[j] = port_data[j].current;
                        pr[j] = get_proto_name(port_data[j].protocol);
                        ac[j] = port_data[j].active;
                        en[j] = (port_ctrl_val >> j) & 1;
                        sr[j] = port_data[j].status;
                    }
                    ui_update_charger_detail(pw, vo, cu, pr, ac, en, sr);
                    // 巴法云发布
                    bemfa_publish_port(idx, res.voltage, res.current, res.power, res.status != 0 || res.voltage > 0.5f);
                }
                break;
            }
            case RES_BLE_STATUS: {
                LOCK_STATE();
                ble_ready_flag = res.value != 0;
                UNLOCK_STATE();
                ESP_LOGI(TAG, "BLE state: %s", ble_ready_flag ? "READY" : "DISCONNECTED");
                ui_set_bt_connected(ble_ready_flag);
                bemfa_publish_status(res.value != 0);
                if (res.value) {
                    // BLE 连上后启动 Bemfa（延迟启动避免内存争抢）
                    static bool bemfa_started = false;
                    if (!bemfa_started) {
                        DeviceConfig dc;
                        config_store_load(&dc);
                        if (dc.bemfa_enable && dc.bemfa_uid[0]) {
                            bemfa_init(&dc, handle_port_control, handle_ble_control);
                            ESP_LOGI(TAG, "Bemfa started after BLE ready");
                        }
                        bemfa_started = true;
                    }
                    static const uint8_t READABLE_PIIDS[] = {1, 2, 3, 4, 6, 16, 21};
                    for (int i = 0; i < sizeof(READABLE_PIIDS); i++) {
                        BleCommand c = {CMD_GET, READABLE_PIIDS[i], 0, 0};
                        xQueueSend(cmd_queue, &c, 0);
                    }
                } else {
                    /* BLE 断连：清零端口数据 */
                    LOCK_STATE();
                    for (int j = 0; j < 4; j++) port_data[j].active = false;
                    UNLOCK_STATE();
                }
                break;
            }
            default: break;
            }
        }
        bemfa_loop();

        /* 倒计时到点检测：设置到时间后执行目标动作（开启/关闭对应端口） */
        {
            uint32_t now = esp_timer_get_time() / 1000000;
            for (int i = 0; i < 4; i++) {
                if (cd_set_time[i] > 0 && cd_set_val[i] > 0 && cd_action[i] != 0) {
                    uint32_t elapsed = now - cd_set_time[i];
                    if (elapsed >= (uint32_t)cd_set_val[i] * 60) {
                        bool target_on = (cd_action[i] == 1);
                        ESP_LOGI(TAG, "Countdown expired: port %d -> %s", i, target_on ? "ON" : "OFF");
                        /* 发送端口控制命令 */
                        BleCommand c = {CMD_PORT, (uint8_t)i, target_on ? 1 : 0, 0};
                        xQueueSend(urgent_queue, &c, 0);
                        /* 清除倒计时状态 */
                        LOCK_STATE();
                        cd_set_time[i] = 0; cd_set_val[i] = 0; cd_action[i] = 0;
                        settings[9 + i] = 0; settings_valid[9 + i] = true;
                        UNLOCK_STATE();
                    }
                }
            }
        }

        /* 处理 AIDA64 暂停/恢复（由按键任务设置标志位） */
        /* 背光关闭后延迟5秒关AIDA64，背光亮起时立即恢复 */
        /* 5秒内亮屏则取消计时，只要屏幕有亮度就不关AIDA64 */
        {
            static bool aida64_paused = false;
            static int64_t stop_tick = 0;

            /* 只要有亮度，立即取消待执行的停止 */
            if (!s_screen_off && stop_tick > 0) {
                stop_tick = 0;
                s_aida64_need_stop = false;
                ESP_LOGI(TAG, "AIDA64 stop cancelled (screen on before timeout)");
            }

            /* 背光亮起且AIDA64已暂停，立即恢复 */
            if (!s_screen_off && aida64_paused) {
                aida64_paused = false;
                settings_t cfg = settings_load();
                ESP_LOGI(TAG, "AIDA64 resuming to %s:%u", cfg.aida64_server, (unsigned)cfg.aida64_port);
                extern void aida64_monitor_start(const char *ip, uint16_t port);
                aida64_monitor_start(cfg.aida64_server, cfg.aida64_port);
                ESP_LOGI(TAG, "AIDA64 resumed (screen on)");
            }

            /* 息屏后触发5秒倒计时 */
            if (s_aida64_need_stop) {
                s_aida64_need_stop = false;
                if (!aida64_paused) {
                    stop_tick = esp_timer_get_time() / 1000;
                    ESP_LOGI(TAG, "AIDA64 stop scheduled in 5s");
                }
            }

            /* 5秒倒计时到，关AIDA64 */
            if (!aida64_paused && stop_tick > 0 && s_screen_off) {
                int64_t now = esp_timer_get_time() / 1000;
                if (now - stop_tick >= 5000) {
                    stop_tick = 0;
                    aida64_paused = true;
                    extern void aida64_monitor_stop(void);
                    aida64_monitor_stop();
                    ESP_LOGI(TAG, "AIDA64 paused (5s after screen off)");
                }
            }
        }

        /* 充电页面全0检测：在充电页面且非手动进入时，全0持续5秒跳回AIDA64 */
        {
            static bool last_charger_check = false;
            static int64_t charger_inactive_start = 0;
            bool on_charger = ui_is_charger_screen();
            bool manual_charger = ui_is_manual_charger();

            if (on_charger && !manual_charger) {
                /* 检查所有端口数据是否全为0 */
                bool all_zero = true;
                for (int i = 0; i < 4; i++) {
                    if (port_data[i].active && port_data[i].power > 0.05f) {
                        all_zero = false;
                        break;
                    }
                }
                if (all_zero) {
                    if (charger_inactive_start == 0) {
                        charger_inactive_start = esp_timer_get_time() / 1000;
                        ESP_LOGI(TAG, "Charger all-zero timer started");
                    } else if ((esp_timer_get_time() / 1000 - charger_inactive_start) > 5000) {
                        ESP_LOGI(TAG, "Charger all-zero 5s, switching back to AIDA64");
                        ui_switch_to_charger(false);
                        ui_clear_inactive_timer();
                        charger_inactive_start = 0;
                        lv_scr_load(ui_get_main_screen());
                    }
                } else {
                    if (charger_inactive_start != 0) {
                        ESP_LOGI(TAG, "Charger data resumed, timer cancelled");
                    }
                    charger_inactive_start = 0;
                }
            } else {
                charger_inactive_start = 0;
            }
            last_charger_check = on_charger;
        }

        /* 处理配置保存后的巴法云重启 */
        if (s_bemfa_need_restart) {
            s_bemfa_need_restart = false;
            DeviceConfig dc;
            config_store_load(&dc);
            if (dc.bemfa_enable && dc.bemfa_uid[0]) {
                bemfa_disconnect();
                bemfa_init(&dc, handle_port_control, handle_ble_control);
                ESP_LOGI(TAG, "Bemfa restarted after config save");
            } else {
                bemfa_disconnect();
                ESP_LOGI(TAG, "Bemfa disabled after config save");
            }
        }

        /* 定期重新读取 piid=16 同步端口控制状态 */
        static int port_ctrl_poll = 0;
        if (++port_ctrl_poll >= 500) { /* ~5秒 */
            port_ctrl_poll = 0;
            BleCommand c = {CMD_GET, 16, 0, 0};
            xQueueSend(cmd_queue, &c, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
// 启动 CUKTECH Web 仪表盘 + BLE
// ============================================================
static void on_config_save(void);
static void start_web_dashboard(void) {
    DeviceConfig dev_cfg;
    config_store_load(&dev_cfg);

    http_server_set_callbacks(get_port_data_json, get_settings_json,
                               handle_port_control, handle_setting_set,
                               handle_protocol_toggle, handle_ble_control);
    http_server_start(&dev_cfg, on_config_save);
    // Bemfa 延迟启动 - 等 BLE 连上后再启动，避免内存争抢
    // bemfa_init 会在 app_task 中 BLE READY 后自动调用
}

static void start_charger_ble_if_configured(void) {
    if (ble_started) return;

    DeviceConfig dev_cfg;
    config_store_load(&dev_cfg);

    if (dev_cfg.device_mac[0] == '\0') {
        ESP_LOGI(TAG, "Charger BLE: no device configured, skipping");
        return;
    }

    ESP_LOGI(TAG, "Charger BLE: init MAC=%s, free heap=%lu bytes",
             dev_cfg.device_mac, (unsigned long)esp_get_free_heap_size());

    cmd_queue = xQueueCreate(20, sizeof(BleCommand));
    urgent_queue = xQueueCreate(4, sizeof(BleCommand));
    result_queue = xQueueCreate(32, sizeof(BleResult));
    state_mutex = xSemaphoreCreateMutex();

    ble_manager_store_setting(16, port_ctrl_val);
    ble_manager_store_setting(21, protocol_extend_val);

    // BLE 任务在核心 1，应用任务在核心 0
    xTaskCreatePinnedToCore(ble_task, "ble", 16384, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(app_task, "app", 8192, NULL, 1, NULL, 0);

    ble_started = true;
}

// ============================================================
// 电池定时器
// ============================================================
static void battery_timer_cb(void *arg) {
    (void)arg;
    int pct = battery_get_percent();
    ui_update_battery(pct);
}

static void on_long_press(void) {
    ESP_LOGW(TAG, "Long press: switching to config portal (keeping WiFi config)...");
    wifi_prov_request_portal();
}

static void on_key_pressed(void) {
    if (s_buttons_locked) {
        ESP_LOGI(TAG, "Buttons locked, ignoring key press");
        return;
    }
    if (ui_is_charger_screen()) {
        /* 充电器页短按：返回 AIDA64 */
        ui_switch_to_charger(false);
    } else {
        /* AIDA64 页短按：切换到充电器详情页 */
        ui_switch_to_charger(true);
    }
}

/* KEY 短按：循环亮度 0→25→50→75→100→0，0% 时息屏并暂停 AIDA64 */
/* 实际启停由 app_task 处理，避免在按键任务中直接操作网络 */
/* 背光关闭后延迟5秒关AIDA64，背光亮起时立即恢复 */
static void on_key_brightness(int _) {
    if (s_buttons_locked) {
        ESP_LOGI(TAG, "Buttons locked, ignoring KEY press");
        return;
    }
    (void)_;
    extern int backlight_cycle(void);
    int pct = backlight_cycle();
    extern void ui_show_brightness_toast(int percent);
    ui_show_brightness_toast(pct);
    if (pct == 0 && !s_screen_off) {
        s_screen_off = true;
        s_aida64_need_stop = true;  /* app_task 会延迟5秒后执行停止 */
        s_aida64_need_start = false;
        ESP_LOGI(TAG, "Screen off, AIDA64 will be stopped in 5s");
    } else if (pct > 0 && s_screen_off) {
        s_screen_off = false;
        s_aida64_need_start = true; /* app_task 会立即执行启动 */
        s_aida64_need_stop = false; /* 取消待执行的停止 */
        ESP_LOGI(TAG, "Screen on, AIDA64 will be resumed");
    }
}

/* 配置保存回调：设置标志位，由 app_task 处理巴法云重启 */
static void on_config_save(void) {
    s_bemfa_need_restart = true;
}

static void start_aida64_if_needed(void) {
    if (!aida64_started) {
        aida64_started = true;
        settings_t cfg = settings_load();
        strncpy(s_server_ip, cfg.aida64_server, sizeof(s_server_ip) - 1);
        s_server_ip[sizeof(s_server_ip) - 1] = '\0';
        ESP_LOGI(TAG, "Starting AIDA64 monitor: %s:%u", s_server_ip, (unsigned)cfg.aida64_port);
        aida64_monitor_start(s_server_ip, cfg.aida64_port);
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "T-Display-S3 AIDA64 + CUKTECH BLE");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    config_store_init();

    esp_lcd_panel_handle_t panel_handle = bsp_lcd_init();
    backlight_init();
    lv_port_init(panel_handle);
    boot_anim_init();
    ui_init();
    button_init();
    button_register_brightness_cb(on_key_brightness);  /* KEY 短按 -> 亮度调节 */
    button_register_longpress_cb(on_long_press);   /* BOOT 长按 -> 配网 */
    button_register_keyclick_cb(on_key_pressed);    /* BOOT 短按 -> 屏幕切换 */

    battery_init();
    ui_update_battery(battery_get_percent());

    esp_timer_handle_t bat_timer;
    esp_timer_create_args_t bat_timer_cfg = {
        .callback = battery_timer_cb,
        .name = "bat_timer"
    };
    esp_timer_create(&bat_timer_cfg, &bat_timer);
    esp_timer_start_periodic(bat_timer, 30000000);

    /* 先检查WiFi是否已配置 */
    bool wifi_configured = settings_is_configured();

    if (wifi_configured) {
        /* 有配置：显示启动动画，尝试连接WiFi */
        boot_anim_play(ui_get_main_screen(), 500, 1000);
        wifi_prov_init();
        ESP_LOGI(TAG, "WiFi configured, starting with boot animation");
    } else {
        /* 无配置：直接进入AP配网模式，不显示启动动画 */
        ESP_LOGI(TAG, "WiFi not configured, going directly to AP mode");
        wifi_prov_init();
    }

    ESP_LOGI(TAG, "All init done!");

    int64_t start_tick = esp_timer_get_time() / 1000;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wifi_prov_state_t st = wifi_prov_get_state();

        if (st == WP_STATE_CONNECTED && !aida64_started) {
            /* WiFi连接成功，显示IP地址 */
            {
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA");
                if (netif) {
                    esp_netif_ip_info_t ip_info;
                    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                        char ip_str[16];
                        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                        boot_anim_set_ip(ip_str);
                    }
                }
            }

            /* 先启动AIDA64监控 */
            start_aida64_if_needed();

            /* 检查是否配置了CUKTECH充电器，如果没有则跳过BLE */
            {
                DeviceConfig dev_cfg;
                config_store_load(&dev_cfg);

                if (dev_cfg.device_mac[0] == '\0') {
                    ESP_LOGI(TAG, "No charger configured, skipping BLE");
                    boot_anim_set_loading_text("No charger config");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    /* 启动Web仪表盘 */
                    start_web_dashboard();
                    /* 跳转到AIDA64页面 */
                    boot_anim_switch_now();
                    /* 解锁按钮 */
                    s_buttons_locked = false;
                    ESP_LOGI(TAG, "Buttons unlocked");
                    break;
                }
            }

            /* 有充电器配置：启动BLE连接 */
            boot_anim_bt_connecting();
            boot_anim_set_loading_text("Connecting BLE...");
            start_charger_ble_if_configured();

            /* 等待BLE连接完成（最多10秒），超时则跳过CUKTECH功能 */
            int64_t ble_start = esp_timer_get_time() / 1000;
            bool ble_ok = false;
            while (1) {
                vTaskDelay(pdMS_TO_TICKS(200));
                if (ble_manager_is_ready()) {
                    ESP_LOGI(TAG, "BLE ready, switching to main screen");
                    boot_anim_bt_ready();
                    ble_ok = true;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    break;
                }
                if (esp_timer_get_time() / 1000 - ble_start > 10000) {
                    ESP_LOGW(TAG, "BLE timeout, skipping CUKTECH functions");
                    break;
                }
            }

            if (!ble_ok) {
                /* BLE超时：停止BLE相关功能，但AIDA64和Web仪表盘正常 */
                ESP_LOGI(TAG, "BLE not ready, CUKTECH functions disabled");
            }

            /* 启动Web仪表盘 */
            start_web_dashboard();
            /* 跳转到AIDA64页面 */
            boot_anim_switch_now();
            /* 解锁按钮 */
            s_buttons_locked = false;
            ESP_LOGI(TAG, "Buttons unlocked");
            break;
        }

        if (st == WP_STATE_FAIL) {
            ESP_LOGW(TAG, "WiFi provisioning failed");
            if (!wifi_configured) {
                continue;
            }
            boot_anim_signal_failed();
            break;
        }

        if (st == WP_STATE_AP_RUNNING) {
            if (wifi_configured) {
                boot_anim_signal_failed();
            }
            /* 配网页面禁用按键 */
            s_buttons_locked = true;
            continue;
        }

        if (esp_timer_get_time() / 1000 - start_tick > 10000 && !aida64_started) {
            if (!wifi_configured) {
                continue;
            }
            ESP_LOGW(TAG, "WiFi timeout, switching to config portal");
            /* 配网页面禁用按键 */
            s_buttons_locked = true;
            /* 显示配网页面并重启配网服务 */
            ui_show_config_screen();
            /* 停止WiFi并重新以AP模式启动 */
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(500));
            wifi_prov_init();
            break;
        }
    }
}
