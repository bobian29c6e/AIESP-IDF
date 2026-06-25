#include "driver/gpio.h"
#include "esp_log.h"

#define AUDIO_POWER_EN 46
#define LCD_BACKLIGHT  1
static const char *TAG = "ESP_AI_POWER";

void esp_ai_power_init(void)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << AUDIO_POWER_EN),
    };
    gpio_config(&io_conf);
    gpio_set_level(AUDIO_POWER_EN, 1);
    ESP_LOGI(TAG, "音频系统电源已开启 (GPIO %d)", AUDIO_POWER_EN);

    io_conf.pin_bit_mask = (1ULL << LCD_BACKLIGHT);
    gpio_config(&io_conf);
    gpio_set_level(LCD_BACKLIGHT, 1);
    ESP_LOGI(TAG, "屏幕背光已点亮 (GPIO %d)", LCD_BACKLIGHT);
}
