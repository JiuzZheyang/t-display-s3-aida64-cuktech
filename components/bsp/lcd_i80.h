#ifndef BSP_LCD_I80_H
#define BSP_LCD_I80_H

#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"   // esp_lcd_new_panel_io_i80 / esp_lcd_i80_bus_handle_t
#include "esp_lcd_io_i80.h"     // esp_lcd_i80_bus_config_t

/* T-Display-S3 1.9" 170x320 ST7789 屏幕硬件（I80 8-bit 并行总线）
 * 引脚定义依据《T-Display-S3 屏幕显示信息》规格表。
 *
 *  注：本屏的 LCD 控制器只焊接了 I80 并行信号线（数据线 39~48），
 *      没有 SPI 走线 —— 之前的 SPI 驱动完全是错的。
 */
#define LCD_PWR_IO          15   // 电源使能
#define LCD_BL_IO           38   // 背光（高电平亮）
#define LCD_RST_IO          5    // 复位（低电平复位）
#define LCD_DC_IO           7    // 数据/命令
#define LCD_CS_IO           6    // 片选
#define LCD_WR_IO           8    // 写信号
#define LCD_RD_IO           9    // 读信号（始终拉高）
#define LCD_BOOT_IO         0
#define LCD_KEY_IO          14   // KEY (power/sleep)    // BOOT 按键

// 8-bit 数据线（D0~D7）
#define LCD_DATA0_IO        39
#define LCD_DATA1_IO        40
#define LCD_DATA2_IO        41
#define LCD_DATA3_IO        42
#define LCD_DATA4_IO        45
#define LCD_DATA5_IO        46
#define LCD_DATA6_IO        47
#define LCD_DATA7_IO        48

// 显示方向（横屏 320x170）
#define LCD_WIDTH           320  // 横向（USB-C 在上）
#define LCD_HEIGHT          170  // 纵向
#define LCD_X_GAP           0
#define LCD_Y_GAP           35   // 240x320 ST7789 RAM 中取中央 170 行：偏移 35 行

esp_lcd_panel_handle_t bsp_lcd_init(void);
esp_lcd_panel_io_handle_t bsp_lcd_get_io_handle(void);

#endif
