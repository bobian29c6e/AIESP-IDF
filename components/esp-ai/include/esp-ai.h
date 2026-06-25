#ifndef _ESP_AI_H_
#define _ESP_AI_H_

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 音频采集/放音参数 */
typedef struct {
    int mic_bck;
    int mic_ws;
    int mic_din;
    int spk_bck;
    int spk_ws;
    int spk_dout;
} esp_ai_audio_config_t;

/* 服务器参数 */
typedef struct {
    const char* server_uri;
    const char* api_key;   // ESP-AI 服务认证 key
    const char* ext1;      // 业务扩展字段，兼容 Arduino esp-ai
    const char* device_id;
    const char* volume;    // 映射到 ext2
    const char* persona;   // 映射到 ext3
    const char* voice;     // 映射到 ext4
} esp_ai_server_config_t;

/* 核心配置结构体 */
typedef struct {
    esp_ai_server_config_t server;
    esp_ai_audio_config_t audio;
} esp_ai_config_t;

/* 回调函数定义 */
typedef void (*esp_ai_event_cb_t)(const char* type, cJSON* data);
typedef void (*esp_ai_ready_cb_t)(void);

/**
 * @brief 初始化并启动 esp-ai 引擎
 */
esp_err_t esp_ai_begin(esp_ai_config_t* config);

/**
 * @brief 当前 WebSocket 是否已连接。
 */
bool esp_ai_is_connected(void);

/**
 * @brief 获取/设置业务忙碌状态。
 */
bool esp_ai_get_busy(void);
void esp_ai_set_busy(bool busy);
int64_t esp_ai_get_busy_cleared_at_us(void);

/**
 * @brief ASR/IAT 状态。唤醒只请求开始；麦克风上传必须等服务端 iat_start。
 */
bool esp_ai_is_iat_active(void);
void esp_ai_set_iat_active(bool active);
void esp_ai_set_session_id(const char* session_id);
void esp_ai_set_audio_upload_enabled(bool enabled, const char* reason);
bool esp_ai_should_send_audio(void);

/**
 * @brief 获取/设置播放音量，范围 0.0 - 1.0。
 */
float esp_ai_get_volume(void);
void esp_ai_set_volume(float volume);

/**
 * @brief 注册 Ready 回调（业务握手成功后触发）
 */
void esp_ai_on_ready(esp_ai_ready_cb_t cb);

/**
 * @brief 注册事件回调（收到指令如 ASR/LLM 时触发）
 */
void esp_ai_on_command(esp_ai_event_cb_t cb);

/**
 * @brief 触发语音唤醒 (用于 HTTP/串口等方式触发)
 */
void esp_ai_wakeup(void);

/**
 * @brief 发送对话文本
 */
esp_err_t esp_ai_tts(const char* text);

/**
 * @brief 标记本地 HTTP 大文件上传中。上传期间音频流控会暂停，避免和 HTTP 接收抢网络栈。
 */
void esp_ai_set_upload_active(bool active);
bool esp_ai_get_upload_active(void);

#ifdef __cplusplus
}
#endif

#endif
