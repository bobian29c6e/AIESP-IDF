#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp-ai.h"
#include <stdlib.h>

static const char *TAG = "ESP_AI_I2S";
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;

esp_err_t esp_ai_i2s_mic_init(int bck, int ws, int din)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = bck, .ws = ws, .din = din, .dout = I2S_GPIO_UNUSED }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "I2S 麦克风初始化成功 (Port 1)");
    return ESP_OK;
}

esp_err_t esp_ai_i2s_spk_init(int bck, int ws, int dout)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = bck, .ws = ws, .dout = dout, .din = I2S_GPIO_UNUSED }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S 扬声器初始化成功 (Port 0)");
    return ESP_OK;
}

esp_err_t esp_ai_i2s_mic_read(int16_t* out_buf, size_t samples_count)
{
    size_t bytes_read = 0;
    static int32_t *raw_buf = NULL;
    static size_t raw_buf_samples = 0;
    static int32_t dc_offset = 0;

    if (raw_buf_samples < samples_count) {
        if (raw_buf) {
            heap_caps_free(raw_buf);
            raw_buf = NULL;
            raw_buf_samples = 0;
        }
        raw_buf = (int32_t *)heap_caps_malloc(samples_count * sizeof(int32_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!raw_buf) {
            ESP_LOGE(TAG, "I2S麦克风原始缓冲申请失败: samples=%u", (unsigned)samples_count);
            return ESP_ERR_NO_MEM;
        }
        raw_buf_samples = samples_count;
    }

    esp_err_t ret = i2s_channel_read(rx_handle, raw_buf, samples_count * 4, &bytes_read, 1000);
    if (ret == ESP_OK) {
        for (int i = 0; i < samples_count; i++) {
            int32_t sample = raw_buf[i] >> 16;
            dc_offset = (dc_offset * 127 + sample) >> 7;
            out_buf[i] = (int16_t)(sample - dc_offset);
        }
    }
    return ret;
}

esp_err_t esp_ai_i2s_spk_write(const int16_t* in_buf, size_t bytes_to_write)
{
    size_t bytes_written = 0;
    return i2s_channel_write(tx_handle, in_buf, bytes_to_write, &bytes_written, 1000);
}
