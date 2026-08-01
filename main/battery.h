#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

/* 初始化电池 ADC（GPIO4, ADC1_CH3, 2:1 分压）
 * 安全初始化：失败时 s_adc_ready=false，不会 abort */
void battery_init(void);

/* 获取电池百分比 0-100，失败返回 -1 */
int battery_get_percent(void);

#endif /* BATTERY_H */
