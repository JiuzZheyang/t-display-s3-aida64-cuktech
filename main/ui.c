#include "ui.h"
#include "lv_port.h"
#include "lvgl.h"
#include "bt_icons.h"
#include "esp_log.h"
#include "battery.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>

extern bool ble_get_ready_flag(void);

static const char *TAG = "ui";

#define C_BG      0xF4F5F7
#define C_TEXT    0x1A1D21
#define C_TEXT2   0x6B7280
#define C_CPU     0x2563EB
#define C_GPU     0xF59E0B
#define C_BAT     0x10B981

static display_config_t s_cfg;
static lv_obj_t *main_scr = NULL;
static lv_obj_t *val_labels[MAX_ROWS][2];
static lv_obj_t *row_labels[MAX_ROWS];   /* 左侧行标签 */
static lv_obj_t *title_labels[2];        /* CPU/GPU 列标题 */
static lv_obj_t *battery_label = NULL;
static lv_obj_t *batt_body = NULL;   /* 电池外框 */
static lv_obj_t *batt_fill = NULL;   /* 电池内部填充体 */
static lv_obj_t *conn_label = NULL;
static lv_obj_t *batt_tip = NULL;    /* 电池正极头 */
static lv_obj_t *mem_bar = NULL;     /* 内存进度条 */
static lv_obj_t *mem_label = NULL;   /* 内存文字 */
static lv_obj_t *mem_title = NULL;  /* "内存"标题 */
static lv_obj_t *charger_label __attribute__((unused)) = NULL;

/* ---- 主题（背景色 / 主文字色 / 次文字色）--- */
typedef struct { uint32_t bg; uint32_t text; uint32_t text2; } ui_theme_t;
static const ui_theme_t s_themes[] = {
    { 0xF4F5F7, 0x1A1D21, 0x6B7280 },  /* 0 浅灰（默认，深色字） */
    { 0x000000, 0xFFFFFF, 0x9CA3AF },  /* 1 黑色（白色字） */
    { 0xFFFFFF, 0x000000, 0x6B7280 },  /* 2 白色（黑色字） */
    { 0xFDF3E3, 0x1A1206, 0x8B6F47 },  /* 3 暖色（黑色字） */
};
#define UI_THEME_COUNT  (sizeof(s_themes) / sizeof(s_themes[0]))
static int s_theme_idx = 1; /* default black */

/* 电池图形尺寸 */
#define BATT_X      4
#define BATT_Y      4
#define BATT_W      26
#define BATT_H      13
#define BATT_PAD    2    /* 外框内边距 */

extern const lv_font_t font_cn_18;
extern const lv_font_t font_num_32;
extern const lv_font_t font_num_48;
extern const lv_font_t frick_14;
extern const lv_font_t frick_32;
extern const lv_font_t frick_48;
extern const lv_font_t frick_22;
extern const lv_font_t frick_64;
extern const lv_font_t frick_cond_72;
extern const lv_font_t frick_cond_80;
extern const lv_font_t frick_cond_96;

/* 亮度 toast */
static lv_obj_t *toast_obj = NULL;
static lv_timer_t *toast_timer = NULL;

static void toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (toast_obj) {
        lv_obj_fade_out(toast_obj, 300, 0);
        toast_timer = NULL;
    }
}

void ui_show_brightness_toast(int percent)
{
    lvgl_port_lock(0);
    if (toast_obj) { lv_obj_del(toast_obj); toast_obj = NULL; }
    if (toast_timer) { lv_timer_del(toast_timer); toast_timer = NULL; }

    toast_obj = lv_obj_create(main_scr);
    lv_obj_set_size(toast_obj, 170, 40);
    lv_obj_set_align(toast_obj, LV_ALIGN_TOP_MID);
    lv_obj_set_y(toast_obj, 12);
    lv_obj_set_style_bg_color(toast_obj, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_radius(toast_obj, 20, 0);
    lv_obj_set_style_shadow_width(toast_obj, 0, 0);
    lv_obj_set_style_border_width(toast_obj, 0, 0);
    lv_obj_clear_flag(toast_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(toast_obj, 0, 0);

    lv_obj_t *bar = lv_bar_create(toast_obj);
    lv_obj_set_size(bar, 130, 12);
    lv_obj_center(bar);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x374151), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xF9FAFB), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 5, 100);
    lv_bar_set_value(bar, percent, LV_ANIM_ON);

    lv_obj_fade_in(toast_obj, 200, 0);
    toast_timer = lv_timer_create(toast_timer_cb, 2500, NULL);
    lvgl_port_unlock();
}

display_config_t *ui_get_config(void) { return &s_cfg; }

static int row_y(int row)
{
    if (s_cfg.row_count <= 2) return 42 + row * 46;
    if (s_cfg.row_count <= 3) return 38 + row * 36;
    return 30 + row * 28;
}

static const lv_font_t *title_font(void)
{
    return s_cfg.row_count <= 3 ? &lv_font_montserrat_32 : &lv_font_montserrat_18;
}

static const lv_font_t *value_font(void)
{
    return s_cfg.row_count <= 3 ? &font_num_32 : &lv_font_montserrat_18;
}

static lv_obj_t *make_label(lv_obj_t *parent, int x, int y, int w,
                            const char *text, const lv_font_t *f,
                            lv_color_t color, bool center)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_pos(l, x, y);
    if (center) {
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, w);
    }
    return l;
}

lv_obj_t *ui_get_main_screen(void) { return main_scr; }

void ui_init(void)
{
    s_cfg = display_config_load();
    if (s_cfg.row_count < 1) s_cfg.row_count = 1;
    if (s_cfg.row_count > MAX_ROWS) s_cfg.row_count = MAX_ROWS;

    lvgl_port_lock(0);

    main_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(main_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(main_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(main_scr, 0, 0);
    lv_obj_set_style_border_width(main_scr, 0, 0);

    /* 电池图标（左上角）—— 手机样式：外框 + 正极头 + 填充体 + 百分比 */
    batt_body = lv_obj_create(main_scr);
    lv_obj_set_size(batt_body, BATT_W, BATT_H);
    lv_obj_set_pos(batt_body, BATT_X, BATT_Y);
    lv_obj_set_style_radius(batt_body, 3, 0);
    lv_obj_set_style_bg_opa(batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_body, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_style_border_width(batt_body, 2, 0);
    lv_obj_set_style_pad_all(batt_body, 0, 0);
    lv_obj_clear_flag(batt_body, LV_OBJ_FLAG_SCROLLABLE);

    /* 正极头（右侧小突起） */
    batt_tip = lv_obj_create(main_scr);
    lv_obj_set_size(batt_tip, 3, 6);
    lv_obj_set_pos(batt_tip, BATT_X + BATT_W, BATT_Y + (BATT_H - 6) / 2);
    lv_obj_set_style_radius(batt_tip, 1, 0);
    lv_obj_set_style_bg_color(batt_tip, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_style_bg_opa(batt_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(batt_tip, 0, 0);
    lv_obj_clear_flag(batt_tip, LV_OBJ_FLAG_SCROLLABLE);

    /* 内部填充体 */
    batt_fill = lv_obj_create(batt_body);
    lv_obj_set_size(batt_fill, 0, BATT_H - 2 * BATT_PAD - 4);
    lv_obj_set_align(batt_fill, LV_ALIGN_LEFT_MID);
    lv_obj_set_x(batt_fill, BATT_PAD - 2);
    lv_obj_set_style_radius(batt_fill, 1, 0);
    lv_obj_set_style_bg_color(batt_fill, lv_color_hex(C_BAT), 0);
    lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(batt_fill, 0, 0);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* 百分比文字（电池右侧） */
    battery_label = lv_label_create(main_scr);
    lv_label_set_text(battery_label, "--%");
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(battery_label, BATT_X + BATT_W + 7, BATT_Y);

    /* 连接状态（右上角） */
    conn_label = lv_label_create(main_scr);
    lv_label_set_text(conn_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(conn_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(conn_label, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(conn_label, 300, 2);

    /* 充电器功率已移至独立页面，AIDA64 页面不再显示 */

    /* 列标题 */
    const lv_font_t *tf = title_font();
    title_labels[0] = make_label(main_scr, 70, 5, 100, s_cfg.titles[0], tf, lv_color_hex(C_CPU), true);
    title_labels[1] = make_label(main_scr, 180, 5, 100, s_cfg.titles[1], tf, lv_color_hex(C_GPU), true);

    /* 行标签 + 值卡片 + 值标签 */
    const lv_font_t *vf = value_font();
    for (int r = 0; r < s_cfg.row_count; r++) {
        int y = row_y(r);
        /* 左侧行标签 */
        row_labels[r] = make_label(main_scr, 6, y + 8, 50, s_cfg.rows[r].label,
                   &font_cn_18, lv_color_hex(C_TEXT2), false);
        /* CPU 值 */
        val_labels[r][0] = make_label(main_scr, 58, y, 120, "--", vf,
                                      lv_color_hex(C_TEXT), true);
        /* GPU 值 */
        val_labels[r][1] = make_label(main_scr, 168, y, 120, "--", vf,
                                      lv_color_hex(C_TEXT), true);
    }

    /* "内存"标题（进度条左侧） */
    mem_title = lv_label_create(main_scr);
    lv_label_set_text(mem_title, "\xe5\x86\x85\xe5\xad\x98");  /* \u5185\u5b58 */
    lv_obj_set_style_text_font(mem_title, &font_cn_18, 0);
    lv_obj_set_style_text_color(mem_title, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(mem_title, 4, 150);

    /* 内存使用率进度条（底部） */
    mem_bar = lv_bar_create(main_scr);
    lv_obj_set_size(mem_bar, 180, 8);
    lv_obj_set_pos(mem_bar, 44, 156);
    lv_obj_set_style_radius(mem_bar, 5, 0);
    lv_obj_set_style_radius(mem_bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(mem_bar, lv_color_hex(0xE5E7EB), LV_PART_MAIN);
    lv_obj_set_style_bg_color(mem_bar, lv_color_hex(0x10B981), LV_PART_INDICATOR);
    lv_bar_set_range(mem_bar, 0, 100);
    lv_bar_set_value(mem_bar, 0, LV_ANIM_OFF);

    mem_label = lv_label_create(main_scr);
    lv_label_set_text(mem_label, "--");
    lv_obj_set_style_text_font(mem_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(mem_label, lv_color_hex(C_TEXT2), 0);
    lv_obj_set_pos(mem_label, 228, 152);

    /* 应用当前主题（从 NVS 加载） */
    s_theme_idx = display_config_load_theme();
    if (s_theme_idx < 0 || s_theme_idx >= (int)UI_THEME_COUNT) s_theme_idx = 0;
    ui_apply_theme(s_theme_idx);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI built: %s/%s, %d rows", s_cfg.titles[0], s_cfg.titles[1], s_cfg.row_count);
}

/* 应用主题：改背景、主/次文字色（CPU蓝 GPU橙强调色保留） */
void ui_apply_theme(int idx)
{
    if (idx < 0 || idx >= (int)UI_THEME_COUNT) idx = 0;
    s_theme_idx = idx;
    const ui_theme_t *t = &s_themes[idx];

    if (main_scr) {
        lv_obj_set_style_bg_color(main_scr, lv_color_hex(t->bg), 0);
    }
    /* 行标签、电池百分比、连接图标→次文字色 */
    for (int r = 0; r < s_cfg.row_count; r++) {
        if (row_labels[r])
            lv_obj_set_style_text_color(row_labels[r], lv_color_hex(t->text2), 0);
        /* 值→主文字色 */
        for (int c = 0; c < 2; c++)
            if (val_labels[r][c])
                lv_obj_set_style_text_color(val_labels[r][c], lv_color_hex(t->text), 0);
    }
    if (conn_label)
        lv_obj_set_style_text_color(conn_label, lv_color_hex(t->text2), 0);
    /* 电池外框/正极头→次文字色 */
    if (batt_body)
        lv_obj_set_style_border_color(batt_body, lv_color_hex(t->text2), 0);
    if (batt_tip)
        lv_obj_set_style_bg_color(batt_tip, lv_color_hex(t->text2), 0);
    /* 内存进度条跟随主题 */
    if (mem_bar)
        lv_obj_set_style_bg_color(mem_bar, lv_color_hex(t->text2), LV_PART_MAIN);
    if (mem_label)
        lv_obj_set_style_text_color(mem_label, lv_color_hex(t->text2), 0);
    if (mem_title)
        lv_obj_set_style_text_color(mem_title, lv_color_hex(t->text2), 0);
    /* CPU/GPU 标题保持蓝/橙强调色，不随主题变 */
}

/* 循环切换主题并保存（KEY 单击触发） */
void ui_cycle_theme(void)
{
    lvgl_port_lock(0);
    int next = (s_theme_idx + 1) % (int)UI_THEME_COUNT;
    ui_apply_theme(next);
    lvgl_port_unlock();
    display_config_save_theme(next);
}

/* CountUp 数字递增动画 */
static int s_cur_val[MAX_ROWS][2] = {0};
static char s_units[MAX_ROWS][2][8] = {{{0}}};
static bool s_first_val[MAX_ROWS][2] = {{true}};

static void countup_cb(void *obj, int32_t val)
{
    for (int r = 0; r < MAX_ROWS; r++) {
        for (int c = 0; c < 2; c++) {
            if (val_labels[r][c] == obj) {
                s_cur_val[r][c] = val;
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld%s", (long)val, s_units[r][c]);
                lv_label_set_text(obj, buf);
                return;
            }
        }
    }
}

void ui_update_value(int row, int col, const char *value)
{
    if (row < 0 || row >= s_cfg.row_count || col < 0 || col >= 2) return;
    if (!val_labels[row][col]) return;

    /* 解析数值和单位："45W" -> 45 + "W" */
    int new_val = 0;
    char unit[8] = {0};
    bool is_numeric = false;
    const char *p = value;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end != p) {
            new_val = (int)v;
            strncpy(unit, end, sizeof(unit) - 1);
            is_numeric = true;
        }
    }

    lvgl_port_lock(0);

    if (!is_numeric) {
        /* 非数字（如时间）直接设置 */
        lv_label_set_text(val_labels[row][col], value);
        lvgl_port_unlock();
        return;
    }

    strncpy(s_units[row][col], unit, sizeof(s_units[row][col]) - 1);

    int from = s_cur_val[row][col];
    if (s_first_val[row][col] || from == new_val) {
        /* 首次或值没变，直接设置不动画 */
        s_first_val[row][col] = false;
        s_cur_val[row][col] = new_val;
        lv_label_set_text(val_labels[row][col], value);
        lvgl_port_unlock();
        return;
    }

    /* CountUp 动画：from -> new_val，500ms easeOutCubic */
    lv_anim_del(val_labels[row][col], countup_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, val_labels[row][col]);
    lv_anim_set_values(&a, from, new_val);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(&a, countup_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lvgl_port_unlock();
}

void ui_update_battery(int percent)
{
    if (!battery_label) return;
    lvgl_port_lock(0);
    if (percent >= 0) {
        if (percent > 100) percent = 100;
        lv_label_set_text_fmt(battery_label, "%d%%", percent);
        /* 填充条宽度：按百分比映射到可用内部宽度 */
        int inner_w = BATT_W - 2 * BATT_PAD - 2;   /* 可用填充宽度 */
        int fill_w = (percent * inner_w + 50) / 100;
        if (fill_w < 1 && percent > 0) fill_w = 1;
        lv_obj_set_width(batt_fill, fill_w);
        /* 颜色：<20% 红，<50% 黄，否则绿 */
        lv_color_t c;
        if (percent < 20)      c = lv_color_hex(0xEF4444);
        else if (percent < 50) c = lv_color_hex(0xF59E0B);
        else                   c = lv_color_hex(C_BAT);
        lv_obj_set_style_bg_color(batt_fill, c, 0);
        lv_obj_set_style_text_color(battery_label, c, 0);
    } else {
        lv_label_set_text(battery_label, "--%");
        lv_obj_set_width(batt_fill, 0);
        lv_obj_set_style_text_color(battery_label, lv_color_hex(C_TEXT2), 0);
    }
    lvgl_port_unlock();
}

void ui_set_connected(bool connected)
{
    if (!conn_label) return;
    lvgl_port_lock(0);
    lv_obj_set_style_text_color(conn_label,
        connected ? lv_color_hex(C_BAT) : lv_color_hex(C_TEXT2), 0);
    lvgl_port_unlock();
}

void ui_update_mem(const char *text, int percent)
{
    if (!mem_bar) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    lvgl_port_lock(0);
    lv_bar_set_value(mem_bar, percent, LV_ANIM_ON);
    /* 进度条颜色随百分比变化：<50% 青蓝，50-80% 紫，>80% 粉红 */
    lv_color_t bar_col;
    if (percent < 50)      bar_col = lv_color_hex(0x06B6D4);  /* 青蓝 */
    else if (percent < 80) bar_col = lv_color_hex(0x8B5CF6);  /* 紫 */
    else                   bar_col = lv_color_hex(0xEC4899);  /* 粉红 */
    lv_obj_set_style_bg_color(mem_bar, bar_col, LV_PART_INDICATOR);
    if (mem_label && text) {
        lv_label_set_text(mem_label, text);
    }
    lvgl_port_unlock();
}
/* 充电器页面状态变量 */
static bool s_on_charger = false;
static float s_prev_total_power = 0.0f;
static uint64_t s_inactive_since = 0;
static bool s_manual_charger = false;


/* 更新充电器功率显示 */
void ui_update_charger(float total_power, bool connected)
{
    /* 简单记录，跳转逻辑在 app_task 主循环中处理 */
    (void)total_power;
    (void)connected;
}

/* 在 LCD 上显示配网界面 */
void ui_show_config_screen(void)
{
    ESP_LOGI(TAG, "ui_show_config_screen called");
    lvgl_port_lock(portMAX_DELAY);
    ESP_LOGI(TAG, "lock acquired, building screen");
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1d21), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);

    /* 标题 - 居中 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "\xe9\x85\x8d\xe7\xbd\x91\xe6\xa8\xa1\xe5\xbc\x8f"); /* 配网模式 */
    lv_obj_set_style_text_color(title, lv_color_hex(0x00ff88), 0);
    lv_obj_set_style_text_font(title, &font_cn_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* 热点名称 */
    /* 热点名称 - 中文标签 + 英文值 */
    lv_obj_t *ap_lb = lv_label_create(scr);
    lv_label_set_text(ap_lb, "热点:");
    lv_obj_set_style_text_color(ap_lb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ap_lb, &font_cn_18, 0);
    lv_obj_align(ap_lb, LV_ALIGN_TOP_LEFT, 10, 55);

    lv_obj_t *ap_val = lv_label_create(scr);
    lv_label_set_text(ap_val, "SmartMonitor");
    lv_obj_set_style_text_color(ap_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ap_val, &font_cn_18, 0);
    lv_obj_align_to(ap_val, ap_lb, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* 地址 - 中文标签 */
    lv_obj_t *url_lb = lv_label_create(scr);
    lv_label_set_text(url_lb, "地址:");
    lv_obj_set_style_text_color(url_lb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(url_lb, &font_cn_18, 0);
    lv_obj_align(url_lb, LV_ALIGN_TOP_LEFT, 10, 80);

    lv_obj_t *url_val = lv_label_create(scr);
    lv_label_set_text(url_val, "192.168.4.1");
    lv_obj_set_style_text_color(url_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(url_val, &font_cn_18, 0);
    lv_obj_align_to(url_val, url_lb, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* 提示 */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "连接热点并配置设备"); /* 连接热点并配置设备 */
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6B7280), 0);
    lv_obj_set_style_text_font(hint, &font_cn_18, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_scr_load(scr);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "config screen displayed");
}

/* ==================== 充电器详情页 ==================== */
static lv_obj_t *charger_scr = NULL;
static lv_obj_t *ch_total_label = NULL;
static lv_obj_t *ch_port_labels[4][4]; /* [port][0=name,1=power,2=V,3=A] */
static lv_obj_t *ch_port_bars[4];
static lv_obj_t *ch_port_bgs[4];   /* 区块背景 */
static lv_obj_t *ch_port_hdrs[4];  /* 标题背景 */
static lv_obj_t *ch_port_glows[4]; /* 玻璃高光 */
static lv_obj_t *ch_port_pwu[4];   /* W 单位 */
static lv_obj_t *ch_bt_icon = NULL;  /* 蓝牙图标 */
static lv_obj_t *ch_c3_border_l = NULL;  /* C3合并时左侧蓝色边框装饰 */
static lv_obj_t *ch_c3_border_r = NULL;  /* C3合并时右侧橙色边框装饰 */
static uint32_t s_port_mask = 0xFF; /* 端口控制掩码, bit0=C1..bit3=A, 1=启用 */

/* 端口配色 (正常状态) */
static const uint32_t port_hdr_col[4]  = {0xFF7F24, 0x40B5FA, 0x00AEEF, 0xE6B800};
static const uint32_t port_grad_top[4] = {0x1A0A00, 0x001A3A, 0x001A30, 0x1A1A00};
static const uint32_t port_title_col[4] = {0xFFFFFF, 0x000000, 0x000000, 0x000000};
static int32_t ch_cur_power[4] = {0,0,0,0};  /* 端口控制掩码, bit0=C1, bit1=C2, bit2=C3, bit3=A */  /* 用于数字动画 (单位: 0.1W) */
static int32_t ch_cur_total = 0;


static void ui_init_charger_screen(void)
{
    if (charger_scr) return;  /* 防止重复初始化 */
    charger_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(charger_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(charger_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(charger_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(charger_scr, 0, 0);
    lv_obj_set_style_border_width(charger_scr, 0, 0);

    int col_w = 102;
    int row_h = 82;
    int gap = 4;
    uint32_t white = 0xFFFFFF;
    uint32_t dark = 0x000000;

    /* === 上排左半： 总功率 (占 2 格， 无边框) === */
    lv_obj_t *tp_bg = lv_obj_create(charger_scr);
    lv_obj_set_size(tp_bg, col_w * 2 + gap, row_h);
    lv_obj_set_pos(tp_bg, 2, 2);
    lv_obj_set_style_radius(tp_bg, 10, 0);
    lv_obj_set_style_bg_color(tp_bg, lv_color_hex(dark), 0);
    lv_obj_set_style_bg_opa(tp_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tp_bg, 0, 0);
    lv_obj_set_style_pad_all(tp_bg, 0, 0);
    lv_obj_clear_flag(tp_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* 总功率数值: frick_cond_80 大号, 靠右, 白色 */
    ch_total_label = lv_label_create(tp_bg);
    lv_label_set_text(ch_total_label, "0.0");
    lv_obj_set_style_text_font(ch_total_label, &frick_cond_80, 0);
    lv_obj_set_style_text_color(ch_total_label, lv_color_hex(white), 0);
    lv_obj_align(ch_total_label, LV_ALIGN_RIGHT_MID, -28, 0);

    /* W 单位: 略大, 底部对齐数字 */
    lv_obj_t *tp_w = lv_label_create(tp_bg);
    lv_label_set_text(tp_w, "W");
    lv_obj_set_style_text_font(tp_w, &frick_22, 0);
    lv_obj_set_style_text_color(tp_w, lv_color_hex(white), 0);
    lv_obj_align_to(tp_w, ch_total_label, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, 0);

    /* 蓝牙图标: W 上方, 顶部与总功率数字顶部齐平 */
    ch_bt_icon = lv_img_create(tp_bg);
    lv_img_set_src(ch_bt_icon, ble_get_ready_flag() ? &bt_on : &bt_off);
    lv_obj_align_to(ch_bt_icon, ch_total_label, LV_ALIGN_OUT_RIGHT_TOP, 6, 2);

    /* === 4 个端口区域 === */
    int x0 = 2, x1 = 2 + col_w + gap, x2 = 2 + (col_w + gap) * 2;
    int y_top = 2, y_bot = 2 + row_h + gap;
    int px[4] = { x0, x1, x2, x2 };
    int py[4] = { y_bot, y_bot, y_bot, y_top };
    static const char *port_names[4] = {"C1", "C2", "C3", "A"};

    for (int i = 0; i < 4; i++) {
        int cx = px[i];
        int cy = py[i];
        uint32_t hdr_col = port_hdr_col[i];
        uint32_t grad_top = port_grad_top[i];

        /* 区块背景 */
        lv_obj_t *bg = lv_obj_create(charger_scr);
        lv_obj_set_size(bg, col_w, row_h);
        lv_obj_set_pos(bg, cx, cy);
        lv_obj_set_style_radius(bg, 10, 0);
        lv_obj_set_style_bg_color(bg, lv_color_hex(grad_top), 0);
        lv_obj_set_style_bg_opa(bg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_grad_color(bg, lv_color_hex(hdr_col), 0);
        lv_obj_set_style_bg_grad_dir(bg, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(bg, lv_color_hex(hdr_col), 0);
        lv_obj_set_style_border_width(bg, 1, 0);
        lv_obj_set_style_pad_all(bg, 0, 0);
        lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
        ch_port_bgs[i] = bg;

        /* C3装饰边框: 用于合并模式时显示蓝橙渐变效果 */
        if (i == 2) {
            /* 左侧蓝色边框装饰 */
            ch_c3_border_l = lv_obj_create(bg);
            lv_obj_set_size(ch_c3_border_l, 3, row_h);
            lv_obj_set_pos(ch_c3_border_l, 0, 0);
            lv_obj_set_style_radius(ch_c3_border_l, 0, 0);
            lv_obj_set_style_bg_color(ch_c3_border_l, lv_color_hex(0x46B4FF), 0);
            lv_obj_set_style_bg_opa(ch_c3_border_l, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(ch_c3_border_l, 0, 0);
            lv_obj_set_style_pad_all(ch_c3_border_l, 0, 0);
            lv_obj_clear_flag(ch_c3_border_l, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(ch_c3_border_l, LV_OBJ_FLAG_HIDDEN);
            /* 右侧橙色边框装饰 */
            ch_c3_border_r = lv_obj_create(bg);
            lv_obj_set_size(ch_c3_border_r, 3, row_h);
            lv_obj_set_pos(ch_c3_border_r, col_w - 3, 0);
            lv_obj_set_style_radius(ch_c3_border_r, 0, 0);
            lv_obj_set_style_bg_color(ch_c3_border_r, lv_color_hex(0xFF7A00), 0);
            lv_obj_set_style_bg_opa(ch_c3_border_r, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(ch_c3_border_r, 0, 0);
            lv_obj_set_style_pad_all(ch_c3_border_r, 0, 0);
            lv_obj_clear_flag(ch_c3_border_r, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(ch_c3_border_r, LV_OBJ_FLAG_HIDDEN);
        }

        /* 玻璃高光: 顶部半透明白色横向亮带 */
        lv_obj_t *glow = lv_obj_create(bg);
        lv_obj_set_size(glow, col_w - 6, 14);
        lv_obj_align(glow, LV_ALIGN_TOP_MID, 0, 2);
        lv_obj_set_style_radius(glow, 8, 0);
        lv_obj_set_style_bg_color(glow, lv_color_hex(white), 0);
        lv_obj_set_style_bg_opa(glow, LV_OPA_10, 0);
        lv_obj_set_style_border_width(glow, 0, 0);
        lv_obj_clear_flag(glow, LV_OBJ_FLAG_SCROLLABLE);
        ch_port_glows[i] = glow;

        /* 标题背景（满宽, 圆角保留） */
        int hdr_w = col_w;
        lv_obj_t *hdr = lv_obj_create(bg);
        lv_obj_set_size(hdr, hdr_w, 18);
        lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_radius(hdr, 4, 0);
        lv_obj_set_style_bg_color(hdr, lv_color_hex(hdr_col), 0);
        lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(hdr, 0, 0);
        lv_obj_set_style_pad_all(hdr, 0, 0);
        lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
        ch_port_hdrs[i] = hdr;

        /* 标题文字: 纯白 */
        lv_obj_t *nl = lv_label_create(hdr);
        lv_label_set_text(nl, port_names[i]);
        lv_obj_set_style_text_font(nl, &frick_14, 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(port_title_col[i]), 0);
        lv_obj_center(nl);
        ch_port_labels[i][0] = nl;

        /* 功率数字 (中间, 大号, 纯白) */
        lv_obj_t *pw_num = lv_label_create(bg);
        lv_label_set_text(pw_num, "0.0");
        lv_obj_set_style_text_font(pw_num, &frick_32, 0);
        lv_obj_set_style_text_color(pw_num, lv_color_hex(0xAAAAAA), 0);
        lv_obj_align(pw_num, LV_ALIGN_LEFT_MID, 6, -4);
        ch_port_labels[i][1] = pw_num;

        /* W 单位 (功率右侧, 纯白) */
        lv_obj_t *pw_u = lv_label_create(bg);
        lv_label_set_text(pw_u, "W");
        lv_obj_set_style_text_font(pw_u, &frick_14, 0);
        lv_obj_set_style_text_color(pw_u, lv_color_hex(0xAAAAAA), 0);
        lv_obj_align(pw_u, LV_ALIGN_RIGHT_MID, -6, -4);
        ch_port_pwu[i] = pw_u;

        /* V (左下角) 纯白 */
        lv_obj_t *vv = lv_label_create(bg);
        lv_label_set_text(vv, "00.0V");
        lv_obj_set_style_text_font(vv, &frick_14, 0);
        lv_obj_set_style_text_color(vv, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_pos(vv, 4, row_h - 16);
        ch_port_labels[i][2] = vv;

        /* A (右下角) 纯白 */
        lv_obj_t *aa = lv_label_create(bg);
        lv_label_set_text(aa, "00.0A");
        lv_obj_set_style_text_font(aa, &frick_14, 0);
        lv_obj_set_style_text_color(aa, lv_color_hex(0xAAAAAA), 0);
        lv_obj_align(aa, LV_ALIGN_BOTTOM_RIGHT, -4, -2);
        ch_port_labels[i][3] = aa;

        /* 进度条: 放在功率/W下方, 左对齐到数字右对齐到W */
        lv_obj_t *bar = lv_bar_create(bg);
        lv_obj_set_size(bar, col_w - 12, 4);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_bar_set_range(bar, 0, 12000);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        /* 指示器: 标题色 */
        lv_obj_set_style_bg_color(bar, lv_color_hex(hdr_col), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
        /* 放在 pw_num 下方, 左对齐 */
        lv_obj_align_to(bar, pw_num, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        ch_port_bars[i] = bar;
    }

    /* 初始化后立即应用端口控制掩码 */
    uint32_t mask = s_port_mask;
    for (int i = 0; i < 4; i++) {
        if (!((mask >> i) & 1)) {
            lv_obj_set_style_bg_color(ch_port_bgs[i], lv_color_hex(0x1a1a1a), 0);
            lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_border_color(ch_port_bgs[i], lv_color_hex(0x555555), 0);
            lv_obj_set_style_border_width(ch_port_bgs[i], 1, 0);
            lv_obj_set_style_bg_color(ch_port_hdrs[i], lv_color_hex(0x333333), 0);
            lv_obj_set_style_border_color(ch_port_hdrs[i], lv_color_hex(0x666666), 0);
            lv_obj_set_style_border_width(ch_port_hdrs[i], 1, 0);
            lv_obj_add_flag(ch_port_glows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(ch_port_labels[i][0], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][1], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][2], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][3], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_pwu[i], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_font(ch_port_labels[i][1], &lv_font_montserrat_32, 0);
            lv_label_set_text(ch_port_labels[i][1], "--");
            lv_label_set_text(ch_port_labels[i][2], "--V");
            lv_label_set_text(ch_port_labels[i][3], "--A");
            lv_obj_add_flag(ch_port_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ch_countup_cb(void *obj, int32_t val) {
    for (int i = 0; i < 4; i++) {
        if (ch_port_labels[i][1] == obj) {
            ch_cur_power[i] = val;
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f", val / 10.0f);
            lv_label_set_text(obj, buf);
            return;
        }
    }
    if (ch_total_label == obj) {
        ch_cur_total = val;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", val / 10.0f);
        lv_label_set_text(obj, buf);
    }
}

void ui_update_charger_detail(float powers[], float voltages[], float currents[], const char *protocols[], bool actives[], bool enabled[], uint8_t status_raw[])
{
    /* 如果有数据但充电页面还没创建，先初始化 */
    bool need_init = false;
    for (int i = 0; i < 4 && !need_init; i++) {
        if (actives[i] && powers[i] > 0.05f) need_init = true;
    }
    if (!charger_scr && (s_on_charger || need_init)) {
        ui_init_charger_screen();
    }
    if (!charger_scr) {
        /* 无数据且不在充电页面，只记录总功率避免后续误触发 */
        return;
    }
    /* 用 try_lock(10ms) 避免与 LVGL 任务死锁导致 WDT 触发 */
    if (!lvgl_port_lock(10)) {
        return; /* 10ms内拿不到锁则跳过本次更新 */
    }

    /* 计算总功率，检查是否有活跃端口 */
    float total = 0;
    bool any_active = false;
    bool all_inactive = true;
    for (int i = 0; i < 4; i++) {
        if (actives[i] && powers[i] > 0.05f) {
            any_active = true;
            all_inactive = false;
            total += powers[i];
        } else if (actives[i]) {
            all_inactive = false;
        }
    }

    /* 自动切换：
     * 有端口从 false→true（有数据了）→ 跳充电页
     * 所有端口 active=false → 跳回 AIDA64（除非手动进入充电页）
     */
    if (!s_on_charger) {
        if (any_active && s_prev_total_power < 0.01f) {
            s_on_charger = true;
            s_manual_charger = false;  /* 自动跳转，非手动 */
            if (!charger_scr) ui_init_charger_screen();
            lv_scr_load(charger_scr);
            /* 不 return，继续执行下面的数据更新，确保首次数据能正确显示 */
        }
    } else {
        if (!any_active && all_inactive) {
            if (s_prev_total_power > 0.01f) {
                /* 数据刚变为全零：记录开始时间 */
                s_inactive_since = esp_timer_get_time() / 1000;
                s_prev_total_power = 0.0f;
            }
        } else {
            /* 又有数据了，取消计时 */
            s_inactive_since = 0;
        }
    }
    /* 只在非跳转场景更新 s_prev_total_power，防止跳转后错误触发 */
    s_prev_total_power = any_active ? total : 0.0f;

    if (!s_on_charger) {
        lvgl_port_unlock();
        return;
    }

    total = 0;
    for (int i = 0; i < 4; i++) {
        if (actives[i]) total += powers[i];
        if (!ch_port_labels[i][1] || !ch_port_labels[i][2] || !ch_port_labels[i][3]) continue;

        if (!enabled[i]) {
            /* === 禁用状态: 深色背景, 灰色虚线边框, 灰色文字, -- 数据 === */
            /* 背景: 深色, 无渐变 */
            lv_obj_set_style_bg_color(ch_port_bgs[i], lv_color_hex(0x1a1a1a), 0);
            lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_border_color(ch_port_bgs[i], lv_color_hex(0x555555), 0);
            lv_obj_set_style_border_width(ch_port_bgs[i], 1, 0);
            lv_obj_set_style_border_side(ch_port_bgs[i], LV_BORDER_SIDE_FULL, 0);
            lv_obj_set_style_border_post(ch_port_bgs[i], true, 0);
            /* 标题背景: 深灰色 */
            lv_obj_set_style_bg_color(ch_port_hdrs[i], lv_color_hex(0x333333), 0);
            lv_obj_set_style_border_color(ch_port_hdrs[i], lv_color_hex(0x666666), 0);
            lv_obj_set_style_border_width(ch_port_hdrs[i], 1, 0);
            lv_obj_set_style_border_side(ch_port_hdrs[i], LV_BORDER_SIDE_FULL, 0);
            /* 隐藏高光 */
            lv_obj_add_flag(ch_port_glows[i], LV_OBJ_FLAG_HIDDEN);
            /* 文字: 全部浅灰色 */
            lv_obj_set_style_text_color(ch_port_labels[i][0], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][1], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][2], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][3], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_pwu[i], lv_color_hex(0xAAAAAA), 0);
            /* 数据: -- (切换到 montserrat_28 因为 frick_32 不支持'-') */
            lv_obj_set_style_text_font(ch_port_labels[i][1], &lv_font_montserrat_32, 0);
            lv_label_set_text(ch_port_labels[i][1], "--");
            lv_label_set_text(ch_port_labels[i][2], "--V");
            lv_label_set_text(ch_port_labels[i][3], "--A");
            /* 隐藏进度条 */
            lv_obj_add_flag(ch_port_bars[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        /* === 正常状态: 恢复配色 === */
        lv_obj_set_style_bg_color(ch_port_bgs[i], lv_color_hex(port_grad_top[i]), 0);
        lv_obj_set_style_bg_grad_color(ch_port_bgs[i], lv_color_hex(port_hdr_col[i]), 0);
        lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_color(ch_port_bgs[i], lv_color_hex(port_hdr_col[i]), 0);
        lv_obj_set_style_border_width(ch_port_bgs[i], 1, 0);
        /* 标题背景: 恢复颜色 */
        lv_obj_set_style_bg_color(ch_port_hdrs[i], lv_color_hex(port_hdr_col[i]), 0);
        lv_obj_set_style_border_width(ch_port_hdrs[i], 0, 0);
        /* 显示高光 */
        lv_obj_clear_flag(ch_port_glows[i], LV_OBJ_FLAG_HIDDEN);
        /* 标题文字颜色恢复 */
        lv_obj_set_style_text_color(ch_port_labels[i][0], lv_color_hex(port_title_col[i]), 0);
        lv_obj_set_style_text_color(ch_port_pwu[i],
            actives[i] ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xAAAAAA), 0);
        /* 强制恢复功率显示初始值 */
        lv_label_set_text(ch_port_labels[i][1], "0.0");
        ch_cur_power[i] = 0;
        lv_obj_set_style_text_font(ch_port_labels[i][1], &frick_32, 0);

        /* C3+A 合并检测: C3(status_raw==0x11)时隐藏A口，C3显示合并标签 */
        bool c3a_merged = (i == 2 && status_raw[2] == 0x11);  /* i=2=C3 */
        if (i == 3) {
            if (c3a_merged || status_raw[2] == 0x11) {
                /* A口被合并 → 完全融入背景 */
                lv_obj_set_style_bg_color(ch_port_bgs[i], lv_color_hex(0x000000), 0);
                lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_NONE, 0);
                lv_obj_set_style_border_color(ch_port_bgs[i], lv_color_hex(0x000000), 0);
                lv_obj_set_style_border_width(ch_port_bgs[i], 0, 0);
                lv_obj_set_style_bg_color(ch_port_hdrs[i], lv_color_hex(0x000000), 0);
                lv_obj_set_style_border_width(ch_port_hdrs[i], 0, 0);
                lv_obj_add_flag(ch_port_glows[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ch_port_bars[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ch_port_pwu[i], LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(ch_port_labels[i][0], "");
                lv_label_set_text(ch_port_labels[i][1], "");
                lv_label_set_text(ch_port_labels[i][2], "");
                lv_label_set_text(ch_port_labels[i][3], "");
                continue;
            } else {
                /* 恢复A口 */
                lv_obj_set_style_bg_color(ch_port_bgs[i], lv_color_hex(port_hdr_col[i]), 0);
                lv_obj_set_style_bg_grad_color(ch_port_bgs[i], lv_color_hex(port_grad_top[i]), 0);
                lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_VER, 0);
                lv_obj_set_style_border_color(ch_port_bgs[i], lv_color_hex(port_hdr_col[i]), 0);
                lv_obj_set_style_border_width(ch_port_bgs[i], 1, 0);
                lv_obj_set_style_bg_color(ch_port_hdrs[i], lv_color_hex(port_hdr_col[i]), 0);
                lv_obj_set_style_border_width(ch_port_hdrs[i], 0, 0);
                lv_obj_clear_flag(ch_port_glows[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ch_port_bars[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ch_port_pwu[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(ch_port_labels[i][0], lv_color_hex(port_title_col[i]), 0);
                lv_label_set_text(ch_port_labels[i][0], "A");
            }
        }
        if (c3a_merged) {
            /* C3+A合并模式：去掉背景填充，只保留一个完整方框 */
            lv_obj_set_style_bg_color(ch_port_bgs[i], lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_bg_grad_color(ch_port_bgs[i], lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(ch_port_bgs[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(ch_port_bgs[i], lv_color_hex(0x46B4FF), 0);
            lv_obj_set_style_border_width(ch_port_bgs[i], 2, 0);
            lv_obj_set_style_border_side(ch_port_bgs[i], LV_BORDER_SIDE_FULL, 0);
            lv_obj_set_style_border_post(ch_port_bgs[i], true, 0);
            /* 隐藏装饰 */
            if (ch_c3_border_l) lv_obj_add_flag(ch_c3_border_l, LV_OBJ_FLAG_HIDDEN);
            if (ch_c3_border_r) lv_obj_add_flag(ch_c3_border_r, LV_OBJ_FLAG_HIDDEN);
            /* 标题背景去掉 */
            lv_obj_set_style_bg_color(ch_port_hdrs[i], lv_color_hex(0x000000), 0);
            lv_obj_set_style_border_width(ch_port_hdrs[i], 0, 0);
            /* 隐藏高光 */
            lv_obj_add_flag(ch_port_glows[i], LV_OBJ_FLAG_HIDDEN);
            /* 标题文字浅蓝色 */
            lv_obj_set_style_text_color(ch_port_labels[i][0], lv_color_hex(0x89d8f3), 0);
            lv_label_set_text(ch_port_labels[i][0], "C3 & A");
        } else if (i == 2) {
            /* 合并结束 → 恢复C3正常样式 */
            lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_VER, 0);
            lv_obj_set_style_bg_grad_color(ch_port_bgs[i], lv_color_hex(port_hdr_col[i]), 0);
            lv_label_set_text(ch_port_labels[i][0], "C3");
            if (ch_c3_border_l) lv_obj_add_flag(ch_c3_border_l, LV_OBJ_FLAG_HIDDEN);
            if (ch_c3_border_r) lv_obj_add_flag(ch_c3_border_r, LV_OBJ_FLAG_HIDDEN);
        }

        /* 功率 - 一位小数动画 */
        lv_obj_set_style_text_font(ch_port_labels[i][1], &frick_32, 0);
        int32_t new_pw = (int32_t)(powers[i] * 10.0f + 0.5f);
        if (new_pw != ch_cur_power[i]) {
            if (new_pw == 0) {
                lv_label_set_text(ch_port_labels[i][1], "0.0");
                ch_cur_power[i] = 0;
            } else if (ch_cur_power[i] == 0) {
                /* 从 0 到有值：直接显示，不动画 */
                char buf2[16];
                snprintf(buf2, sizeof(buf2), "%d.%d", (int)(new_pw / 10), (int)(new_pw % 10));
                lv_label_set_text(ch_port_labels[i][1], buf2);
                ch_cur_power[i] = new_pw;
            } else {
                /* 正常变化 → 动画 */
                lv_anim_del(ch_port_labels[i][1], ch_countup_cb);
                lv_anim_t a;
                lv_anim_init(&a);
                lv_anim_set_var(&a, ch_port_labels[i][1]);
                lv_anim_set_values(&a, ch_cur_power[i], new_pw);
                lv_anim_set_time(&a, 400);
                lv_anim_set_exec_cb(&a, ch_countup_cb);
                lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
                lv_anim_start(&a);
                ch_cur_power[i] = new_pw;
            }
        }

        char buf[32];
        /* 电压 - 有数据显示实际值, 无数据显示 00.0 */
        if (voltages[i] > 0.1f)
            snprintf(buf, sizeof(buf), "%.1fV", voltages[i]);
        else
            snprintf(buf, sizeof(buf), "00.0V");
        lv_label_set_text(ch_port_labels[i][2], buf);

        /* 电流 - 有数据显示实际值, 无数据显示 00.0 */
        if (currents[i] > 0.01f)
            snprintf(buf, sizeof(buf), "%.1fA", currents[i]);
        else
            snprintf(buf, sizeof(buf), "00.0A");
        lv_label_set_text(ch_port_labels[i][3], buf);

        /* 活跃端口高亮功率 */
        lv_obj_set_style_text_color(ch_port_labels[i][1],
            actives[i] ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_color(ch_port_labels[i][2],
            actives[i] ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_color(ch_port_labels[i][3],
            actives[i] ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xAAAAAA), 0);
    }

    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "%.1f", total);
    int32_t new_total = (int32_t)(total * 10.0f + 0.5f);
    if (new_total != ch_cur_total) {
        lv_anim_del(ch_total_label, ch_countup_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ch_total_label);
        lv_anim_set_values(&a, ch_cur_total, new_total);
        lv_anim_set_time(&a, 500);
        lv_anim_set_exec_cb(&a, ch_countup_cb);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }

    /* 更新进度条 (只显示有输出且启用的端口) */
    for (int i = 0; i < 4; i++) {
        if (ch_port_bars[i]) {
            if (enabled[i] && powers[i] > 0.05f) {
                lv_obj_clear_flag(ch_port_bars[i], LV_OBJ_FLAG_HIDDEN);
                int val = (int)(powers[i] * 100.0f);
                if (val > 12000) val = 12000;
                if (val < 0) val = 0;
                lv_bar_set_value(ch_port_bars[i], val, LV_ANIM_OFF);
            } else {
                lv_obj_add_flag(ch_port_bars[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    lvgl_port_unlock();
}
void ui_switch_to_charger(bool charger)
{
    s_on_charger = charger;
    s_manual_charger = charger;  /* 手动进入/离开，标记为手动模式 */
    lvgl_port_lock(0);
    if (charger) {
        if (!charger_scr) ui_init_charger_screen();
        lv_scr_load(charger_scr);
    } else {
        lv_scr_load(main_scr);
    }
    lvgl_port_unlock();
}

bool ui_is_charger_screen(void)
{
    return s_on_charger;
}

bool ui_is_manual_charger(void)
{
    return s_manual_charger;
}

void ui_clear_inactive_timer(void)
{
    s_inactive_since = 0;
}

/* 设置蓝牙连接状态图标 */
void ui_set_bt_connected(bool connected)
{
    if (!ch_bt_icon) return;
    lvgl_port_lock(0);
    if (connected) {
        lv_img_set_src(ch_bt_icon, &bt_on);
    } else {
        lv_img_set_src(ch_bt_icon, &bt_off);
    }
    lvgl_port_unlock();
}

/* 设置端口控制掩码, 立即更新禁用样式 (无需等 BLE 数据) */
void ui_set_port_mask(uint32_t mask)
{
    s_port_mask = mask;
    if (!charger_scr) return;
    lvgl_port_lock(0);
    for (int i = 0; i < 4; i++) {
        bool en = (mask >> i) & 1;
        if (!en) {
            /* 禁用: 深色背景, 灰色文字, -- 数据 */
            lv_obj_set_style_bg_color(ch_port_bgs[i], lv_color_hex(0x1a1a1a), 0);
            lv_obj_set_style_bg_grad_dir(ch_port_bgs[i], LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_border_color(ch_port_bgs[i], lv_color_hex(0x555555), 0);
            lv_obj_set_style_border_width(ch_port_bgs[i], 1, 0);
            lv_obj_set_style_bg_color(ch_port_hdrs[i], lv_color_hex(0x333333), 0);
            lv_obj_set_style_border_color(ch_port_hdrs[i], lv_color_hex(0x666666), 0);
            lv_obj_set_style_border_width(ch_port_hdrs[i], 1, 0);
            lv_obj_add_flag(ch_port_glows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(ch_port_labels[i][0], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][1], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][2], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_labels[i][3], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_color(ch_port_pwu[i], lv_color_hex(0xAAAAAA), 0);
            lv_obj_set_style_text_font(ch_port_labels[i][1], &lv_font_montserrat_32, 0);
            lv_label_set_text(ch_port_labels[i][1], "--");
            lv_label_set_text(ch_port_labels[i][2], "--V");
            lv_label_set_text(ch_port_labels[i][3], "--A");
            lv_obj_add_flag(ch_port_bars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lvgl_port_unlock();
}
