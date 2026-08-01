#include "battery.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_3   /* GPIO4, NOT CH4=GPIO5=LCD_RST! */
#define BAT_DIVIDER     2

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static bool s_adc_ready = false;

void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGW(TAG, "ADC init failed");
        return;
    }
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel config failed");
        return;
    }
    s_adc_ready = true;
    ESP_LOGI(TAG, "battery ADC init OK (GPIO4)");
}

int battery_get_percent(void)
{
    if (!s_adc_ready) return -1;
    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &raw) != ESP_OK) return -1;
    /* 12 位 ADC: 0-4095, 衰减到 0-3.3V
     * 分压 2:1 -> 实际电压 = raw/4095 * 3.3 * 2
     * 锂电池: 3.0V=0%, 4.2V=100% */
    float voltage = (float)raw / 4095.0f * 3.3f * BAT_DIVIDER;
    int percent = (int)((voltage - 3.0f) / 1.2f * 100.0f);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}
