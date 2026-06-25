#include "esp-ai.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* 内部函数声明 (来自其他模块) */
void esp_ai_power_init(void);
esp_err_t esp_ai_i2s_mic_init(int bck, int ws, int din);
esp_err_t esp_ai_i2s_spk_init(int bck, int ws, int dout);
esp_err_t esp_ai_ws_init(esp_ai_config_t* config);
esp_err_t esp_ai_ws_send_text_raw(const char* json_str);
esp_err_t esp_ai_uart_init(void);

static const char *TAG = "ESP_AI_CORE";

static size_t json_escape(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) {
        return 0;
    }
    size_t pos = 0;
    for (size_t i = 0; in[i] && pos + 1 < out_len; ++i) {
        const char *rep = NULL;
        if (in[i] == '\\') rep = "\\\\";
        else if (in[i] == '"') rep = "\\\"";
        else if (in[i] == '\n') rep = "\\n";
        else if (in[i] == '\r') rep = "\\r";

        if (rep) {
            size_t rep_len = strlen(rep);
            if (pos + rep_len >= out_len) {
                break;
            }
            memcpy(out + pos, rep, rep_len);
            pos += rep_len;
        } else {
            out[pos++] = in[i];
        }
    }
    out[pos] = '\0';
    return pos;
}

/* 内部函数声明 (来自其他模块) */
void esp_ai_power_init(void);
esp_err_t esp_ai_i2s_mic_init(int bck, int ws, int din);
esp_err_t esp_ai_i2s_spk_init(int bck, int ws, int dout);
esp_err_t esp_ai_ws_init(esp_ai_config_t* config);
esp_err_t esp_ai_uart_init(void);
void esp_ai_audio_init(void);
esp_err_t esp_ai_nvs_save(const char* key, const char* value);
esp_err_t esp_ai_nvs_load(const char* key, char* out_value, size_t max_len);

bool esp_ai_ws_is_ready(void);

bool esp_ai_is_connected(void)
{
    return esp_ai_ws_is_ready();
}

esp_err_t esp_ai_begin(esp_ai_config_t* config)
{
    ESP_LOGI(TAG, "正在复现 esp-ai 库核心逻辑...");
    if (config->server.volume) {
        esp_ai_set_volume(strtof(config->server.volume, NULL));
    }

    // 0. 尝试保存当前配置到 NVS (仅用于测试阶段的自动落地)
    esp_ai_nvs_save("api_key", config->server.api_key);
    esp_ai_nvs_save("device_id", config->server.device_id);

    // 1. 初始化电源
    esp_ai_power_init();

    // 2. 初始化 I2S 音频
    esp_ai_i2s_mic_init(config->audio.mic_bck, config->audio.mic_ws, config->audio.mic_din);
    esp_ai_i2s_spk_init(config->audio.spk_bck, config->audio.spk_ws, config->audio.spk_dout);

    // 3. 启动后台音频任务（录音、TTS 播放、音频流控）
    esp_ai_audio_init();

    // 4. 初始化 ASRPro 串口唤醒
    esp_ai_uart_init();

    // 5. 启动 WebSocket 业务
    return esp_ai_ws_init(config);
}

esp_err_t esp_ai_tts(const char* text)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_ai_set_busy(true);
    char escaped[768];
    char json_str[896];
    json_escape(text, escaped, sizeof(escaped));
    snprintf(json_str, sizeof(json_str), "{\"type\":\"char_txt\",\"text\":\"%s\"}", escaped);
    esp_err_t ret = esp_ai_ws_send_text_raw(json_str);
    if (ret != ESP_OK) {
        esp_ai_set_busy(false);
    }
    return ret;
}

void esp_ai_wakeup(void)
{
    ESP_LOGI(TAG, "检测到唤醒触发，请求开启 IAT；等待服务端 iat_start 后再上传麦克风音频...");
    esp_ai_set_iat_active(true);
    esp_ai_set_audio_upload_enabled(false, "wakeup");
    esp_ai_set_busy(true);

    char json_str[] = "{\"type\":\"iat\",\"data\":{\"status\":\"start\"}}";
    esp_ai_ws_send_text_raw(json_str);
}
