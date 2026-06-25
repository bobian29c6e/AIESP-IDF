#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp-ai.h"
#include "driver/i2s_std.h"
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/ringbuf.h"
#include "helix/mp3dec.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "ESP_AI_AUDIO";
#define AUDIO_BUFFER_SIZE (1024 * 256)
#define MP3_PCM_MAX_SAMPLES (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP)
static RingbufHandle_t audio_rb = NULL;
static bool s_audio_busy_seen = false;
static int s_audio_full_ticks = 0;
static int64_t s_audio_busy_started_at = 0;
static volatile bool s_upload_active = false;

void esp_ai_set_upload_active(bool active)
{
    s_upload_active = active;
}

bool esp_ai_get_upload_active(void)
{
    return s_upload_active;
}

/* 内部接口声明 */
bool esp_ai_ws_is_connected(void);
esp_err_t esp_ai_ws_send_bin(const uint8_t* data, size_t len);
esp_err_t esp_ai_ws_send_json(const char* type, cJSON* data);
bool esp_ai_should_send_audio(void);
esp_err_t esp_ai_i2s_mic_read(int16_t* out_buf, size_t samples_count);
esp_err_t esp_ai_i2s_spk_write(const int16_t* in_buf, size_t bytes_to_write);
float esp_ai_get_volume(void);

/* 音频播放任务：从缓冲区读取 MP3 并实时解码播放 */
static void audio_playback_task(void *pvParameters)
{
    HMP3Decoder hMP3Decoder = MP3InitDecoder();
    MP3FrameInfo mp3FrameInfo;
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(MP3_PCM_MAX_SAMPLES * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (!hMP3Decoder || !pcm_buf) {
        ESP_LOGE(TAG, "MP3 playback init failed: decoder=%p pcm=%p internal=%lu psram=%lu",
                 hMP3Decoder,
                 pcm_buf,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        if (pcm_buf) heap_caps_free(pcm_buf);
        if (hMP3Decoder) MP3FreeDecoder(hMP3Decoder);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Helix MP3 解码泵已启动...");

    while (1) {
        size_t item_size;
        uint8_t *mp3_data = (uint8_t *)xRingbufferReceive(audio_rb, &item_size, pdMS_TO_TICKS(100));

        if (mp3_data != NULL) {
            uint8_t *read_ptr = mp3_data;
            int bytes_left = (int)item_size;
            int frames_since_yield = 0;

            while (bytes_left > 0) {
                int offset = MP3FindSyncWord(read_ptr, bytes_left);
                if (offset < 0) break;

                read_ptr += offset;
                bytes_left -= offset;

                int err = MP3Decode(hMP3Decoder, &read_ptr, &bytes_left, pcm_buf, 0);
                if (err == ERR_MP3_NONE) {
                    MP3GetLastFrameInfo(hMP3Decoder, &mp3FrameInfo);
                    if (mp3FrameInfo.outputSamps <= 0 || mp3FrameInfo.outputSamps > MP3_PCM_MAX_SAMPLES) {
                        ESP_LOGW(TAG, "Drop invalid MP3 frame outputSamps=%d max=%d bytesLeft=%d",
                                 mp3FrameInfo.outputSamps, MP3_PCM_MAX_SAMPLES, bytes_left);
                        continue;
                    }
                    float volume = esp_ai_get_volume();
                    for (int i = 0; i < mp3FrameInfo.outputSamps; ++i) {
                        int32_t sample = (int32_t)(pcm_buf[i] * volume);
                        if (sample > INT16_MAX) sample = INT16_MAX;
                        if (sample < INT16_MIN) sample = INT16_MIN;
                        pcm_buf[i] = (int16_t)sample;
                    }
                    // 写入解码后的 PCM 数据到扬声器
                    esp_ai_i2s_spk_write(pcm_buf, mp3FrameInfo.outputSamps * sizeof(int16_t));
                    if (++frames_since_yield >= 4) {
                        frames_since_yield = 0;
                        vTaskDelay(1);
                    }
                } else {
                    if (bytes_left > 0) {
                        read_ptr++;
                        bytes_left--;
                    } else {
                        break;
                    }
                    vTaskDelay(1);
                }
            }
            vRingbufferReturnItem(audio_rb, (void *)mp3_data);
        }
        vTaskDelay(1);
    }
    heap_caps_free(pcm_buf);
    MP3FreeDecoder(hMP3Decoder);
    vTaskDelete(NULL);
}

/* 流控任务：每秒汇报一次可用缓冲区空间 */
static void audio_flow_control_task(void *pvParameters)
{
    while (1) {
        if (s_upload_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool ws_connected = esp_ai_ws_is_connected();
        if (ws_connected && audio_rb != NULL) {
            size_t free_size = xRingbufferGetCurFreeSize(audio_rb);
            bool busy = esp_ai_get_busy();
            int64_t now = esp_timer_get_time();

            if (busy) {
                if (s_audio_busy_started_at == 0) {
                    s_audio_busy_started_at = now;
                }
                if (free_size < AUDIO_BUFFER_SIZE) {
                    s_audio_busy_seen = true;
                    s_audio_full_ticks = 0;
                } else if (s_audio_busy_seen) {
                    s_audio_full_ticks++;
                    if (s_audio_full_ticks >= 3) {
                        ESP_LOGI(TAG, "Audio buffer drained, mark session idle");
                        esp_ai_set_busy(false);
                        s_audio_busy_seen = false;
                        s_audio_full_ticks = 0;
                        s_audio_busy_started_at = 0;
                    }
                } else if ((now - s_audio_busy_started_at) > 10000000LL) {
                    ESP_LOGW(TAG, "Audio busy timeout without buffered audio, mark session idle");
                    esp_ai_set_busy(false);
                    s_audio_full_ticks = 0;
                    s_audio_busy_started_at = 0;
                }
            } else {
                s_audio_busy_seen = false;
                s_audio_full_ticks = 0;
                s_audio_busy_started_at = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 外部推流接口：由 WebSocket 调用 */
void esp_ai_audio_ingest_bin(const uint8_t* data, size_t len)
{
    if (audio_rb) {
        if (xRingbufferSend(audio_rb, data, len, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Audio buffer full/drop len=%u free=%u", (unsigned)len, (unsigned)xRingbufferGetCurFreeSize(audio_rb));
        }
    }
}

/* 音频采集与上传任务 (16kHz, 16-bit PCM) */
static void audio_uplink_task(void *pvParameters)
{
    const size_t samples_per_frame = 512;
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(samples_per_frame * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Audio uplink pcm buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "音频上行泵已启动...");
    while (1) {
        if (esp_ai_i2s_mic_read(pcm_buf, samples_per_frame) == ESP_OK) {
            if (esp_ai_should_send_audio() && esp_ai_ws_is_connected() && !s_upload_active) {
                esp_ai_ws_send_bin((uint8_t*)pcm_buf, samples_per_frame * sizeof(int16_t));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    heap_caps_free(pcm_buf);
}

void esp_ai_audio_init(void)
{
    ESP_LOGI(TAG, "Audio init before alloc: internal=%lu largest=%lu psram=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    audio_rb = xRingbufferCreateWithCaps(AUDIO_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (audio_rb == NULL) {
        ESP_LOGW(TAG, "PSRAM audio ringbuffer failed, fallback to internal 40KB");
        audio_rb = xRingbufferCreate(1024 * 40, RINGBUF_TYPE_BYTEBUF);
    }
    BaseType_t uplink_ok = xTaskCreateWithCaps(audio_uplink_task, "ai_uplink_task", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (uplink_ok != pdPASS) {
        ESP_LOGW(TAG, "PSRAM uplink task stack failed, fallback to internal");
        xTaskCreate(audio_uplink_task, "ai_uplink_task", 4096, NULL, 5, NULL);
    }
    BaseType_t flow_ok = xTaskCreateWithCaps(audio_flow_control_task, "ai_flow_task", 3072, NULL, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (flow_ok != pdPASS) {
        ESP_LOGW(TAG, "PSRAM flow task stack failed, fallback to internal");
        xTaskCreate(audio_flow_control_task, "ai_flow_task", 3072, NULL, 4, NULL);
    }
    xTaskCreate(audio_playback_task, "ai_spk_task", 8192, NULL, 6, NULL);
    ESP_LOGI(TAG, "音频引擎(上行/下行解码/流控)初始化完成 internal=%lu largest=%lu psram=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
