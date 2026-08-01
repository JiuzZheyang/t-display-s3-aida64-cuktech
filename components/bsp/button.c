#include "button.h"
#include "lcd_i80.h"        /* LCD_BOOT_IO 定义 */
#include "backlight.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

#define BOOT_BTN_IO        LCD_BOOT_IO      /* GPIO 0 */
#define KEY_BTN_IO         LCD_KEY_IO       /* GPIO 14 - power/sleep key */
#define POLL_PERIOD_MS     10
#define DEBOUNCE_MS        20
#define SHORT_PRESS_MAX_MS  1000           /* 超过 1s 松手不算短按 */
#define LONG_PRESS_MS      3000           /* 长按 3 秒触发 */

static button_brightness_cb_t s_brightness_cb = NULL;
static button_longpress_cb_t s_longpress_cb = NULL;
static button_keyclick_cb_t s_keyclick_cb = NULL;
static TaskHandle_t s_task_handle = NULL;

void button_register_brightness_cb(button_brightness_cb_t cb)
{
    s_brightness_cb = cb;
}

void button_register_longpress_cb(button_longpress_cb_t cb)
{
    s_longpress_cb = cb;
}

void button_register_keyclick_cb(button_keyclick_cb_t cb)
{
    s_keyclick_cb = cb;
}


/* 进入深度休眠（关机）：配置 KEY(GPIO14) 低电平唤醒 */
static void enter_deep_sleep(void)
{
    ESP_LOGW(TAG, "Entering deep sleep (power off). Press KEY to wake.");
    /* 关背光 */
    backlight_set_percent(0);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 等 KEY 松开，避免立即被唤醒 */
    while (gpio_get_level(KEY_BTN_IO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 隔离所有非唤醒 GPIO，降低功耗，避免漏电影响充电 */
    rtc_gpio_deinit(KEY_BTN_IO);
    esp_sleep_config_gpio_isolate();

    /* ext0 唤醒：KEY 拉低 (level=0) 唤醒 */
    rtc_gpio_pullup_en(KEY_BTN_IO);
    rtc_gpio_pulldown_dis(KEY_BTN_IO);
    esp_sleep_enable_ext0_wakeup(KEY_BTN_IO, 0);

    esp_deep_sleep_start();  /* 不返回；唤醒后从头 reboot */
}

static void button_task(void *arg)
{
    (void)arg;
    int  press_stable_ms = 0;
    bool fired = false;
    int  key_stable_ms = 0;
    bool key_fired = false;

    /* 启动时若按键仍被按住（例如长按 BOOT 触发重启后手指未松开），
     * 先等待它们释放，避免把遗留的长按误判成一次短按（切亮度/切主题） */
    while (gpio_get_level(BOOT_BTN_IO) == 0 || gpio_get_level(KEY_BTN_IO) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (1) {
        /* ---- KEY(GPIO14) 电源键：短按切主题，长按 3s 关机 ---- */
        bool key_pressed = (gpio_get_level(KEY_BTN_IO) == 0);
        if (key_pressed) {
            key_stable_ms += POLL_PERIOD_MS;
            if (!key_fired && key_stable_ms >= LONG_PRESS_MS) {
                key_fired = true;
                enter_deep_sleep();
            }
        } else {
            /* KEY 短按：切换主题 */
            if (key_stable_ms >= DEBOUNCE_MS && !key_fired) {
                ESP_LOGI(TAG, "KEY short press, cycle theme");
                if (s_brightness_cb) s_brightness_cb(0);  /* 复用回调做主题切换 */
            }
            key_stable_ms = 0;
            key_fired = false;
        }

        bool pressed = (gpio_get_level(BOOT_BTN_IO) == 0);

        if (pressed) {
            press_stable_ms += POLL_PERIOD_MS;
        } else {
            /* 松开：短按 (<1s) -> 切换屏幕；1-3s 松手 -> 忽略 */
            if (press_stable_ms >= DEBOUNCE_MS && press_stable_ms < SHORT_PRESS_MAX_MS && !fired) {
                ESP_LOGI(TAG, "BOOT short press (%dms), switch screen", press_stable_ms);
                if (s_keyclick_cb) s_keyclick_cb();
            }
            press_stable_ms = 0;
            fired = false;
        }

        /* 长按检测 */
        if (pressed && !fired && press_stable_ms >= LONG_PRESS_MS) {
            fired = true;
            ESP_LOGW(TAG, "LONG press detected");
            if (s_longpress_cb) s_longpress_cb();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void button_init(void)
{
    /* GPIO 0 在 ESP32-S3 上电阶段会被 ROM bootloader 用作 strap，
     * 进入 app 后可安全配为输入 + 上拉，作为普通按键使用。 */
    gpio_config_t io_cfg = {
        .pin_bit_mask  = (1ULL << BOOT_BTN_IO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    /* KEY(GPIO14) 电源键：输入 + 上拉 */
    gpio_config_t key_cfg = {
        .pin_bit_mask  = (1ULL << KEY_BTN_IO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&key_cfg));

    BaseType_t ok = xTaskCreatePinnedToCore(button_task, "btn", 4096,
                                            NULL, 3, &s_task_handle, 0);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "button init: BOOT on GPIO%u, poll=%ums debounce=%ums",
             BOOT_BTN_IO, POLL_PERIOD_MS, DEBOUNCE_MS);
}
