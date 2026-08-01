#ifndef BSP_BUTTON_H
#define BSP_BUTTON_H

#include <stdbool.h>

/*
 * BOOT 按键（GPIO 0，板载侧键 / BOOT 键）
 *   - 短按：循环切换背光亮度（backlight_cycle）
 *   - 默认上拉，按下时拉低
 *   - 10ms 轮询 + 20ms 软件去抖，可靠识别单击
 *
 * 长按、双击等更复杂的手势暂未实现（如有需要再扩展）。
 */
void button_init(void);

/* 按键回调：每次短按时被调用，参数为当前亮度百分比 (5/25/50/75/100)。
 * 由 backlight_cycle() 内部触发，UI 层可在此显示亮度 Toast。 */
typedef void (*button_brightness_cb_t)(int percent);

/* 注册亮度变化回调（可选；不注册则按键只调 backlight_cycle） */
void button_register_brightness_cb(button_brightness_cb_t cb);

/* 长按回调（持续按下 3 秒触发一次，用于清除配置重新配网） */
typedef void (*button_longpress_cb_t)(void);
void button_register_longpress_cb(button_longpress_cb_t cb);

/* KEY(GPIO14) 单击回调（短按触发，用于切换主题；长按仍为关机） */
typedef void (*button_keyclick_cb_t)(void);
void button_register_keyclick_cb(button_keyclick_cb_t cb);

#endif /* BSP_BUTTON_H */
