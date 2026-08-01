#ifndef BSP_BACKLIGHT_H
#define BSP_BACKLIGHT_H

#include <stdint.h>

/*
 * 背光控制（LEDC PWM）
 *   - GPIO 38 (LCD_BL_IO) 改用 LEDC 通道输出 PWM，替代原来的 GPIO 直驱
 *   - 提供 5 档亮度：100% / 75% / 50% / 25% / 5%
 *   - 默认上电为 100%
 *   - backlight_cycle() 供按键调用：循环切换下一档
 */
void backlight_init(void);

/* 切到下一档亮度，返回当前亮度百分比 (5/25/50/75/100) */
int  backlight_cycle(void);

/* 直接设置亮度百分比 (1-100)，返回实际生效的百分比 */
int  backlight_set_percent(int percent);

/* 获取当前亮度百分比 */
int  backlight_get_percent(void);

#endif /* BSP_BACKLIGHT_H */
