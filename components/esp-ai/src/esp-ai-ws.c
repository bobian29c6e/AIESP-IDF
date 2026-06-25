#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp-ai.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ESP_AI_WS";
static esp_websocket_client_handle_t client = NULL;
static esp_ai_event_cb_t on_command_cb = NULL;
static esp_ai_ready_cb_t on_ready_cb = NULL;
static SemaphoreHandle_t ws_send_mutex = NULL;
static volatile bool s_business_ready = false;
#define WS_SEND_TIMEOUT_MS 1000
#define WS_TEXT_SEND_TIMEOUT_MS 3000
#define WS_TEXT_SEND_LOCK_MS 1200

static bool is_unreserved_uri_char(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static void url_encode_component(const char *src, char *dst, size_t dst_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] && pos + 1 < dst_len; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (is_unreserved_uri_char(c)) {
            dst[pos++] = (char)c;
        } else {
            if (pos + 3 >= dst_len) {
                break;
            }
            dst[pos++] = '%';
            dst[pos++] = hex[(c >> 4) & 0x0F];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
}

/* 内部接口声明 (来自音频模块) */
void esp_ai_audio_ingest_bin(const uint8_t* data, size_t len);
void esp_ai_set_busy(bool busy);
void esp_ai_set_iat_active(bool active);
void esp_ai_set_session_id(const char* session_id);
void esp_ai_set_audio_upload_enabled(bool enabled, const char* reason);
bool esp_ai_should_send_audio(void);

static bool is_session_done_status(const char *status)
{
    return status &&
           (strcmp(status, "tts_real_end") == 0 ||
            strcmp(status, "end") == 0 ||
            strcmp(status, "session_end") == 0);
}

static const char *extract_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const char *extract_session_status(cJSON *root, cJSON *biz_data)
{
    if (cJSON_IsString(biz_data)) {
        return biz_data->valuestring;
    }
    if (cJSON_IsObject(biz_data)) {
        const char *status = extract_string(biz_data, "status");
        if (status) return status;
    }
    return extract_string(root, "status");
}

static void stop_audio_upload_if_needed(const char *reason)
{
    if (esp_ai_should_send_audio()) {
        ESP_LOGI(TAG, "停止麦克风上传: %s", reason ? reason : "unknown");
    }
    esp_ai_set_audio_upload_enabled(false, reason);
}

static void update_state_from_text_event(const char *type, cJSON *root, cJSON *biz_data)
{
    if (!type || !root) return;

    const char *session_id = extract_string(root, "session_id");
    if (!session_id && cJSON_IsObject(biz_data)) {
        session_id = extract_string(biz_data, "session_id");
    }
    if (session_id) {
        esp_ai_set_session_id(session_id);
    }

    if (strcmp(type, "session_start") == 0) {
        esp_ai_set_iat_active(true);
        esp_ai_set_busy(true);
        ESP_LOGI(TAG, "会话开始 session_id=%s", session_id ? session_id : "");
        return;
    }

    if (strcmp(type, "session_stop") == 0) {
        stop_audio_upload_if_needed("session_stop");
        esp_ai_set_iat_active(false);
        esp_ai_set_busy(false);
        return;
    }

    if (strcmp(type, "auth_fail") == 0 || strcmp(type, "error") == 0) {
        s_business_ready = false;
        stop_audio_upload_if_needed(type);
        esp_ai_set_iat_active(false);
        esp_ai_set_busy(false);
        return;
    }

    if (strcmp(type, "play_audio") == 0) {
        stop_audio_upload_if_needed("play_audio");
        esp_ai_set_busy(true);
        return;
    }

    if (strcmp(type, "instruct") == 0) {
        esp_ai_set_busy(true);
        return;
    }

    if (strcmp(type, "session_status") == 0) {
        const char *status = extract_session_status(root, biz_data);
        if (!status) return;

        if (strcmp(status, "iat_start") == 0) {
            esp_ai_set_iat_active(true);
            esp_ai_set_audio_upload_enabled(true, "iat_start");
            esp_ai_set_busy(true);
            ESP_LOGI(TAG, "服务端允许开始上传麦克风音频");
        } else if (strcmp(status, "iat_end") == 0) {
            stop_audio_upload_if_needed("iat_end");
            esp_ai_set_busy(true);
        } else if (is_session_done_status(status)) {
            stop_audio_upload_if_needed(status);
            esp_ai_set_iat_active(false);
            esp_ai_set_busy(false);
        } else if (strstr(status, "start")) {
            esp_ai_set_busy(true);
        }
        return;
    }

    if (strcmp(type, "tts_real_end") == 0 ||
        strcmp(type, "llm_end") == 0 ||
        strcmp(type, "session_end") == 0) {
        stop_audio_upload_if_needed(type);
        if (strcmp(type, "tts_real_end") == 0 || strcmp(type, "session_end") == 0) {
            esp_ai_set_iat_active(false);
        }
        esp_ai_set_busy(false);
    }
}

/* 检查底层连接状态：仅供内部发送使用，不代表业务鉴权成功。 */
bool esp_ai_ws_is_connected(void)
{
    return client && esp_websocket_client_is_connected(client);
}

bool esp_ai_ws_is_ready(void)
{
    return esp_ai_ws_is_connected() && s_business_ready;
}

/* 发送二进制数据 (用于音频上传) */
esp_err_t esp_ai_ws_send_bin(const uint8_t* data, size_t len)
{
    if (!esp_ai_ws_is_connected()) return ESP_FAIL;

    if (xSemaphoreTake(ws_send_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int ret = esp_websocket_client_send_bin(client, (const char*)data, len, pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
        xSemaphoreGive(ws_send_mutex);
        return (ret >= 0) ? ESP_OK : ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

/* 内部：发送 JSON 指令 */
esp_err_t esp_ai_ws_send_json(const char* type, cJSON* data)
{
    if (!esp_ai_ws_is_connected()) return ESP_FAIL;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (data) cJSON_AddItemToObject(root, "data", data);
    char *json_str = cJSON_PrintUnformatted(root);

    int ret = -1;
    if (xSemaphoreTake(ws_send_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        ret = esp_websocket_client_send_text(client, json_str, strlen(json_str), pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
        xSemaphoreGive(ws_send_mutex);
    }

    ESP_LOGI(TAG, ">> 发送业务指令: %s", json_str);
    free(json_str);
    cJSON_Delete(root);
    return (ret >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_ai_ws_send_text_raw(const char* json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (!esp_ai_ws_is_connected()) {
            ESP_LOGW(TAG, ">> 文本指令发送失败: ws not connected attempt=%d", attempt);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int ret = -1;
        if (xSemaphoreTake(ws_send_mutex, pdMS_TO_TICKS(WS_TEXT_SEND_LOCK_MS)) == pdTRUE) {
            ret = esp_websocket_client_send_text(client, json_str, strlen(json_str), pdMS_TO_TICKS(WS_TEXT_SEND_TIMEOUT_MS));
            xSemaphoreGive(ws_send_mutex);
        } else {
            ESP_LOGW(TAG, ">> 文本指令发送锁等待超时 attempt=%d", attempt);
        }

        if (ret >= 0) {
            ESP_LOGI(TAG, ">> 发送业务指令: %s", json_str);
            return ESP_OK;
        }

        ESP_LOGW(TAG, ">> 文本指令发送失败 ret=%d attempt=%d", ret, attempt);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    ESP_LOGE(TAG, ">> 文本指令发送最终失败: %s", json_str);
    return ESP_FAIL;
}

/* 内部：对齐 Arduino esp-ai 连接后通知 */
static void send_connected_notice(void)
{
    char json_str[] = "{\"type\":\"play_audio_ws_conntceed\"}";

    if (xSemaphoreTake(ws_send_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        esp_websocket_client_send_text(client, json_str, strlen(json_str), pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
        xSemaphoreGive(ws_send_mutex);
    }

    ESP_LOGI(TAG, ">> 发送连接通知: %s", json_str);
}

/* 事件回调 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    (void)handler_args;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✓ 物理连接成功，开始业务握手...");
            s_business_ready = false;
            esp_ai_set_audio_upload_enabled(false, "ws_connected");
            esp_ai_set_busy(false);
            send_connected_notice();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected, wait for auto reconnect");
            s_business_ready = false;
            esp_ai_set_audio_upload_enabled(false, "ws_disconnected");
            esp_ai_set_iat_active(false);
            esp_ai_set_busy(false);
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                // 处理服务器下发的指令
                cJSON *root = cJSON_ParseWithLength((const char *)data->data_ptr, data->data_len);
                if (root) {
                    cJSON *type = cJSON_GetObjectItem(root, "type");
                    cJSON *command_id = cJSON_GetObjectItem(root, "command_id");
                    cJSON *biz_data = cJSON_GetObjectItem(root, "data");
                    if (type && cJSON_IsString(type)) {
                        ESP_LOGI(TAG, "<< 收到指令: %s", type->valuestring);
                        update_state_from_text_event(type->valuestring, root, biz_data);
                        if (strcmp(type->valuestring, "auth_fail") != 0 &&
                            strcmp(type->valuestring, "error") != 0) {
                            if (!s_business_ready) {
                                s_business_ready = true;
                                if (on_ready_cb) {
                                    on_ready_cb();
                                }
                            }
                        }
                        if (on_command_cb) {
                            if (strcmp(type->valuestring, "instruct") == 0 && cJSON_IsString(command_id)) {
                                on_command_cb(command_id->valuestring, biz_data);
                            } else if (strcmp(type->valuestring, "session_status") == 0 && biz_data == NULL) {
                                on_command_cb(type->valuestring, root);
                            } else if (strcmp(type->valuestring, "cron_task") == 0 &&
                                       (biz_data == NULL || !cJSON_IsObject(biz_data))) {
                                on_command_cb(type->valuestring, root);
                            } else {
                                on_command_cb(type->valuestring, biz_data);
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            } else if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                // 收到 TTS 二进制音频包，压入音频缓冲区
                esp_ai_audio_ingest_bin((const uint8_t*)data->data_ptr, data->data_len);
            }
            break;
    }
}

/* 启动连接 */
esp_err_t esp_ai_ws_init(esp_ai_config_t* config)
{
    if (ws_send_mutex == NULL) {
        ws_send_mutex = xSemaphoreCreateMutex();
    }

    char device_id[96];
    char api_key[128];
    char ext1[128];
    char volume[48];
    char persona[128];
    char voice[192];
    url_encode_component(config->server.device_id, device_id, sizeof(device_id));
    url_encode_component(config->server.api_key, api_key, sizeof(api_key));
    url_encode_component(config->server.ext1 ? config->server.ext1 : "", ext1, sizeof(ext1));
    url_encode_component(config->server.volume ? config->server.volume : "0.5", volume, sizeof(volume));
    url_encode_component(config->server.persona ? config->server.persona : "", persona, sizeof(persona));
    url_encode_component(config->server.voice ? config->server.voice : "", voice, sizeof(voice));

    char final_uri[1024];
    snprintf(final_uri, sizeof(final_uri),
             "%s/?v=1.1.0&device_id=%s&api_key=%s&ext1=%s&ext2=%s&ext3=%s&ext4=%s&ext5=&ext6=&ext7=",
             config->server.server_uri,
             device_id,
             api_key,
             ext1,
             volume,    // ext2
             persona,   // ext3
             voice);    // ext4

    const esp_websocket_client_config_t ws_cfg = {
        .uri = final_uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 5000,
        .buffer_size = 2048,
        .task_stack = 4096,
        .task_core_id_set = true,
        .task_core_id = 1,
        .enable_close_reconnect = true,
    };

    ESP_LOGI(TAG, "正在连接 (含业务参数): %s", final_uri);
    ESP_LOGI(TAG, "WS启动前内存: internal=%lu largest=%lu psram=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        ESP_LOGE(TAG, "WebSocket init failed: internal=%lu largest=%lu psram=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)config);
    esp_err_t ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s internal=%lu largest=%lu psram=%lu",
                 esp_err_to_name(ret),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    return ret;
}

void esp_ai_on_ready(esp_ai_ready_cb_t cb) { on_ready_cb = cb; }
void esp_ai_on_command(esp_ai_event_cb_t cb) { on_command_cb = cb; }
