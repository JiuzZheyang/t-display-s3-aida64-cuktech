#include "boot_anim.h"
#include "lv_port.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "boot_anim";

#include "display_config.h"

#define C_BG        theme_get_bg(display_config_load_theme())
#define C_TEXT3     0x9CA3AF
#define C_ACCENT    0x2563EB
#define C_ACCENT2   0x10B981

#define TIMER_PERIOD  50

static volatile bool s_ready = false;
static volatile bool s_wifi_connected = false;
static volatile bool s_bt_connecting = false;
static volatile bool s_bt_ready_flag = false;
static volatile bool s_switch_now = false;
static volatile bool s_timeout = false;
static volatile bool s_ip_set = false;
static volatile bool s_progress_complete = false;
static volatile bool s_switch_delayed = false;
static lv_obj_t   *s_boot_scr     = NULL;
static lv_obj_t   *s_target_scr   = NULL;
static lv_timer_t *s_switch_timer = NULL;
static lv_timer_t *s_anim_timer   = NULL;
static uint32_t    s_min_ms       = 2000;
static uint32_t    s_max_ms       = 5000;
static uint32_t    s_start_tick   = 0;

static lv_obj_t *s_title = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_ip_label = NULL;
static lv_obj_t *s_wifi_icon = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_bt_icon = NULL;
static lv_obj_t *s_bt_label = NULL;

void boot_anim_signal_ready(void)
{
    if (!s_boot_scr) return;
    s_ready = true;
    s_wifi_connected = true;
    if (s_wifi_icon && s_wifi_label) {
        lvgl_port_lock(portMAX_DELAY);
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(C_ACCENT2), 0);
        lv_label_set_text(s_wifi_label, "OK");
        lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(C_ACCENT2), 0);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "ready signal received");
}

void boot_anim_set_ip(const char *ip)
{
    if (!s_boot_scr || !s_ip_label) return;
    s_ip_set = true;
    lvgl_port_lock(portMAX_DELAY);
    lv_label_set_text(s_ip_label, ip);
    lv_obj_set_style_text_color(s_ip_label, lv_color_hex(C_ACCENT2), 0);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "IP: %s", ip);
}

void boot_anim_signal_failed(void)
{
    if (!s_boot_scr) return;
    s_ready = true;
    s_wifi_connected = false;
    if (s_ip_label) {
        lvgl_port_lock(portMAX_DELAY);
        lv_label_set_text(s_ip_label, "WiFi Failed");
        lv_obj_set_style_text_color(s_ip_label, lv_color_hex(0xEF4444), 0);
        lvgl_port_unlock();
    }
    ESP_LOGW(TAG, "WiFi failed signal received");
}

void boot_anim_switch_now(void)
{
    s_switch_now = true;
}

void boot_anim_set_loading_text(const char *text)
{
    (void)text;
}

/* BLE 开始连接时调用，进度条开始从 50% 向 100% 走 */
void boot_anim_bt_connecting(void)
{
    s_bt_connecting = true;
    ESP_LOGI(TAG, "bt connecting signal");
}

void boot_anim_bt_ready(void)
{
    if (!s_boot_scr) return;
    s_bt_ready_flag = true;
    if (!s_bt_icon || !s_bt_label) return;
    lvgl_port_lock(portMAX_DELAY);
    lv_obj_set_style_text_color(s_bt_icon, lv_color_hex(C_ACCENT2), 0);
    lv_label_set_text(s_bt_label, "OK");
    lv_obj_set_style_text_color(s_bt_label, lv_color_hex(C_ACCENT2), 0);
    lvgl_port_unlock();
}

static void anim_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t elapsed = lv_tick_elaps(s_start_tick);

    /* 进度条逻辑：
     * - 从 0% 匀速缓慢走到 85%（约 6 秒）
     * - 到 85% 后等待 BLE 就绪
     * - BLE 就绪后：85% → 100%（0.5 秒）
     * - 100% 后等 1 秒跳转
     * - WiFi 超时 15 秒直接跳配网页面
     */
    if (s_progress_bar) {
        uint32_t progress = 0;

        if (s_bt_ready_flag) {
            /* BLE 就绪：85% → 100%，0.5 秒走到 */
            static uint32_t s_fill_start = 0;
            if (s_fill_start == 0) s_fill_start = lv_tick_get();
            uint32_t fill_elapsed = lv_tick_elaps(s_fill_start);
            progress = 85 + (fill_elapsed * 15) / 500;
            if (progress > 100) progress = 100;

            if (progress >= 100) {
                if (!s_progress_complete) {
                    s_progress_complete = true;
                    s_switch_delayed = true;
                    ESP_LOGI(TAG, "progress 100%%, waiting 1s before switch");
                }
                if (s_switch_delayed && fill_elapsed > 1500) {
                    s_switch_delayed = false;
                    boot_anim_switch_now();
                }
            }
        } else {
            /* 匀速走到 85%，约 6 秒 */
            static uint32_t s_progress_start = 0;
            if (s_progress_start == 0) s_progress_start = lv_tick_get();
            uint32_t progress_elapsed = lv_tick_elaps(s_progress_start);
            progress = (progress_elapsed * 85) / 6000;
            if (progress > 85) progress = 85;
        }

        lv_bar_set_value(s_progress_bar, progress, LV_ANIM_ON);
    }

    /* Jiuzy_ 标题动画：前 1.5 秒从上方滑入 + 淡入 */
    if (s_title) {
        uint32_t anim_duration = 1500;
        if (elapsed < anim_duration) {
            int start_y = -120;
            int end_y = -55;
            int current_y = start_y + ((int)(elapsed * (end_y - start_y)) / (int)anim_duration);
            lv_obj_set_y(s_title, current_y);
            uint8_t opa = (elapsed * 255) / anim_duration;
            lv_obj_set_style_text_opa(s_title, opa, 0);
        } else {
            lv_obj_set_y(s_title, -55);
            lv_obj_set_style_text_opa(s_title, 255, 0);
        }
    }
}

/* 销毁启动屏并清空所有子对象指针，防止后续访问野指针 */
static void boot_anim_destroy(void)
{
    if (s_boot_scr) {
        lv_obj_del(s_boot_scr);
    }
    s_boot_scr       = NULL;
    s_title          = NULL;
    s_progress_bar   = NULL;
    s_ip_label       = NULL;
    s_wifi_icon      = NULL;
    s_wifi_label     = NULL;
    s_bt_icon        = NULL;
    s_bt_label       = NULL;
    if (s_anim_timer)   { lv_timer_del(s_anim_timer);   s_anim_timer   = NULL; }
    if (s_switch_timer) { lv_timer_del(s_switch_timer); s_switch_timer = NULL; }
}

static void switch_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t elapsed = lv_tick_elaps(s_start_tick);

    /* 10秒超时：WiFi连不上，跳配网页面 */
    if (elapsed >= 10000) {
        s_timeout = true;
        /* WiFi未连接时跳配网页面，否则跳AIDA64 */
        if (!s_wifi_connected) {
            /* 配网页面由 wifi_prov 自己显示，我们只需清除启动页 */
            boot_anim_destroy();
            ESP_LOGI(TAG, "timeout 10s, WiFi not connected, switching to config");
        } else if (s_target_scr) {
            lv_scr_load(s_target_scr);
            boot_anim_destroy();
            ESP_LOGI(TAG, "timeout 10s, WiFi connected, switching to AIDA64");
        }
        ESP_LOGI(TAG, "timeout 35s, forced switch");
        return;
    }

    if (s_switch_now && s_target_scr) {
        lv_scr_load(s_target_scr);
        boot_anim_destroy();
        ESP_LOGI(TAG, "switched to target screen");
    }
}

void boot_anim_init(void)
{
    lvgl_port_lock(portMAX_DELAY);

    s_boot_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_boot_scr, lv_color_hex(C_BG), 0);
    lv_obj_clear_flag(s_boot_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_boot_scr, 0, 0);

    /* 标题 Jiuzy_ - 初始在屏幕上方外（y=-120），由动画移入 */
    s_title = lv_label_create(s_boot_scr);
    lv_label_set_text(s_title, "Jiuzy_");
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(s_title, 0, 0);
    lv_obj_set_style_text_letter_space(s_title, 2, 0);
    lv_obj_set_y(s_title, -120);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 0);

    /* 第二行：WiFi图标 + "WiFi" 文字 + 蓝牙图标 + "BT" 文字 */
    s_wifi_icon = lv_label_create(s_boot_scr);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(C_TEXT3), 0);
    lv_obj_align(s_wifi_icon, LV_ALIGN_CENTER, -45, 5);

    s_wifi_label = lv_label_create(s_boot_scr);
    lv_label_set_text(s_wifi_label, "WiFi");
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_label, lv_color_hex(C_TEXT3), 0);
    lv_obj_align(s_wifi_label, LV_ALIGN_CENTER, -15, 5);

    s_bt_icon = lv_label_create(s_boot_scr);
    lv_label_set_text(s_bt_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_bt_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bt_icon, lv_color_hex(C_TEXT3), 0);
    lv_obj_align(s_bt_icon, LV_ALIGN_CENTER, 20, 5);

    s_bt_label = lv_label_create(s_boot_scr);
    lv_label_set_text(s_bt_label, "BT");
    lv_obj_set_style_text_font(s_bt_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bt_label, lv_color_hex(C_TEXT3), 0);
    lv_obj_align(s_bt_label, LV_ALIGN_CENTER, 48, 5);

    /* 第三行：进度条 */
    s_progress_bar = lv_bar_create(s_boot_scr);
    lv_obj_set_size(s_progress_bar, 214, 10);
    lv_bar_set_range(s_progress_bar, 0, 100);
    lv_bar_set_value(s_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_progress_bar, 5, 0);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 5, LV_PART_INDICATOR);
    lv_obj_align(s_progress_bar, LV_ALIGN_CENTER, 0, 35);

    /* 第四行：IP地址 */
    s_ip_label = lv_label_create(s_boot_scr);
    lv_label_set_text(s_ip_label, "");
    lv_obj_set_style_text_font(s_ip_label, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_ip_label, lv_color_hex(C_TEXT3), 0);
    lv_obj_align(s_ip_label, LV_ALIGN_CENTER, 0, 75);

    lv_scr_load_anim(s_boot_scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    lvgl_port_unlock();

    s_start_tick = lv_tick_get();
    s_anim_timer = lv_timer_create(anim_timer_cb, TIMER_PERIOD, NULL);
    ESP_LOGI(TAG, "boot screen created");
}

void boot_anim_play(void *target, uint32_t min_ms, uint32_t max_ms)
{
    s_target_scr  = (lv_obj_t *)target;
    s_min_ms      = min_ms;
    s_max_ms      = max_ms;
    s_ready       = false;
    s_wifi_connected = false;
    s_switch_now  = false;
    s_ip_set      = false;
    s_bt_ready_flag = false;
    s_bt_connecting = false;
    s_progress_complete = false;
    s_switch_delayed = false;
    s_start_tick  = lv_tick_get();
    s_switch_timer = lv_timer_create(switch_timer_cb, 100, NULL);
    ESP_LOGI(TAG, "switch timer started (min=%lu, max=%lu)", min_ms, max_ms);
}

bool boot_anim_timed_out(void)
{
    return s_timeout;
}