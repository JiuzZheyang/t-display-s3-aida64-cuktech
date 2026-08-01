#include "lcd_i80.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_io_i80.h"
#include "driver/gpio.h"
#include <string.h>

static const char *TAG = "lcd_i80";

/* 保存 io_handle 供外部获取（esp_lvgl_port 需要它） */
static esp_lcd_panel_io_handle_t s_io_handle = NULL;

esp_lcd_panel_io_handle_t bsp_lcd_get_io_handle(void)
{
    return s_io_handle;
}

/*
 * T-Display-S3 1.9" 170x320 ST7789 屏幕（I80 8-bit 并行总线）
 *
 * 参考 https://github.com/xayzyt/ESP32-S3-AIDA64-Monitor 的 LVGL + LCD 适配方式：
 *   - esp_lcd_new_i80_bus + esp_lcd_new_panel_io_i80 + esp_lcd_new_panel_st7789
 *   - swap_color_bytes = 1 放在 IO 配置的 .flags 里（不是 panel dev config 的 data_endian）
 *   - panel: swap_xy(true) + mirror(true, false) + invert_color(true) + set_gap(0, 35)
 *   - 不再做 4 色自检（已删掉），直接交给 LVGL 渲染
 */
esp_lcd_panel_handle_t bsp_lcd_init(void)
{
    /* 1. PWR + BL + RD */
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << LCD_PWR_IO) | (1ULL << LCD_BL_IO) | (1ULL << LCD_RD_IO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&gpio_cfg);
    gpio_set_level(LCD_PWR_IO, 1);
    gpio_set_level(LCD_RD_IO, 1);
    gpio_set_level(LCD_BL_IO,  0);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* 2. I80 总线 */
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num       = LCD_DC_IO,
        .wr_gpio_num       = LCD_WR_IO,
        .clk_src           = LCD_CLK_SRC_PLL160M,
        .data_gpio_nums    = {
            LCD_DATA0_IO, LCD_DATA1_IO, LCD_DATA2_IO, LCD_DATA3_IO,
            LCD_DATA4_IO, LCD_DATA5_IO, LCD_DATA6_IO, LCD_DATA7_IO,
        },
        .bus_width         = 8,
        .max_transfer_bytes = LCD_WIDTH * LCD_HEIGHT * 2 + 100,
        .psram_trans_align = 64,
        .sram_trans_align  = 4,
    };
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &i80_bus));

    /* 3. IO 设备 — swap_color_bytes=1 在这里设（参考项目的做法） */
    esp_lcd_panel_io_i80_config_t panel_io_cfg = {
        .cs_gpio_num     = LCD_CS_IO,
        .pclk_hz         = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
        .dc_levels       = { .dc_data_level = 1 },
        .flags           = { .swap_color_bytes = 1 },
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &panel_io_cfg, &io_handle));
    s_io_handle = io_handle;   /* 保存供 esp_lvgl_port 使用 */

    /* 4. ST7789 面板 */
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = LCD_RST_IO,
        .color_space    = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &dev_cfg, &panel_handle));

    /* 5. 复位 + init */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    /* 6. 横屏配置（与参考 demo 完全一致） */
    esp_lcd_panel_io_tx_param(io_handle, 0x36, (uint8_t[]){0x60}, 1);
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 35));

    /* 7. 开显示。背光不再由 GPIO 直驱，交给 backlight.c 的 LEDC PWM 接管：
     *    - 这里保持 BL=0（屏黑），由 backlight_init() 上电时点亮，
     *      配合开机动画的淡入效果，避免 init 期间闪白屏。
     *    - 若未调用 backlight_init()，BL 会一直为低 → 屏黑；因此
     *      main.c 必须在 bsp_lcd_init 之后调用 backlight_init()。 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_LOGI(TAG, "LCD init done: %dx%d (BL left to LEDC)", LCD_WIDTH, LCD_HEIGHT);

    return panel_handle;
}
