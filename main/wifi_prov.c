#include "wifi_prov.h"
#include "esp_timer.h"
#include "settings.h"
#include "http_server.h"
#include "boot_anim.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdio.h>
#include "ui.h"
#include "esp_system.h"

static const char *TAG = "wifi_prov";

/* RTC 内存标志：软重启后保留。为魔数时表示“强制进配网”（保留 WiFi 配置） */
#define PORTAL_REQUEST_MAGIC  0x50524F56  /* 'PROV' */
static RTC_NOINIT_ATTR uint32_t s_portal_request;

/* 配网热点 SSID / 密码 */
#define PROV_AP_SSID       "SmartMonitor"
#define PROV_AP_PASS       "12345678"
#define PROV_AP_CHANNEL    1
#define TRY_CONNECT_MS     12000   /* 尝试已保存WiFi的超时 */
#define RECONNECT_MS       5000    /* 重试间隔 */

static volatile wifi_prov_state_t s_state     = WP_STATE_IDLE;
static volatile bool              s_connected  = false;
static char                      s_ssid[33]   = {0};
static volatile bool             s_force_portal = false;

/* 前向声明 */
static void start_softap(void);
static void start_sta(const char *ssid, const char *pass);

/* ========== WiFi 事件处理 ========== */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    /* ---- STA 事件 ---- */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == WP_STATE_TRY_SAVED) {
            ESP_LOGW(TAG, "STA disconnected, retrying...");
            s_connected = false;
            esp_wifi_connect();
        } else if (s_state == WP_STATE_AP_RUNNING) {
            ESP_LOGW(TAG, "STA disconnected");
            s_connected = false;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_state == WP_STATE_TRY_SAVED || s_state == WP_STATE_AP_RUNNING) {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));

            /* 通知启动页面显示IP地址 */
            {
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
                boot_anim_set_ip(ip_str);
            }

            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                strncpy(s_ssid, (char *)ap.ssid, sizeof(s_ssid) - 1);
                s_ssid[sizeof(s_ssid) - 1] = '\0';
                ESP_LOGI(TAG, "Connected to SSID: %s", s_ssid);
            }

            /* AP 模式下获得 IP 时才启动配网服务器（STA 模式由主循环启动仪表盘） */
            if (s_force_portal) {
                http_server_start_provisioning();
                ESP_LOGI(TAG, "Got IP in portal mode, staying in config screen");
            } else {
                s_connected = true;
                s_state = WP_STATE_CONNECTED;
                wifi_prov_signal_connected();  /* 通知开机动画切换 */
            }
        }
    }
    /* ---- AP 事件（手机连上热点） ---- */
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_state == WP_STATE_AP_RUNNING) {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: device connected (aid=%d)", ev->aid);
        }
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_state == WP_STATE_AP_RUNNING) {
            ESP_LOGI(TAG, "AP: device disconnected");
        }
    }
}

/* ========== 初始化通用 WiFi 基础设配（只能执行一次） ========== */
static bool s_wifi_inited = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;

static void wifi_common_init(void)
{
    if (s_wifi_inited) return;
    s_wifi_inited = true;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    assert(s_ap_netif);
    s_sta_netif = esp_netif_create_default_wifi_sta();
    assert(s_sta_netif);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
}

/* ========== 启动 STA（连接指定WiFi） ========== */
static void start_sta(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "STA connecting to: %s", ssid);

    wifi_common_init();

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (pass && pass[0] != '\0') {
        strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ========== 启动 SoftAP（配网模式） ========== */
static void start_softap(void)
{
    ESP_LOGI(TAG, "Starting SoftAP: %s", PROV_AP_SSID);

    wifi_common_init();

    /* 默认 AP IP 已是 192.168.4.1，DHCP 已自动启动，无需手动配置 */

    wifi_config_t ap_cfg = {
        .ap = {
            .channel        = PROV_AP_CHANNEL,
            .authmode       = WIFI_AUTH_OPEN,
            .ssid_hidden    = 0,
            .max_connection = 4,
            .beacon_interval = 100,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid,     PROV_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started → connect to '%s' then open 192.168.4.1",
             PROV_AP_SSID);

    /* 在 LCD 上显示配网提示界面 */
    ui_show_config_screen();
}

/* ========== 配网任务 ========== */
void wifi_prov_task(void *param)
{
    (void)param;

    /* 检查是否被请求强制进配网（长按 BOOT 触发，保留 WiFi 配置） */
    bool force_portal = (s_portal_request == PORTAL_REQUEST_MAGIC);
    s_portal_request = 0;  /* 清标志，下次重启恢复正常 */

    /* 加载已保存配置 */
    settings_t cfg = settings_load();

    if (force_portal) {
        ESP_LOGW(TAG, "Forced config portal (WiFi config kept)");
        s_force_portal = true;
        start_softap();
        http_server_start_provisioning();
        s_state = WP_STATE_AP_RUNNING;
        ESP_LOGI(TAG, "wifi_prov_task exiting, state=%d", s_state);
        vTaskDelete(NULL);
        return;
    }

    if (cfg.configured && cfg.wifi_ssid[0] != '\0') {
        /* 有已保存WiFi → 先尝试连接 */
        s_state = WP_STATE_TRY_SAVED;
        start_sta(cfg.wifi_ssid, cfg.wifi_pass);

        int64_t start = esp_timer_get_time() / 1000;
        bool already_ap = false;

        while (1) {
            if (s_connected) {
                ESP_LOGI(TAG, "Saved WiFi connected!");
                break;
            }

            int64_t now = esp_timer_get_time() / 1000;

            /* 超时 → 切换 AP 配网模式 */
            if (!already_ap && (now - start >= TRY_CONNECT_MS)) {
                ESP_LOGW(TAG, "Saved WiFi timeout (%d ms), starting AP mode", TRY_CONNECT_MS);

                esp_wifi_stop();
                vTaskDelay(pdMS_TO_TICKS(200));

                start_softap();
                http_server_start_provisioning();
                already_ap = true;
                s_state = WP_STATE_AP_RUNNING;
            }

            if (s_state == WP_STATE_FAIL) break;

            vTaskDelay(pdMS_TO_TICKS(200));
        }
    } else {
        /* 无已保存WiFi → 直接启动 AP 配网 */
        start_softap();
        http_server_start_provisioning();
        s_state = WP_STATE_AP_RUNNING;
    }

    ESP_LOGI(TAG, "wifi_prov_task exiting, state=%d", s_state);
    vTaskDelete(NULL);
}

/* ========== 外部调用 ========== */
void wifi_prov_init(void)
{
    s_state    = WP_STATE_IDLE;
    s_connected = false;
    memset(s_ssid, 0, sizeof(s_ssid));

    xTaskCreatePinnedToCore(wifi_prov_task, "wifi_prov", 8192, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "WiFi provisioning task created");
}

wifi_prov_state_t wifi_prov_get_state(void)
{
    return s_state;
}

const char *wifi_prov_get_connected_ssid(void)
{
    return s_ssid;
}

void wifi_prov_signal_connected(void)
{
    boot_anim_signal_ready();
}

/* 请求进入配网模式（保留 WiFi 配置）：置 RTC 标志后重启 */
void wifi_prov_request_portal(void)
{
    ESP_LOGW(TAG, "Portal requested, restarting into config mode...");
    s_portal_request = PORTAL_REQUEST_MAGIC;
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}
