#include "display_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "disp_cfg";
#define NVS_NS "disp_cfg"

static const display_config_t s_default = {
    .titles = { "CPU", "GPU" },
    .row_count = 3,
    .rows = {
        { .label = "\xe4\xbd\xbf\xe7\x94\xa8\xe7\x8e\x87", .siv = { 1, 2 } },  /* 使用率 SIV1,SIV2 */
        { .label = "\xe6\xb8\xa9\xe5\xba\xa6",       .siv = { 3, 4 } },  /* 温度 SIV3,SIV4 */
        { .label = "\xe5\x8a\x9f\xe7\x8e\x87",       .siv = { 5, 6 } },  /* 功率 SIV5,SIV6 */
    },
    .mem_siv_pct = 7,
    .mem_siv_used = 8,
};

display_config_t display_config_load(void)
{
    display_config_t cfg;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, using defaults");
        return s_default;
    }
    size_t len = sizeof(cfg);
    if (nvs_get_blob(h, "layout3", &cfg, &len) != ESP_OK || len != sizeof(cfg)) {
        ESP_LOGW(TAG, "NVS read failed, using defaults");
        cfg = s_default;
    }
    nvs_close(h);
    if (cfg.row_count < 1 || cfg.row_count > MAX_ROWS) {
        ESP_LOGW(TAG, "Invalid row count %d, resetting to defaults", cfg.row_count);
        cfg = s_default;
    }
    ESP_LOGI(TAG, "loaded: %d rows, titles=[%s,%s]",
             cfg.row_count, cfg.titles[0], cfg.titles[1]);
    return cfg;
}

bool display_config_save(const display_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, "layout3", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved: %d rows", cfg->row_count);
    return err == ESP_OK;
}

int display_config_load_theme(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t v = 0;
    if (nvs_get_u8(h, "theme", &v) != ESP_OK) v = 1; /* black */
    nvs_close(h);
    return (int)v;
}

bool display_config_save_theme(int idx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, "theme", (uint8_t)idx);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

uint32_t theme_get_bg(int idx)
{
    static const uint32_t bg_colors[] = { 0xF4F5F7, 0x000000, 0xFFFFFF, 0xFDF3E3 };
    if (idx < 0 || idx > 3) return 0xF4F5F7;
    return bg_colors[idx];
}

uint8_t clock_config_load_fmt(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "clk_fmt", &v) == ESP_OK) { nvs_close(h); return v; }
        nvs_close(h);
    }
    return 0;  /* default 24h */
}

bool clock_config_save_fmt(uint8_t fmt)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, "clk_fmt", fmt);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
uint8_t clock_config_load_sec(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 1;
        if (nvs_get_u8(h, "clk_sec", &v) == ESP_OK) { nvs_close(h); return v; }
        nvs_close(h);
    }
    return 1; /* default show seconds */
}

bool clock_config_save_sec(uint8_t sec)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, "clk_sec", sec);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

uint8_t clock_config_load_style(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "clk_sty", &v) == ESP_OK) { nvs_close(h); return v; }
        nvs_close(h);
    }
    return 0;
}

bool clock_config_save_style(uint8_t style)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, "clk_sty", style);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}
