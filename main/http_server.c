#include "http_server.h"
#include "config.h"
#include "config_store.h"
#include "settings.h"
#include "display_config.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "HTTP";
static httpd_handle_t _server = NULL;
static DeviceConfig _cfg_store;
static bool _provisioning = false;  /* AP 配网模式标志 */
static http_config_cb _on_save = NULL;
static port_data_cb _port_data_cb = NULL;
static settings_cb _settings_cb = NULL;
static port_control_cb _port_ctl_cb = NULL;
static setting_set_cb _setting_set_cb = NULL;
static protocol_toggle_cb _proto_toggle_cb = NULL;
static ble_control_cb _ble_ctl_cb = NULL;

/* WiFi 扫描缓存 */
#define MAX_WIFI_LIST 15
typedef struct { char ssid[33]; int8_t rssi; int8_t authmode; } wifi_info_t;
static wifi_info_t s_wifi_list[MAX_WIFI_LIST];
static int s_wifi_count = 0;

void http_server_set_callbacks(port_data_cb ports, settings_cb settings,
                               port_control_cb port_ctl, setting_set_cb setting_set,
                               protocol_toggle_cb proto_toggle,
                               ble_control_cb ble_ctl) {
    _port_data_cb = ports;
    _settings_cb = settings;
    _port_ctl_cb = port_ctl;
    _setting_set_cb = setting_set;
    _proto_toggle_cb = proto_toggle;
    _ble_ctl_cb = ble_ctl;
}

/* ==================== WiFi 扫描 ==================== */
static void wifi_scan_sync(void) {
    wifi_scan_config_t scan_cfg = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_wifi_scan_start(&scan_cfg, true);
    wifi_ap_record_t ap_records[MAX_WIFI_LIST];
    uint16_t ap_count = MAX_WIFI_LIST;
    if (esp_wifi_scan_get_ap_records(&ap_count, ap_records) == ESP_OK) {
        s_wifi_count = (int)ap_count;
        for (int i = 0; i < s_wifi_count; i++) {
            strncpy(s_wifi_list[i].ssid, (char *)ap_records[i].ssid, 32);
            s_wifi_list[i].ssid[32] = '\0';
            s_wifi_list[i].rssi = ap_records[i].rssi;
            s_wifi_list[i].authmode = ap_records[i].authmode;
        }
    }
}

static int _get_wifi_scan_handler(httpd_req_t *req) {
    /* 缓存扫描结果，至少5秒内不重新扫描 */
    static int64_t last_scan = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_scan > 5000 || s_wifi_count == 0) {
        wifi_scan_sync();
        last_scan = now;
    }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_wifi_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", s_wifi_list[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", s_wifi_list[i].rssi);
        cJSON_AddNumberToObject(o, "authmode", s_wifi_list[i].authmode);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    cJSON_free(json); cJSON_Delete(arr);
    return 0;
}

/* ==================== Ping / 状态 ==================== */
static int _get_ping_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json); cJSON_Delete(root);
    return 0;
}

static int _get_status_handler(httpd_req_t *req) {
    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\x22ok\x22:true,\x22device_model\x22:\x22%s\x22,\x22firmware_version\x22:\x22%s\x22}",
        DEVICE_MODEL, FW_VERSION);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, len);
    return 0;
}

/* ==================== 配置 API ==================== */
static void _json_cfg(cJSON *root) {
    cJSON_AddStringToObject(root, "wifi_ssid", _cfg_store.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", _cfg_store.wifi_pass);
    cJSON_AddStringToObject(root, "device_mac", _cfg_store.device_mac);
    cJSON_AddStringToObject(root, "device_token", _cfg_store.device_token);
    cJSON_AddStringToObject(root, "device_ble_key", _cfg_store.device_ble_key);
    cJSON_AddStringToObject(root, "mqtt_broker", _cfg_store.mqtt_broker);
    cJSON_AddNumberToObject(root, "mqtt_port", _cfg_store.mqtt_port);
    cJSON_AddStringToObject(root, "mqtt_user", _cfg_store.mqtt_user);
    cJSON_AddStringToObject(root, "mqtt_pass", _cfg_store.mqtt_pass);
    cJSON_AddStringToObject(root, "mqtt_topic_prefix", _cfg_store.mqtt_topic_prefix);
    cJSON_AddBoolToObject(root, "mqtt_enable", _cfg_store.mqtt_enable);
    cJSON_AddBoolToObject(root, "bemfa_enable", _cfg_store.bemfa_enable);
    cJSON_AddStringToObject(root, "bemfa_uid", _cfg_store.bemfa_uid);
    /* AIDA64 设置 */
    settings_t s = settings_load();
    cJSON_AddStringToObject(root, "aida64_server", s.aida64_server);
    cJSON_AddNumberToObject(root, "aida64_port", s.aida64_port);
}

static int _get_config_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    _json_cfg(root);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json); cJSON_Delete(root);
    return 0;
}

static int _post_config_handler(httpd_req_t *req) {
    char buf[2048];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return -1; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return -1; }

    #define SET_STR(f, k) do { cJSON *j = cJSON_GetObjectItem(root, k); \
        if (j && cJSON_IsString(j)) strncpy(_cfg_store.f, cJSON_GetStringValue(j), sizeof(_cfg_store.f)-1); } while(0)
    SET_STR(wifi_ssid, "wifi_ssid"); SET_STR(wifi_pass, "wifi_pass");
    SET_STR(device_mac, "device_mac"); SET_STR(device_token, "device_token");
    SET_STR(device_ble_key, "device_ble_key"); SET_STR(mqtt_broker, "mqtt_broker");
    SET_STR(mqtt_user, "mqtt_user"); SET_STR(mqtt_pass, "mqtt_pass");
    SET_STR(mqtt_topic_prefix, "mqtt_topic_prefix");
    SET_STR(bemfa_uid, "bemfa_uid");

    cJSON *je = cJSON_GetObjectItem(root, "mqtt_enable");
    if (je && cJSON_IsBool(je)) _cfg_store.mqtt_enable = cJSON_IsTrue(je);
    cJSON *jb = cJSON_GetObjectItem(root, "bemfa_enable");
    if (jb && cJSON_IsBool(jb)) _cfg_store.bemfa_enable = cJSON_IsTrue(jb);
    cJSON *jp = cJSON_GetObjectItem(root, "mqtt_port");
    if (jp && cJSON_IsNumber(jp)) _cfg_store.mqtt_port = (uint16_t)cJSON_GetNumberValue(jp);
    _cfg_store.valid = (_cfg_store.wifi_ssid[0] != '\0' && _cfg_store.device_mac[0] != '\0');

    /* AIDA64 设置 */
    settings_t s = settings_load();
    cJSON *jas = cJSON_GetObjectItem(root, "aida64_server");
    if (jas && cJSON_IsString(jas)) strncpy(s.aida64_server, cJSON_GetStringValue(jas), sizeof(s.aida64_server)-1);
    cJSON *jap = cJSON_GetObjectItem(root, "aida64_port");
    if (jap && cJSON_IsNumber(jap)) s.aida64_port = (uint16_t)cJSON_GetNumberValue(jap);
    s.configured = true;
    settings_save(&s);

    ESP_LOGI(TAG, "Config saved: wifi=%s aida64=%s:%u", _cfg_store.wifi_ssid, s.aida64_server, s.aida64_port);
    config_store_save(&_cfg_store);
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "已保存，正在重启...");
    char *rjson = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rjson);
    cJSON_free(rjson); cJSON_Delete(resp);

    if (_on_save) _on_save();
    return 0;
}

/* ==================== 配网 API（AP 模式） ==================== */
static int _post_provision_handler(httpd_req_t *req) {
    char buf[2048];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return -1; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return -1; }

    /* 保存 WiFi 设置 */
    settings_t s = settings_load();
    cJSON *j;
    j = cJSON_GetObjectItem(root, "wifi_ssid");
    if (j && cJSON_IsString(j)) { strncpy(s.wifi_ssid, cJSON_GetStringValue(j), sizeof(s.wifi_ssid)-1); s.wifi_ssid[sizeof(s.wifi_ssid)-1]='\0'; }
    j = cJSON_GetObjectItem(root, "wifi_pass");
    if (j && cJSON_IsString(j)) { strncpy(s.wifi_pass, cJSON_GetStringValue(j), sizeof(s.wifi_pass)-1); s.wifi_pass[sizeof(s.wifi_pass)-1]='\0'; }
    j = cJSON_GetObjectItem(root, "aida64_server");
    if (j && cJSON_IsString(j)) { strncpy(s.aida64_server, cJSON_GetStringValue(j), sizeof(s.aida64_server)-1); s.aida64_server[sizeof(s.aida64_server)-1]='\0'; }
    j = cJSON_GetObjectItem(root, "aida64_port");
    if (j && cJSON_IsNumber(j)) s.aida64_port = (uint16_t)cJSON_GetNumberValue(j);
    s.configured = true;
    settings_save(&s);

    /* 保存设备配置 */
    j = cJSON_GetObjectItem(root, "device_mac");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.device_mac, cJSON_GetStringValue(j), sizeof(_cfg_store.device_mac)-1);
    j = cJSON_GetObjectItem(root, "device_token");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.device_token, cJSON_GetStringValue(j), sizeof(_cfg_store.device_token)-1);
    j = cJSON_GetObjectItem(root, "device_ble_key");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.device_ble_key, cJSON_GetStringValue(j), sizeof(_cfg_store.device_ble_key)-1);
    /* MQTT 设置 */
    j = cJSON_GetObjectItem(root, "mqtt_broker");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.mqtt_broker, cJSON_GetStringValue(j), sizeof(_cfg_store.mqtt_broker)-1);
    j = cJSON_GetObjectItem(root, "mqtt_user");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.mqtt_user, cJSON_GetStringValue(j), sizeof(_cfg_store.mqtt_user)-1);
    j = cJSON_GetObjectItem(root, "mqtt_pass");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.mqtt_pass, cJSON_GetStringValue(j), sizeof(_cfg_store.mqtt_pass)-1);
    j = cJSON_GetObjectItem(root, "mqtt_topic_prefix");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.mqtt_topic_prefix, cJSON_GetStringValue(j), sizeof(_cfg_store.mqtt_topic_prefix)-1);
    j = cJSON_GetObjectItem(root, "mqtt_enable");
    if (j && cJSON_IsBool(j)) _cfg_store.mqtt_enable = cJSON_IsTrue(j);
    j = cJSON_GetObjectItem(root, "mqtt_port");
    if (j && cJSON_IsNumber(j)) _cfg_store.mqtt_port = (uint16_t)cJSON_GetNumberValue(j);
    /* 巴法云设置 */
    j = cJSON_GetObjectItem(root, "bemfa_uid");
    if (j && cJSON_IsString(j)) strncpy(_cfg_store.bemfa_uid, cJSON_GetStringValue(j), sizeof(_cfg_store.bemfa_uid)-1);
    j = cJSON_GetObjectItem(root, "bemfa_enable");
    if (j && cJSON_IsBool(j)) _cfg_store.bemfa_enable = cJSON_IsTrue(j);
    _cfg_store.valid = (_cfg_store.device_mac[0] != '\0');
    config_store_save(&_cfg_store);

    /* 保存显示设置 */
    display_config_t dcfg = display_config_load();
    char fld[32];
    (void)fld;
    for (int r = 0; r < 3; r++) {
        char name[16];
        snprintf(name, sizeof(name), "siv%d0", r);
        j = cJSON_GetObjectItem(root, name);
        if (j && cJSON_IsNumber(j)) dcfg.rows[r].siv[0] = (int)cJSON_GetNumberValue(j);
        snprintf(name, sizeof(name), "siv%d1", r);
        j = cJSON_GetObjectItem(root, name);
        if (j && cJSON_IsNumber(j)) dcfg.rows[r].siv[1] = (int)cJSON_GetNumberValue(j);
    }
    j = cJSON_GetObjectItem(root, "mem_siv_pct");
    if (j && cJSON_IsNumber(j)) dcfg.mem_siv_pct = (int)cJSON_GetNumberValue(j);
    j = cJSON_GetObjectItem(root, "mem_siv_used");
    if (j && cJSON_IsNumber(j)) dcfg.mem_siv_used = (int)cJSON_GetNumberValue(j);
    display_config_save(&dcfg);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Provision saved: ssid=%s aida64=%s:%u", s.wifi_ssid, s.aida64_server, s.aida64_port);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "配置已保存，正在重启...");
    char *rjson = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rjson);
    cJSON_free(rjson); cJSON_Delete(resp);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return 0;
}

/* ==================== 仪表盘 API ==================== */
static int _get_ports_handler(httpd_req_t *req) {
    if (!_port_data_cb) { httpd_resp_send_500(req); return -1; }
    cJSON *root = _port_data_cb();
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    cJSON_free(json); cJSON_Delete(root);
    return 0;
}

static int _get_settings_handler(httpd_req_t *req) {
    if (!_settings_cb) { httpd_resp_send_500(req); return -1; }
    cJSON *root = _settings_cb();
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json); cJSON_Delete(root);
    return 0;
}

static int _post_port_handler(httpd_req_t *req) {
    char buf[256]; int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return -1; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return -1; }
    cJSON *jp = cJSON_GetObjectItem(root, "port");
    cJSON *ja = cJSON_GetObjectItem(root, "action");
    bool ok = false;
    if (jp && ja && cJSON_IsString(jp) && cJSON_IsString(ja) && _port_ctl_cb)
        ok = _port_ctl_cb(cJSON_GetStringValue(jp), cJSON_GetStringValue(ja));
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    char *rjson = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rjson);
    cJSON_free(rjson); cJSON_Delete(resp);
    return 0;
}

static int _post_setting_handler(httpd_req_t *req) {
    char buf[256]; int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return -1; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return -1; }
    cJSON *jp = cJSON_GetObjectItem(root, "piid");
    cJSON *jv = cJSON_GetObjectItem(root, "value");
    bool ok = false;
    if (jp && jv && cJSON_IsNumber(jp) && cJSON_IsNumber(jv) && _setting_set_cb)
        ok = _setting_set_cb((int)cJSON_GetNumberValue(jp), (int)cJSON_GetNumberValue(jv));
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    char *rjson = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rjson);
    cJSON_free(rjson); cJSON_Delete(resp);
    return 0;
}

static int _post_ble_handler(httpd_req_t *req) {
    char buf[128]; int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return -1; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return -1; }
    cJSON *je = cJSON_GetObjectItem(root, "enabled");
    bool ok = false;
    if (je && cJSON_IsBool(je) && _ble_ctl_cb) ok = _ble_ctl_cb(cJSON_IsTrue(je));
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    char *rjson = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rjson);
    cJSON_free(rjson); cJSON_Delete(resp);
    return 0;
}

static int _post_protocol_handler(httpd_req_t *req) {
    char buf[256]; int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return -1; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return -1; }
    cJSON *jp = cJSON_GetObjectItem(root, "port");
    cJSON *jproto = cJSON_GetObjectItem(root, "protocol");
    cJSON *ja = cJSON_GetObjectItem(root, "action");
    bool ok = false;
    if (jp && jproto && ja && cJSON_IsString(jp) && cJSON_IsString(jproto) && cJSON_IsString(ja) && _proto_toggle_cb)
        ok = _proto_toggle_cb(cJSON_GetStringValue(jp), cJSON_GetStringValue(jproto),
                              strcmp(cJSON_GetStringValue(ja), "on") == 0);
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    char *rjson = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rjson);
    cJSON_free(rjson); cJSON_Delete(resp);
    return 0;
}

static const char *SCR_LABELS[] = {"5分钟","1分钟","10分钟","30分钟","常亮"};
static int _get_sleep_handler(httpd_req_t *req) {
    if (!_settings_cb) { httpd_resp_send_500(req); return -1; }
    cJSON *root = _settings_cb();
    int val = 0;
    cJSON *j6 = cJSON_GetObjectItem(root, "6");
    if (j6 && cJSON_IsNumber(j6)) val = (int)cJSON_GetNumberValue(j6);
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "value", val);
    cJSON_AddStringToObject(resp, "label", (val >= 0 && val < 5) ? SCR_LABELS[val] : "?");
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json); cJSON_Delete(resp);
    return 0;
}

static int _post_sleep_handler(httpd_req_t *req) {
    char buf[128]; int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_500(req); return -1; }
    buf[len] = '\0';
    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return -1; }
    cJSON *jv = cJSON_GetObjectItem(root, "value");
    bool ok = false;
    if (jv && cJSON_IsNumber(jv) && _setting_set_cb) {
        int val = (int)cJSON_GetNumberValue(jv);
        if (val >= 0 && val < 5) ok = _setting_set_cb(6, val);
    }
    cJSON_Delete(root);
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    char *rjson = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, rjson);
    cJSON_free(rjson); cJSON_Delete(resp);
    return 0;
}

/* ==================== 强制门户 ==================== */
static int _captive_redirect_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_sendstr(req, "");
    return 0;
}

/* ==================== DNS 劫持（仅 AP 模式） ==================== */
static void dns_hijack_task(void *param) {
    (void)param;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(53),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "DNS hijack started on port 53");
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in src; socklen_t src_len = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (n >= 12) {
            buf[2] |= 0x80; buf[3] |= 0x80; buf[7] = 1;
            int pos = 12;
            while (pos < n && buf[pos] != 0) pos += buf[pos] + 1;
            pos += 5;
            if (pos + 16 <= (int)sizeof(buf)) {
                uint8_t answer[] = {0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0x00,0x00,0x00,0x3C, 0x00,0x04, 192,168,4,1};
                memcpy(buf + pos, answer, 16);
                n = pos + 16;
                sendto(sock, buf, n, 0, (struct sockaddr *)&src, src_len);
            }
        }
    }
}

/* ==================== 仪表盘 HTML ==================== */
static const char _DASH[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>CUKTECH 10 Ultra</title><style>"
":root{--bg:#121215;--card:#000;--card-b:rgba(137,246,243,0.5);--text:rgba(255,255,255,0.9);--dim:rgba(255,255,255,0.4);--sub:rgba(255,255,255,0.6);--c1:#46B4FF;--c2:#FF7A00;--c3:#89D8F3;--a:#FFD24B}"
"body.light{--bg:#f0f0f5;--card:#fff;--card-b:rgba(0,0,0,0.1);--text:#1a1a1a;--dim:#888;--sub:#666}"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Noto Sans SC','MiSans',sans-serif;background:#0a0a0f;color:var(--text);display:flex;justify-content:center;min-height:100vh;overflow-x:hidden}"
".phone{width:100%;max-width:430px;min-height:100vh;background:var(--bg);position:relative}"
".nav{position:fixed;top:0;left:0;right:0;height:48px;max-width:430px;margin:0 auto;display:flex;align-items:center;justify-content:space-between;padding:0 16px;background:var(--bg);z-index:100;border-radius:0 0 18px 18px}"
".nt{font-size:16px;font-weight:500;color:var(--text)}"
".th{width:28px;height:28px;font-size:18px;line-height:28px;text-align:center;cursor:pointer}"
".top{padding:48px 0 20px;background:linear-gradient(180deg,var(--bg),var(--bg));text-align:center;position:sticky;top:0;z-index:1}"
".tpl{font-size:13px;color:var(--dim)}"
".tpv{font-size:56px;font-weight:700;line-height:1.1}"
".tpu{font-size:16px;color:var(--sub);margin-left:2px}"
".pr{display:flex;justify-content:center;gap:6px;margin:16px 12px 0;flex-wrap:wrap}"
".pb{background:var(--card);border:1px solid var(--card-b);border-radius:14px;padding:14px 8px;min-width:76px;flex:1;text-align:center;transition:.3s}"
".pn{font-size:12px;color:var(--dim);font-weight:500;margin-bottom:6px}"
".pw{font-size:24px;font-weight:700;color:var(--text)}"
".pw .w{font-size:12px;color:var(--dim);font-weight:400}"
".pvi{font-size:11px;color:var(--sub);margin-top:4px}"
".pp{font-size:10px;margin-top:6px;padding:2px 8px;border-radius:8px;display:inline-block}"
".sec{font-size:13px;font-weight:600;padding:16px 20px 8px;color:var(--dim)}"
".card{background:var(--card);border:1px solid var(--card-b);border-radius:14px;margin:6px 16px;padding:14px 16px}"
".row{display:flex;align-items:center;justify-content:space-between;padding:10px 0}"
".row+.row{border-top:1px solid var(--card-b)}"
".rl{font-size:14px;color:var(--text)}"
".rv{font-size:14px;color:var(--dim)}"
".tg{position:relative;width:44px;height:24px;cursor:pointer}"
".tg input{display:none}"
".tg .sl{position:absolute;inset:0;background:#333;border-radius:12px;transition:.3s}"
".tg .sl::after{content:'';position:absolute;width:20px;height:20px;background:#888;border-radius:50%;top:2px;left:2px;transition:.3s}"
".tg input:checked+.sl{background:#4CAF50}"
".tg input:checked+.sl::after{left:22px;background:#fff}"
".sg{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:8px}"
".scb{background:var(--card);border:2px solid var(--card-b);border-radius:12px;padding:10px 4px;text-align:center;cursor:pointer;transition:.2s}"
".scb.active{border-color:#4CAF50}"
".scb .si{font-size:22px}"
".scb .sn{font-size:11px;color:var(--dim);margin-top:4px}"
".scb.active .sn{color:#4CAF50}"
".pg{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:6px}"
".pcb{background:var(--card);border:1px solid var(--card-b);border-radius:6px;padding:3px 7px;text-align:center;cursor:pointer;transition:.2s;font-size:11px;color:var(--dim)}"
".pcb.on{border-color:#4CAF50;color:#81C784}"
".ph{font-size:12px;color:var(--dim);font-weight:600;margin:8px 0 4px}"
".ob{width:100%;padding:14px;background:var(--card);border:1px solid var(--card-b);border-radius:12px;color:var(--dim);font-size:14px;cursor:pointer;text-align:center}"
".ob:active{opacity:.7}"
".os{font-size:12px;color:var(--dim);margin-top:8px;text-align:center}"
"a{color:var(--c1);text-decoration:none}"
"</style></head><body>"
"<div class='phone'>"
"<div class='nav'><div class='nt'>CUKTECH 10 Ultra</div><div class='th' id='thBtn' onclick='toggleTheme()'></div></div>"
"<div class='top'>"
"<div class='tpl'>Total Power</div>"
"<div class='tpv' id='tp'>0<span class='tpu'>W</span></div>"
"<div class='pr' id='ports'></div>"
"</div>"
"<div class='sec'>BLE 连接</div>"
"""<div class='card'>"
"<div class='row'><span class='rl'>连接状态</span><span class='rv' id='bleSt'>--</span></div>"
"<div class='row'><span class='rl'>BLE 控制</span><label class='tg'><input type='checkbox' id='bleEn' onchange='toggleBle(this.checked)'><span class='sl'></span></label></div>"
"</div>"
"<div class='sec'>端口控制</div>"
"<div class='card' id='portctl'></div>"
"<div class='sec'>倒计时</div>"
"<div class='card' id='cd'></div>"
"<div class='sec'>设置</div>"
"<div class='card'>"
"<div class='row'><span class='rl'>场景模式</span><span class='rv' id='v5'>--</span></div>"
"<div class='row'><span class='rl'>息屏时间</span><select id='v6' onchange='setSleep(this.value)' style='background:var(--card);border:1px solid var(--card-b);border-radius:8px;padding:4px 8px;font-size:13px;color:var(--text);-webkit-appearance:auto'><option value='0'>5分钟</option><option value='1'>1分钟</option><option value='2'>10分钟</option><option value='3'>30分钟</option><option value='4'>常亮</option></select></div>"
"<div class='row'><span class='rl'>语言</span><span class='rv' id='v13'>--</span></div>"
"<div class='row'><span class='rl'>USB-A小电流</span><label class='tg'><input type='checkbox' id='s15' onchange='setS(15,this.checked?1:0)'><span class='sl'></span></label></div>"
"<div class='row'><span class='rl'>空闲息屏</span><label class='tg'><input type='checkbox' id='s19' onchange='setS(19,this.checked?1:0)'><span class='sl'></span></label></div>"
"<div class='row'><span class='rl'>屏幕方向锁</span><label class='tg'><input type='checkbox' id='s20' onchange='setS(20,this.checked?1:0)'><span class='sl'></span></label></div>"
"</div>"
"<div class='sec'>场景模式</div>"
"<div class='card'><div class='sg' id='scenes'></div>"
"<div style='margin-top:10px;font-size:12px;color:var(--dim);text-align:center;min-height:18px' id='sceneDesc'>--</div></div>"

"<div style='text-align:center;padding:16px;font-size:12px;color:var(--dim)'>"
"<a href='/config'>高级配置</a></div>"
"</div>"
"<script>"
"var PN=['C1','C2','C3','USB-A'],PM={};"
"var SCENES=[{v:1,n:'AI模式',i:'\\u{1F916}',d:'自动识别设备智能匹配最优充电功率'},{v:2,n:'数码生态',i:'\\u{1F4BB}',d:'多口同时充电均衡分配功率'},{v:3,n:'单口模式',i:'\\u{1F50C}',d:'单口最大功率输出优先C1口'},{v:4,n:'均衡模式',i:'\\u{2696}\\uFE0F',d:'多个端口均衡分配充电功率'}];"
"var SN={1:'AI模式',2:'数码生态',3:'单口模式',4:'均衡模式'};"
"var SCR={0:'5分钟',1:'1分钟',2:'10分钟',3:'30分钟',4:'常亮'};"
"var LNG={0:'English',1:'中文'};"
"var PM2={};"
"var CDPI=[9,10,11,12];"
"var PMAP=[{p:'c1',n:'C1',ps:[{n:'PD',b:0},{n:'PPS',b:1},{n:'UFCS',b:2}]},{p:'c2',n:'C2',ps:[{n:'PD',b:8},{n:'PPS',b:9},{n:'UFCS',b:10}]},{p:'c3',n:'C3',ps:[{n:'UFCS',b:16},{n:'SCP',b:17}]},{p:'a',n:'USB-A',ps:[{n:'UFCS',b:24},{n:'SCP',b:25}]}];"
"function init(){var h='';PMAP.forEach(function(pg){h+='<div style=\"margin-bottom:10px\">';h+='<div class=\"row\" style=\"padding:4px 0\"><span class=\"rl\">'+pg.n+'</span><div style=\"display:flex;align-items:center;gap:6px\">';pg.ps.forEach(function(pr){h+='<div class=\"pcb\" id=\"pb_'+pg.p+'_'+pr.n.toLowerCase()+'\" onclick=\"setProto(\\''+pg.p+'\\',\\''+pr.n.toLowerCase()+'\\')\">'+pr.n+'</div>';});h+='<label class=\"tg\"><input type=\"checkbox\" id=\"pc_'+pg.p+'\" onchange=\"setPort(\\''+pg.p+'\\',this.checked?\\'on\\':\\'off\\')\"><span class=\"sl\"></span></label>';h+='</div></div></div>';});document.getElementById('portctl').innerHTML=h;h='';SCENES.forEach(function(s){h+='<div class=\"scb\" id=\"sc'+s.v+'\" onclick=\"setS(5,'+s.v+')\"><div class=\"si\">'+s.i+'</div><div class=\"sn\">'+s.n+'</div></div>';});document.getElementById('scenes').innerHTML=h;h='';var CDPN=['C1','C2','C3','USB-A'];for(var i=0;i<4;i++){h+='<div class=\"row\"><span class=\"rl\">'+CDPN[i]+'<span class=\"rv\" id=\"cds'+CDPI[i]+'\" style=\"font-size:11px;color:var(--dim);margin-left:6px\"></span></span><div style=\"display:flex;align-items:center;gap:4px;flex-wrap:wrap\"><span class=\"rv\" id=\"cdv'+CDPI[i]+'\" style=\"min-width:52px;text-align:right\">--</span><button onclick=\"setCd('+CDPI[i]+',30)\" style=\"font-size:11px;padding:2px 5px;border:1px solid var(--card-b);border-radius:4px;background:var(--card);color:var(--text);cursor:pointer\">30</button><button onclick=\"setCd('+CDPI[i]+',60)\" style=\"font-size:11px;padding:2px 5px;border:1px solid var(--card-b);border-radius:4px;background:var(--card);color:var(--text);cursor:pointer\">60</button><input id=\"cdi'+CDPI[i]+'\" type=\"number\" min=\"0\" max=\"1440\" placeholder=\"分\" style=\"width:44px;font-size:11px;padding:2px 4px;background:var(--card);border:1px solid var(--card-b);border-radius:4px;color:var(--text);text-align:center\"><button onclick=\"setCdInput('+CDPI[i]+')\" style=\"font-size:11px;padding:2px 5px;border:1px solid var(--card-b);border-radius:4px;background:var(--card);color:var(--text);cursor:pointer\">设置</button><button onclick=\"setCd('+CDPI[i]+',0)\" style=\"background:none;border:none;color:var(--dim);cursor:pointer;font-size:13px\">✕</button></div></div>';}document.getElementById('cd').innerHTML=h;}"
"init();upd();setInterval(upd,10000);"
"function fmtCd(s){if(!s||s<=0)return'--';if(s<60)return'<span style=\"color:#FF6B6B\">'+s+'秒</span>';var m=Math.floor(s/60);var r=s%60;return m+'分'+(r>0?r+'秒':'');}"
"function updCd(){fetch('/api/settings').then(function(r){return r.json()}).then(function(d){if(!d.cd_remains)return;var pm16=d['16']||0;for(var i=0;i<4;i++){var el=document.getElementById('cdv'+CDPI[i]);if(el)el.innerHTML=fmtCd(d.cd_remains[i]?d.cd_remains[i].remain:0);var es=document.getElementById('cds'+CDPI[i]);if(es){var on=!!(pm16&(1<<i));var act=d.cd_remains[i]?d.cd_remains[i].action:0;es.textContent=on?'已开':'已关';if(act==1)es.textContent+='·将开';else if(act==-1)es.textContent+='·将关';}}}).catch(function(){});}"
"setInterval(updCd,1000);updCd();"
"function upd(){var t0=Date.now();fetch('/api/ports').then(function(r){return r.json()}).then(function(d){var h='',tp=0;var PC=['var(--c1)','var(--c2)','var(--c3)','var(--a)'];var merged=d[2]&&d[2].status_raw==17;d.forEach(function(p,i){if(i==3&&merged)return;var nm=PN[i];if(i==2&&merged)nm='C3 & A';tp+=p.power;h+='<div class=\"pb\" style=\"background:'+(i==2&&merged?'#0a0a0f':'')+';border:'+(i==2&&merged?'2px solid transparent':'')+';border-image:'+(i==2&&merged?'linear-gradient(135deg,#46B4FF,#FF7A00) 1':'')+';border-radius:8px\"><div class=\"pn\">'+nm+'</div><div class=\"pw\" style=\"color:'+PC[i]+'\">'+p.power.toFixed(1)+'<span class=\"w\">W</span></div><div class=\"pvi\">'+p.voltage.toFixed(1)+'V  '+p.current.toFixed(1)+'A</div><div class=\"pp\" style=\"background:'+(p.active?'rgba(76,175,80,0.2)':'rgba(255,255,255,0.05)')+';color:'+(p.active?'#81C784':'var(--dim)')+'\">'+p.protocol+'</div></div>';});document.getElementById('ports').innerHTML=h;document.getElementById('tp').textContent=tp.toFixed(1);}).catch(function(){});fetch('/api/settings').then(function(r){return r.json()}).then(function(d){PM=d;['c1','c2','c3','a'].forEach(function(p,i){var el=document.getElementById('pc_'+p);if(el)el.checked=!!(d['16']&(1<<(i==3?3:i)));});document.getElementById('v5').textContent=SN[d['5']]||'--';document.getElementById('v6').value=d['6']||0;document.getElementById('v13').textContent=LNG[d['13']]||'--';document.getElementById('s15').checked=!!d['15'];document.getElementById('s19').checked=!!d['19'];document.getElementById('s20').checked=!!d['20'];document.getElementById('bleEn').checked=!!d['ble_enabled'];SCENES.forEach(function(s){var el=document.getElementById('sc'+s.v);if(el)el.className='scb'+(d['5']==s.v?' active':'');});var sd=document.getElementById('sceneDesc');if(sd){var sv=d['5'];SCENES.forEach(function(s){if(s.v===sv)sd.textContent=s.d;});}var v21=d['21']||0;PMAP.forEach(function(pg){pg.ps.forEach(function(pr){var el=document.getElementById('pb_'+pg.p+'_'+pr.n.toLowerCase());if(!el)return;el.className='pcb'+((v21&(1<<pr.b))?' on':'');var pdOff=(pg.p==='c1'||pg.p==='c2')&&!(v21&(1<<(pg.p==='c1'?0:8)));var dis=pr.n.toLowerCase()==='pps'&&pdOff;el.style.opacity=dis?'0.3':'1';el.style.pointerEvents=dis?'none':'auto';});});}).catch(function(){});}"
"function setPort(p,a){fetch('/api/port',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({port:p,action:a})}).then(function(){setTimeout(upd,500);});}"
"function setProto(p,proto){var v=PM['21']||0;var b=0;PMAP.forEach(function(pg){if(pg.p===p)pg.ps.forEach(function(pr){if(pr.n.toLowerCase()===proto)b=pr.b;});});var on=!(v&(1<<b));fetch('/api/protocol',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({port:p,protocol:proto,action:on?'on':'off'})}).then(function(){setTimeout(upd,500);});}"
"function setS(piid,v){fetch('/api/setting',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({piid:piid,value:v})}).then(function(){setTimeout(upd,500);});}"
"function setSleep(v){fetch('/api/sleep',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({value:parseInt(v)})}).then(function(){setTimeout(upd,500);});}"
"function setCd(piid,v){var val=parseInt(v);if(isNaN(val)||val<0)val=0;if(val>1440)val=1440;setS(piid,val);}"
"function setCdInput(piid){var v=document.getElementById('cdi'+piid).value;setCd(piid,v);}"
"function toggleTheme(){var b=document.body;var isLight=b.classList.contains('light');b.classList.toggle('light');localStorage.setItem('theme',isLight?'dark':'light');document.getElementById('thBtn').textContent=isLight?'\\u2600\\uFE0F':'\\u{1F319}';}"
"(function(){var t=localStorage.getItem('theme');if(t==='light'){document.body.classList.add('light');document.getElementById('thBtn').textContent='\\u{1F319}';}else{document.getElementById('thBtn').textContent='\\u2600\\uFE0F';}})();"
"function updBle(){fetch('/api/settings').then(function(r){return r.json()}).then(function(d){var st=d['ble_enabled'];document.getElementById('bleEn').checked=!!st;document.getElementById('bleSt').textContent=st?'已启用':'已禁用';document.getElementById('bleSt').style.color=st?'#81C784':'#888';}).catch(function(){});}setInterval(updBle,10000);updBle();"
"function toggleBle(on){var st=document.getElementById('bleSt');st.textContent='切换中...';fetch('/api/ble',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:on})}).then(function(){setTimeout(updBle,2000);});}"
"</script></body></html>";

static int _get_dash_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=300");
    httpd_resp_send_chunk(req, _DASH, sizeof(_DASH) - 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return 0;
}

/* ==================== 配置 / 配网页面 ==================== */
static const char _CFG_HTML[] =
"<!DOCTYPE html><html lang='zh'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>设备配置</title><style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;background:linear-gradient(135deg,#0f172a 0%,#1e293b 100%);color:#e2e8f0;min-height:100vh;padding:16px}"
".wrap{max-width:440px;margin:0 auto}"
"h1{text-align:center;font-size:22px;font-weight:700;background:linear-gradient(90deg,#10b981,#06b6d4);-webkit-background-clip:text;-webkit-text-fill-color:transparent;margin:12px 0 4px}"
".sub{text-align:center;font-size:12px;color:#64748b;margin-bottom:16px}"
".card{background:rgba(30,41,59,0.8);border:1px solid rgba(148,163,184,0.1);border-radius:14px;padding:18px;margin-bottom:14px}"
".sec{font-size:14px;font-weight:600;color:#94a3b8;margin-bottom:10px;padding-bottom:6px;border-bottom:1px solid rgba(148,163,184,0.1)}"
"label{display:block;font-size:12px;color:#94a3b8;margin:8px 0 4px;font-weight:500}"
"input,select{width:100%;height:38px;background:rgba(15,23,42,0.6);border:1.5px solid rgba(148,163,184,0.15);border-radius:8px;padding:0 10px;color:#e2e8f0;font-size:14px;outline:none;transition:border-color .2s}"
"input:focus,select:focus{border-color:#10b981}"
".row2{display:flex;gap:8px}.row2 input:first-child{flex:3}.row2 input:last-child{flex:1}"
".wl{max-height:220px;overflow-y:auto;border-radius:8px;margin:6px 0}"
".wi{display:flex;align-items:center;padding:10px 12px;border-radius:8px;cursor:pointer;border:1.5px solid transparent;margin-bottom:4px;background:rgba(15,23,42,0.6);transition:.2s}"
".wi:hover{background:rgba(30,41,59,0.9);border-color:rgba(16,185,129,0.3)}"
".wi.sel{border-color:#10b981;background:rgba(16,185,129,0.1)}"
".wn{flex:1;font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
".wr{font-size:11px;color:#64748b}"
".btn{width:100%;padding:13px;border:none;border-radius:10px;font-size:15px;font-weight:700;cursor:pointer;transition:.2s;margin-top:8px}"
".btn-scan{background:rgba(30,41,59,0.9);color:#e2e8f0;border:1px solid rgba(148,163,184,0.2)}"
".btn-save{background:linear-gradient(135deg,#10b981,#06b6d4);color:#fff;box-shadow:0 4px 16px rgba(16,185,129,0.3)}"
".btn:active{transform:translateY(1px)}"
".btn:disabled{opacity:.5;cursor:not-allowed}"
".tog{display:flex;align-items:center;gap:8px;margin:6px 0}"
".tog input{display:none;width:auto;height:auto}"
".tog .sl{width:44px;height:24px;background:#333;border-radius:12px;cursor:pointer;position:relative;transition:.3s;flex-shrink:0}"
".tog .sl::after{content:'';width:20px;height:20px;background:#888;border-radius:50%;position:absolute;top:2px;left:2px;transition:.3s}"
".tog input:checked+.sl{background:#4CAF50}.tog input:checked+.sl::after{left:22px;background:#fff}"
"#msg{text-align:center;padding:10px;border-radius:8px;font-size:13px;margin-top:8px;display:none}"
".ok{background:rgba(16,185,129,0.1);color:#10b981;display:block!important}"
".grid2{display:grid;grid-template-columns:1fr 1fr;gap:6px}"
".siv-row{display:flex;gap:6px;align-items:center;margin-bottom:4px}"
".siv-row .lb{width:50px;font-size:11px;color:#64748b;text-align:right;flex-shrink:0}"
".siv-row select{flex:1;height:30px;font-size:11px;padding:0 4px}"
"a{color:#06b6d4;text-decoration:none}"
"</style></head><body><div class='wrap'>"
"<h1>\u26A1 设备配置</h1>"
"<p class='sub'>WiFi / AIDA64 / CUKTECH / 显示设置</p>"

"<div class='card'>"
"<div class='sec'>WiFi 网络</div>"
"<div id='wl' class='wl'><div style='color:#64748b;text-align:center;padding:12px;font-size:13px'>点击下方扫描</div></div>"
"<button class='btn btn-scan' id='bscan' onclick='scanWifi()'>\u{1F50D} 扫描 WiFi</button>"
"<label>WiFi 名称 (SSID)</label>"
"<input id='wifi_ssid' placeholder='选择或输入' maxlength='32'>"
"<label>WiFi 密码</label>"
"<input id='wifi_pass' type='password' placeholder='无密码留空' maxlength='64'>"
"</div>"

"<div class='card'>"
"<div class='sec'>AIDA64 显示</div>"
"<label>RemoteSensor IP 地址</label>"
"<input id='aida64_server' placeholder='192.168.1.121' maxlength='64'>"
"<label>端口</label>"
"<input id='aida64_port' type='number' value='7789' min='1' max='65535'>"
"</div>"

"<div class='card'>"
"<div class='sec'>CUKTECH 充电器</div>"
"<label>设备 MAC</label>"
"<input id='device_mac' placeholder='3C:CD:73:30:CD:A1' maxlength='31'>"
"<label>Device Token</label>"
"<input id='device_token' maxlength='63'>"
"<label>BLE Key</label>"
"<input id='device_ble_key' maxlength='63'>"
"</div>"

"<div class='card'>"
"<div class='sec'>MQTT 服务器</div>"
"<div class='tog'><input type='checkbox' id='mqtt_enable' checked><label class='sl' for='mqtt_enable'></label><span>启用 MQTT</span></div>"
"<label>Broker 地址</label>"
"<input id='mqtt_broker' placeholder='broker.example.com'>"
"<div class='row2'><div><label>端口</label><input id='mqtt_port' type='number' value='1883'></div><div><label>用户名</label><input id='mqtt_user'></div></div>"
"<label>密码</label>"
"<input id='mqtt_pass' type='password'>"
"<label>Topic 前缀</label>"
"<input id='mqtt_topic_prefix' value='cuktech/charger'>"
"</div>"

"<div class='card'>"
"<div class='sec'>巴法云（小爱/小度）</div>"
"<div class='tog'><input type='checkbox' id='bemfa_enable'><label class='sl' for='bemfa_enable'></label><span>启用巴法云</span></div>"
"<label>私钥 (UID)</label>"
"<input id='bemfa_uid' placeholder='注册巴法云获取'>"
"<div class='card'>"
"<div class='sec'>高级显示设置 (SIV)</div>"
"<div id='siv-rows'></div>"
"<label>内存使用率 SIV</label>"
"<div class='siv-row'><select id='mem_siv_pct'></select></div>"
"<label>内存已用 SIV</label>"
"<div class='siv-row'><select id='mem_siv_used'></select></div>"
"</div>"

"<button class='btn btn-save' onclick='saveAll()'>\u2705 保存并重启</button>"
"<div id='msg'></div>"
"<div style='text-align:center;margin:12px'><a href='/'>\u2190 返回仪表盘</a></div>"

"</div>"
"<script>"
"function $(i){return document.getElementById(i)}"
"function mkSivSel(v){var s=document.createElement('select');for(var i=0;i<32;i++){var o=new Option('SIV'+i,i);if(i===v)o.selected=true;s.add(o)}return s}"
"function initSiv(){var labels=['使用率','温度','功率'];var el=$('siv-rows');for(var r=0;r<3;r++){var d=document.createElement('div');d.className='siv-row';var lb=document.createElement('div');lb.className='lb';lb.textContent=labels[r];var s1=mkSivSel(0),s2=mkSivSel(0);s1.id='siv'+r+'0';s2.id='siv'+r+'1';d.appendChild(lb);d.appendChild(s1);d.appendChild(s2);el.appendChild(d)}}"
"initSiv();"
"function fillSiv(v,r,c){var el=$('siv'+r+c);if(el)el.value=v}"
"for(var i=0;i<32;i++){$('mem_siv_pct').add(new Option('SIV'+i,i));$('mem_siv_used').add(new Option('SIV'+i,i))}"

"fetch('/api/config').then(function(r){return r.json()}).then(function(d){"
"['wifi_ssid','wifi_pass','device_mac','device_token','device_ble_key','mqtt_broker','mqtt_user','mqtt_pass','mqtt_topic_prefix','bemfa_uid','aida64_server'].forEach(function(k){var e=$(k);if(e&&d[k]!==undefined)e.value=d[k]});"
"if(d.aida64_port)$('aida64_port').value=d.aida64_port;"
"if(d.mqtt_port)$('mqtt_port').value=d.mqtt_port;"
"$('mqtt_enable').checked=!!d.mqtt_enable;$('bemfa_enable').checked=!!d.bemfa_enable;"
"}).catch(function(){});"

"fetch('/api/display').then(function(r){return r.json()}).then(function(d){"
"for(var r=0;r<3;r++){fillSiv(d['siv'+r+'0']||0,r,0);fillSiv(d['siv'+r+'1']||0,r,1)}"
"if(d.mem_siv_pct)$('mem_siv_pct').value=d.mem_siv_pct;"
"if(d.mem_siv_used)$('mem_siv_used').value=d.mem_siv_used;"
"}).catch(function(){});"

"function scanWifi(){var b=$('bscan');b.textContent='\u23F3 扫描中...';b.disabled=true;"
"$('wl').innerHTML='<div style=\"text-align:center;padding:16px;color:#64748b\">\u626B\u63CF\u4E2D...</div>';"
"fetch('/api/wifi_scan').then(function(r){return r.json()}).then(function(d){"
"if(!d||!d.length){$('wl').innerHTML='<div style=\"text-align:center;padding:12px;color:#64748b\">\u672A\u627E\u5230</div>';b.textContent='\u{1F50D} \u91CD\u65B0\u626B\u63CF';b.disabled=false;return;}"
"d.sort(function(a,b){return b.rssi-a.rssi});"
"var h=d.map(function(w){var sig=w.rssi>=-55?'4':w.rssi>=-67?'3':w.rssi>=-78?'2':'1';var lock=w.authmode>0?'\u{1F512} ':'';"
"return '<div class=\"wi\" data-ssid=\"'+w.ssid.replace(/\"/g,'&quot;')+'\"><span class=\"wn\">'+lock+w.ssid+'</span><span class=\"wr\">'+w.rssi+'dBm</span></div>';}).join('');"
"$('wl').innerHTML=h;"
"document.querySelectorAll('.wi').forEach(function(it){it.onclick=function(){$('wifi_ssid').value=this.dataset.ssid;document.querySelectorAll('.wi').forEach(function(i){i.classList.remove('sel')});this.classList.add('sel');$('wifi_pass').focus()}});"
"b.textContent='\u{1F50D} \u91CD\u65B0\u626B\u63CF';b.disabled=false;"
"}).catch(function(){$('wl').innerHTML='<div style=\"text-align:center;padding:12px;color:#ef4444\">\u626B\u63CF\u5931\u8D25</div>';b.textContent='\u{1F50D} \u626B\u63CF WiFi';b.disabled=false})}"


"function saveAll(){var d={};"
"['wifi_ssid','wifi_pass','device_mac','device_token','device_ble_key','mqtt_broker','mqtt_user','mqtt_pass','mqtt_topic_prefix','bemfa_uid','aida64_server'].forEach(function(k){d[k]=$(k).value});"
"d.aida64_port=parseInt($('aida64_port').value)||7789;"
"d.mqtt_port=parseInt($('mqtt_port').value)||1883;"
"d.mqtt_enable=$('mqtt_enable').checked;d.bemfa_enable=$('bemfa_enable').checked;"
"for(var r=0;r<3;r++){d['siv'+r+'0']=parseInt($('siv'+r+'0').value);d['siv'+r+'1']=parseInt($('siv'+r+'1').value)}"
"d.mem_siv_pct=parseInt($('mem_siv_pct').value);d.mem_siv_used=parseInt($('mem_siv_used').value);"
"var m=$('msg');m.textContent='\u23F3 \u4FDD\u5B58\u4E2D...';m.className='';m.style.display='block';"
"fetch('/api/provision',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})"
".then(function(r){return r.json()}).then(function(r){m.textContent=r.message||'\u2705 \u5DF2\u4FDD\u5B58';m.className='ok';setTimeout(function(){location.reload()},3000)})"
".catch(function(){m.textContent='\u4FDD\u5B58\u5931\u8D25';m.className='ok';m.style.color='#ef4444'})}"
"</script></body></html>";

static int _get_config_page_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, _CFG_HTML, sizeof(_CFG_HTML) - 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return 0;
}

/* ==================== 显示配置 API ==================== */
static int _get_display_handler(httpd_req_t *req) {
    display_config_t cfg = display_config_load();
    cJSON *root = cJSON_CreateObject();
    for (int r = 0; r < 3; r++) {
        char k[16];
        snprintf(k, sizeof(k), "siv%d0", r); cJSON_AddNumberToObject(root, k, cfg.rows[r].siv[0]);
        snprintf(k, sizeof(k), "siv%d1", r); cJSON_AddNumberToObject(root, k, cfg.rows[r].siv[1]);
    }
    cJSON_AddNumberToObject(root, "mem_siv_pct", cfg.mem_siv_pct);
    cJSON_AddNumberToObject(root, "mem_siv_used", cfg.mem_siv_used);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json); cJSON_Delete(root);
    return 0;
}

/* ==================== 服务器启动 ==================== */

/* Debug: charge history */
static void register_uris(void) {
    const httpd_uri_t uris[] = {
        { .uri = "/api/ping",          .method = HTTP_GET,  .handler = _get_ping_handler },
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = _get_status_handler },
        { .uri = "/api/config",        .method = HTTP_GET,  .handler = _get_config_handler },
        { .uri = "/api/config",        .method = HTTP_POST, .handler = _post_config_handler },
        { .uri = "/api/provision",     .method = HTTP_POST, .handler = _post_provision_handler },
        { .uri = "/api/wifi_scan",     .method = HTTP_GET,  .handler = _get_wifi_scan_handler },
        { .uri = "/api/display",       .method = HTTP_GET,  .handler = _get_display_handler },
        { .uri = "/api/ports",         .method = HTTP_GET,  .handler = _get_ports_handler },
        { .uri = "/api/settings",      .method = HTTP_GET,  .handler = _get_settings_handler },
        { .uri = "/api/port",          .method = HTTP_POST, .handler = _post_port_handler },
        { .uri = "/api/setting",       .method = HTTP_POST, .handler = _post_setting_handler },
        { .uri = "/api/protocol",      .method = HTTP_POST, .handler = _post_protocol_handler },
        { .uri = "/api/ble",           .method = HTTP_POST, .handler = _post_ble_handler },
        { .uri = "/api/sleep",         .method = HTTP_GET,  .handler = _get_sleep_handler },
        { .uri = "/api/sleep",         .method = HTTP_POST, .handler = _post_sleep_handler },
        { .uri = "/config",            .method = HTTP_GET,  .handler = _get_config_page_handler },
        /* 强制门户检测 */
        { .uri = "/generate_204",      .method = HTTP_GET,  .handler = _captive_redirect_handler },
        { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = _captive_redirect_handler },
        { .uri = "/connecttest.txt",   .method = HTTP_GET,  .handler = _captive_redirect_handler },
    };
    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(_server, &uris[i]);
    }
    /* 主页：配网模式 -> 配置页，正常模式 -> 仪表盘 */
    const httpd_uri_t main_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = _provisioning ? _get_config_page_handler : _get_dash_handler
    };
    httpd_register_uri_handler(_server, &main_uri);
}

void http_server_start(DeviceConfig *cfg, http_config_cb on_save) {
    if (_server) {
        httpd_stop(_server);
        _server = NULL;
    }
    memcpy(&_cfg_store, cfg, sizeof(DeviceConfig));
    _on_save = on_save;
    _provisioning = false;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_resp_headers = 8;
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.lru_purge_enable = 1;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    ESP_LOGI(TAG, "Starting dashboard server on port 80, free heap=%lu", (unsigned long)esp_get_free_heap_size());
    if (httpd_start(&_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }
    register_uris();
    ESP_LOGI(TAG, "Dashboard server started on port 80");
}

void http_server_start_provisioning(void) {
    if (_server) return;
    _provisioning = true;

    /* 加载现有配置，供配置页显示当前值 */
    config_store_load(&_cfg_store);

    /* 配网用 WiFi 扫描 */
    wifi_scan_sync();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_resp_headers = 8;
    config.max_uri_handlers = 24;
    config.stack_size = 8192;
    config.lru_purge_enable = 1;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    ESP_LOGI(TAG, "Starting provisioning server on port 80");
    if (httpd_start(&_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }
    register_uris();

    /* 强制门户的 DNS 劫持 */
    xTaskCreatePinnedToCore(dns_hijack_task, "dns_hijack", 3072, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "Provisioning server started (DNS hijack + captive portal)");
}

void http_server_stop(void) {
    if (_server) { httpd_stop(_server); _server = NULL; }
}
