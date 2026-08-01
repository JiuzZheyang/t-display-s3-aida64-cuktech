#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "display_config.h"

void ui_init(void);
lv_obj_t *ui_get_main_screen(void);
void ui_show_brightness_toast(int percent);

/* 更新某个位置的显示值（字符串） */
void ui_update_value(int row, int col, const char *value);

/* 更新电池百分比 */
void ui_update_battery(int percent);

/* 更新连接状态 */
void ui_set_connected(bool connected);

/* 更新内存使用率进度条 */
void ui_update_mem(const char *text, int percent);

/* 更新充电器功率显示 */
void ui_update_charger(float total_power, bool connected);

/* 更新充电器详情页端口数据 */
void ui_update_charger_detail(float powers[], float voltages[], float currents[], const char *protocols[], bool actives[], bool enabled[], uint8_t status_raw[]);

/* 切换到充电器页面 / AIDA64 页面 */
void ui_switch_to_charger(bool charger);
bool ui_is_charger_screen(void);
bool ui_is_manual_charger(void);
void ui_clear_inactive_timer(void);
/* 当用户手动切回 AIDA64 时调用，锁定 AIDA64 页面不被自动跳转覆盖 */
void ui_lock_aida64(void);
/* 重置自动跳转锁定 */
void ui_unlock_aida64(void);
void ui_set_port_mask(uint32_t mask);
void ui_set_bt_connected(bool connected);

/* 获取当前显示配置 */
display_config_t *ui_get_config(void);

/* 在 LCD 上显示配网界面 */
void ui_show_config_screen(void);

/* 主题切换：应用指定主题 / 循环切换并保存 */
void ui_apply_theme(int idx);
void ui_cycle_theme(void);
