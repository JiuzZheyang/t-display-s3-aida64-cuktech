#include "lv_port.h"
#include "lcd_i80.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "lv_port";

/*
 * 使用 esp_lvgl_port 组件管理 LVGL（参考 xayzyt/ESP32-S3-AIDA64-Monitor）
 *
 * esp_lvgl_port 自动处理：
 *   - LVGL 任务（绑定到核心 1）
 *   - 互斥锁（lvgl_port_lock / lvgl_port_unlock）
 *   - 显示 buffer 分配（DMA 内存）
 *   - flush 回调（自动调用 esp_lcd_panel_draw_bitmap）
 *   - swap_xy / mirror 的坐标映射（关键！之前手动实现导致半屏）
 */

void lv_port_init(esp_lcd_panel_handle_t panel_handle)
{
    /* 1. LVGL 任务配置 */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority     = 4,
        .task_stack        = 8192,
        .task_affinity     = 1,   /* 核心 1 */
        .task_max_sleep_ms = 500,
        .timer_period_ms   = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* 2. 显示注册
     *    io_handle 和 panel_handle 都必须提供（esp_lvgl_port 内部用 io_handle 发命令）
     *    rotation.swap_xy/mirror 必须和 lcd_i80.c 里的 panel 配置一致 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle   = bsp_lcd_get_io_handle(),
        .panel_handle= panel_handle,
        .buffer_size = LCD_WIDTH * 80,   /* 80 行 partial buffer（DMA） */
        .double_buffer  = false,
        .hres           = LCD_WIDTH,
        .vres           = LCD_HEIGHT,
        .rotation = {
            .swap_xy  = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    assert(disp);

    ESP_LOGI(TAG, "LVGL init done: %dx%d, swap_xy+mirror, 80-row partial",
             LCD_WIDTH, LCD_HEIGHT);
}
