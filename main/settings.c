#include "settings.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "settings";

#define DEFAULT_AIDA64_SERVER ""

static const settings_t default_settings = {
    .wifi_ssid     = "",
    .wifi_pass     = "",
    .aida64_server = DEFAULT_AIDA64_SERVER,
    .aida64_port   = 7789,
    .configured    = false,
};

settings_t settings_load(void)
{
    settings_t s = default_settings;
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed %d, using defaults", err);
        return s;
    }

    size_t ssid_len = sizeof(s.wifi_ssid);
    size_t pass_len = sizeof(s.wifi_pass);
    size_t srv_len  = sizeof(s.aida64_server);

    if (nvs_get_str(h, "ssid", s.wifi_ssid, &ssid_len) != ESP_OK) s.wifi_ssid[0] = '\0';
    if (nvs_get_str(h, "pass", s.wifi_pass, &pass_len) != ESP_OK) s.wifi_pass[0] = '\0';
    if (nvs_get_str(h, "srv",  s.aida64_server, &srv_len) != ESP_OK) s.aida64_server[0] = '\0';

    uint8_t cfg_flag = 0;
    if (nvs_get_u8(h, "cfg", &cfg_flag) == ESP_OK) {
        s.configured = (cfg_flag != 0);
    } else {
        s.configured = false;
    }

    /* 端口 */
    uint16_t port = 0;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port > 0) {
        s.aida64_port = port;
    }

    /* 若未配置过 srv，使用默认 */
    if (s.aida64_server[0] == '\0') {
        strncpy(s.aida64_server, DEFAULT_AIDA64_SERVER, sizeof(s.aida64_server) - 1);
    }

    nvs_close(h);
    ESP_LOGI(TAG, "loaded: configured=%d, ssid='%s', srv='%s', port=%u",
             s.configured, s.wifi_ssid, s.aida64_server, s.aida64_port);
    return s;
}

bool settings_save(const settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed %d", err);
        return false;
    }

    if (nvs_set_str(h, "ssid", s->wifi_ssid) != ESP_OK) goto fail;
    if (nvs_set_str(h, "pass", s->wifi_pass) != ESP_OK) goto fail;
    if (nvs_set_str(h, "srv",  s->aida64_server) != ESP_OK) goto fail;
    if (nvs_set_u16(h, "port", s->aida64_port) != ESP_OK) goto fail;
    if (nvs_set_u8(h, "cfg", s->configured ? 1 : 0) != ESP_OK) goto fail;

    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed %d", err);
        return false;
    }
    ESP_LOGI(TAG, "saved: configured=%d, srv='%s', port=%u", s->configured, s->aida64_server, s->aida64_port);
    return true;

fail:
    nvs_close(h);
    return false;
}

bool settings_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return false;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings reset");
    return err == ESP_OK;
}

const char *settings_default_server(void)
{
    return DEFAULT_AIDA64_SERVER;
}

uint16_t settings_default_port(void)
{
    return 7789;
}

bool settings_is_configured(void)
{
    settings_t s = settings_load();
    return s.configured && s.wifi_ssid[0] != '\0';
}
