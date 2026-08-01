#ifndef LV_PORT_H
#define LV_PORT_H

#include "esp_lcd_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"   /* esp_lvgl_port 提供 lvgl_port_lock/unlock + LVGL 任务管理 */

/* 横屏 320x170 */
#define LCD_WIDTH  320
#define LCD_HEIGHT 170

/* 初始化 LCD + LVGL（内部用 esp_lvgl_port 组件管理任务/锁/buffer/旋转） */
void lv_port_init(esp_lcd_panel_handle_t panel_handle);

/* lvgl_port_lock / lvgl_port_unlock 由 esp_lvgl_port 提供，无需自己声明。
 * 用法：lvgl_port_lock(0) = 永久等待；lvgl_port_lock(100) = 等 100ms。
 * 在非 LVGL 任务（如 AIDA64 SSE 回调）里修改 UI 对象时必须加锁。 */

#endif
