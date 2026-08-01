#include "backlight.h"
#include "lcd_i80.h"        /* LCD_BL_IO 定义 */
#include "esp_log.h"
#include "esp_err.h"
#include "driver/ledc.h"
#include "driver/gpio.h"

static const char *TAG = "backlight";

/* LEDC 配置：10-bit 分辨率 (0-1023) + 5kHz 频率，足够平滑无闪烁 */
#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_DUTY_RES    LEDC_TIMER_10_BIT
#define BL_DUTY_MAX         1023
#define BL_FREQ_HZ          5000

/* 5 档亮度（百分比）。0% = 完全关闭背光（息屏），再按一下回到 25% */
static const int bl_levels[] = { 0, 25, 50, 75, 100 };
#define BL_LEVEL_COUNT  (sizeof(bl_levels) / sizeof(bl_levels[0]))

static int bl_current_idx = BL_LEVEL_COUNT - 1;  /* 默认 100% */

static inline int duty_from_percent(int percent)
{
    if (percent <= 0)  return 0;   /* 0% -> duty 0，完全关闭背光 */
    if (percent > 100) percent = 100;
    /* +0.5 四舍五入，避免低段丢失 */
    return (percent * BL_DUTY_MAX + 50) / 100;
}

static inline int percent_from_idx(int idx)
{
    if (idx < 0) idx = 0;
    if (idx >= (int)BL_LEVEL_COUNT) idx = (int)BL_LEVEL_COUNT - 1;
    return bl_levels[idx];
}

void backlight_init(void)
{
    /* 1. LEDC 定时器 */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* 2. LEDC 通道，绑定到 LCD_BL_IO (GPIO38)。
     *    之前 lcd_i80.c 已把 BL 配为 GPIO 输出并拉低，
     *    ledc_channel_config 内部会重配 IO_MUX 把输出切到 LEDC 信号。 */
    ledc_channel_config_t ch_cfg = {
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LCD_BL_IO,
        .duty       = duty_from_percent(percent_from_idx(bl_current_idx)),
        .hpoint     = 0,
        .flags      = { .output_invert = 0 },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ESP_LOGI(TAG, "backlight init: PWM %uHz %ubit on GPIO%u, duty=%d (%d%%)",
             BL_FREQ_HZ, 10, LCD_BL_IO,
             duty_from_percent(percent_from_idx(bl_current_idx)),
             percent_from_idx(bl_current_idx));
}

int backlight_cycle(void)
{
    bl_current_idx = (bl_current_idx + 1) % BL_LEVEL_COUNT;
    int percent = percent_from_idx(bl_current_idx);
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty_from_percent(percent));
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    ESP_LOGI(TAG, "brightness -> %d%%", percent);
    return percent;
}

int backlight_set_percent(int percent)
{
    /* 找到最接近的档位，便于 cycle 后续连贯 */
    int best_idx = 0;
    int best_diff = 1000;
    for (int i = 0; i < (int)BL_LEVEL_COUNT; i++) {
        int d = bl_levels[i] - percent;
        if (d < 0) d = -d;
        if (d < best_diff) { best_diff = d; best_idx = i; }
    }
    bl_current_idx = best_idx;
    int actual = percent_from_idx(bl_current_idx);
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty_from_percent(actual));
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    return actual;
}

int backlight_get_percent(void)
{
    return percent_from_idx(bl_current_idx);
}
