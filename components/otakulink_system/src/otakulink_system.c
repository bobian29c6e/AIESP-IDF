#include "otakulink_system.h"

#include <stdio.h>
#include <string.h>

#include "esp-ai.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "OTAKULINK_SYS";

#define BUSINESS_WS_BASE_URI "ws://api.espai2.fun:80/connect_espai_node"
#define SYSTEM_TASK_PERIOD_MS 1000
#define HEALTH_LOG_INTERVAL_MS 15000
#define WS_RECONNECT_MIN_INTERVAL_MS 5000
#define WS_RECONNECT_DELAY_MS 2000
#define LOW_MEM_HOLD_MS 10000
#define WIFI_RESET_MIN_INTERVAL_MS 60000
#define RECOVERY_FALLBACK_MS 30000
#define AI_WS_PAUSE_HOLD_MS 15000

#define RISK_HEAP_BYTES (35 * 1024)
#define RISK_MAX_BYTES (22 * 1024)
#define CRITICAL_HEAP_BYTES (20 * 1024)
#define CRITICAL_MAX_BYTES (8 * 1024)
#define RECOVERY_HEAP_BYTES (44 * 1024)
#define RECOVERY_MAX_BYTES (30 * 1024)

typedef struct {
    bool initialized;
    bool wifi_connected;
    bool business_ws_connected;
    bool business_ws_paused;
    bool business_ws_started;
    bool reconnect_pending;
    bool ai_pause_active;
    int64_t reconnect_due_ms;
    int64_t last_reconnect_attempt_ms;
    int64_t low_mem_since_ms;
    int64_t recovery_started_ms;
    int64_t last_health_log_ms;
    int64_t last_wifi_reset_ms;
    int64_t ai_pause_until_ms;
    bool network_recovery_active;
    bool risk_active;
    uint32_t wifi_disconnects;
    uint32_t wifi_stack_resets;
    uint32_t business_ws_connects;
    uint32_t business_ws_disconnects;
    uint32_t business_ws_errors;
    uint32_t low_mem_events;
    char uri[768];
} otakulink_system_state_t;

static otakulink_system_state_t s_state;
static SemaphoreHandle_t s_lock;
static esp_websocket_client_handle_t s_business_ws;
static TaskHandle_t s_system_task;
static otakulink_business_ws_message_cb_t s_business_ws_message_cb;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void lock_state(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_state(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void copy_state(otakulink_system_state_t *out)
{
    lock_state();
    *out = s_state;
    unlock_state();
}

static bool is_url_safe_char(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

static void append_url_encoded(char *out, size_t out_len, size_t *pos, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!out || !pos || !in || out_len == 0) {
        return;
    }

    for (const unsigned char *p = (const unsigned char *)in; *p && *pos + 1 < out_len; ++p) {
        if (is_url_safe_char((char)*p)) {
            out[(*pos)++] = (char)*p;
        } else if (*pos + 3 < out_len) {
            out[(*pos)++] = '%';
            out[(*pos)++] = hex[*p >> 4];
            out[(*pos)++] = hex[*p & 0x0f];
        } else {
            break;
        }
    }
    out[*pos] = '\0';
}

static void append_query_kv(char *out, size_t out_len, size_t *pos, const char *key, const char *value)
{
    if (*pos + strlen(key) + 2 >= out_len) {
        return;
    }
    out[(*pos)++] = '&';
    out[*pos] = '\0';
    strlcat(out, key, out_len);
    *pos = strlen(out);
    if (*pos + 1 >= out_len) {
        return;
    }
    out[(*pos)++] = '=';
    out[*pos] = '\0';
    append_url_encoded(out, out_len, pos, value ? value : "");
}

static void build_business_ws_uri(const otakulink_business_ws_config_t *config, char *out, size_t out_len)
{
    size_t pos = 0;
    out[0] = '\0';
    strlcpy(out, BUSINESS_WS_BASE_URI, out_len);
    strlcat(out, "?device_type=hardware", out_len);
    pos = strlen(out);
    append_query_kv(out, out_len, &pos, "device_id", config->device_id);
    append_query_kv(out, out_len, &pos, "version", config->version ? config->version : "1.1.0");
    append_query_kv(out, out_len, &pos, "api_key", config->api_key);
    append_query_kv(out, out_len, &pos, "ext2", config->ext2);
    append_query_kv(out, out_len, &pos, "ext3", config->ext3);
    append_query_kv(out, out_len, &pos, "ext4", config->ext4);
    append_query_kv(out, out_len, &pos, "ext5", config->ext5);
}

static void schedule_business_ws_reconnect_locked(int delay_ms, const char *reason)
{
    int64_t due = now_ms() + delay_ms;
    if (!s_state.reconnect_pending || due < s_state.reconnect_due_ms) {
        s_state.reconnect_due_ms = due;
    }
    s_state.reconnect_pending = true;
    ESP_LOGI(TAG, "[业务WS] 已安排重连 延迟=%dms 原因=%s", delay_ms, reason ? reason : "未知");
}

static void pause_business_ws_locked(bool paused, const char *reason)
{
    if (s_state.business_ws_paused == paused) {
        return;
    }
    s_state.business_ws_paused = paused;
    if (paused) {
        s_state.reconnect_pending = false;
    }
    ESP_LOGW(TAG, "[业务WS] %s 原因=%s", paused ? "已暂停" : "已恢复", reason ? reason : "未知");
}

static void business_ws_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        lock_state();
        s_state.business_ws_connected = true;
        s_state.business_ws_connects++;
        s_state.reconnect_pending = false;
        unlock_state();
        ESP_LOGI(TAG, "[业务WS] 已连接 SRAM=%lu 最大连续=%lu",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (s_business_ws_message_cb) {
            s_business_ws_message_cb("__connected", NULL);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        lock_state();
        s_state.business_ws_connected = false;
        s_state.business_ws_started = false;
        s_state.business_ws_disconnects++;
        if (!s_state.business_ws_paused && s_state.wifi_connected) {
            schedule_business_ws_reconnect_locked(WS_RECONNECT_DELAY_MS, "事件断开");
        }
        unlock_state();
        ESP_LOGW(TAG, "[业务WS] 已断开 event=%ld SRAM=%lu 最大连续=%lu",
                 (long)event_id,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        break;
    case WEBSOCKET_EVENT_ERROR:
        lock_state();
        s_state.business_ws_errors++;
        unlock_state();
        ESP_LOGW(TAG, "[业务WS] 发生错误 type=%d errno=%d",
                 data ? data->error_handle.error_type : -1,
                 data ? data->error_handle.esp_transport_sock_errno : 0);
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data && data->op_code == 0x1 && data->data_ptr && data->data_len > 0) {
            int len = data->data_len > 160 ? 160 : data->data_len;
            ESP_LOGI(TAG, "[业务WS] 收到消息 长度=%d 内容=%.*s%s",
                     data->data_len, len, data->data_ptr, data->data_len > len ? "..." : "");
            char *json_text = heap_caps_malloc(data->data_len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (json_text) {
                memcpy(json_text, data->data_ptr, data->data_len);
                json_text[data->data_len] = '\0';
                cJSON *root = cJSON_Parse(json_text);
                cJSON *type = root ? cJSON_GetObjectItem(root, "type") : NULL;
                if (root && cJSON_IsString(type) && s_business_ws_message_cb) {
                    s_business_ws_message_cb(type->valuestring, root);
                } else if (root && !cJSON_IsString(type)) {
                    ESP_LOGW(TAG, "[业务WS] 消息缺少字符串type字段");
                } else if (!root) {
                    ESP_LOGW(TAG, "[业务WS] 消息JSON解析失败");
                }
                if (root) cJSON_Delete(root);
                heap_caps_free(json_text);
            }
        }
        break;
    default:
        break;
    }
}

static void stop_business_ws(const char *reason)
{
    if (!s_business_ws) {
        return;
    }

    int64_t t0 = now_ms();
    esp_websocket_client_stop(s_business_ws);
    int64_t dt = now_ms() - t0;
    if (dt > 1000) {
        ESP_LOGW(TAG, "[业务WS][诊断] 停止耗时=%lldms 原因=%s", (long long)dt, reason ? reason : "未知");
    }

    lock_state();
    s_state.business_ws_connected = false;
    s_state.business_ws_started = false;
    unlock_state();
}

static void start_business_ws_if_needed(void)
{
    otakulink_system_state_t snap;
    copy_state(&snap);

    if (!snap.initialized || !snap.wifi_connected || snap.business_ws_paused || !snap.reconnect_pending) {
        return;
    }
    if (now_ms() < snap.reconnect_due_ms) {
        return;
    }
    if (snap.uri[0] == '\0') {
        lock_state();
        s_state.reconnect_pending = false;
        unlock_state();
        return;
    }
    if (snap.business_ws_started && s_business_ws) {
        lock_state();
        s_state.reconnect_pending = false;
        unlock_state();
        return;
    }
    if (now_ms() - snap.last_reconnect_attempt_ms < WS_RECONNECT_MIN_INTERVAL_MS) {
        lock_state();
        s_state.reconnect_due_ms = snap.last_reconnect_attempt_ms + WS_RECONNECT_MIN_INTERVAL_MS;
        s_state.reconnect_pending = true;
        unlock_state();
        return;
    }

    if (s_business_ws) {
        stop_business_ws("重连前清理旧连接");
        esp_websocket_client_destroy(s_business_ws);
        s_business_ws = NULL;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = snap.uri,
        .task_name = "business_ws",
        .task_stack = 6144,
        .task_prio = 4,
        .buffer_size = 1024,
        .network_timeout_ms = 5000,
        .reconnect_timeout_ms = 10000,
        .ping_interval_sec = 30,
        .pingpong_timeout_sec = 45,
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
    };

    s_business_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_business_ws) {
        lock_state();
        s_state.business_ws_errors++;
        schedule_business_ws_reconnect_locked(WS_RECONNECT_MIN_INTERVAL_MS, "初始化失败");
        unlock_state();
        ESP_LOGE(TAG, "[业务WS] 初始化失败");
        return;
    }

    esp_websocket_register_events(s_business_ws, WEBSOCKET_EVENT_ANY, business_ws_event_handler, NULL);

    lock_state();
    s_state.last_reconnect_attempt_ms = now_ms();
    s_state.reconnect_pending = false;
    s_state.business_ws_started = true;
    unlock_state();

    ESP_LOGI(TAG, "[业务WS] 正在连接 uri=%s", snap.uri);
    esp_err_t err = esp_websocket_client_start(s_business_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[业务WS] 启动失败: %s", esp_err_to_name(err));
        lock_state();
        s_state.business_ws_started = false;
        s_state.business_ws_errors++;
        schedule_business_ws_reconnect_locked(WS_RECONNECT_MIN_INTERVAL_MS, "启动失败");
        unlock_state();
    }
}

static void wifi_reset_task(void *arg)
{
    const char *reason = (const char *)arg;
    ESP_LOGW(TAG, "[网络] 正在重置WiFi网络栈 原因=%s", reason ? reason : "未知");
    stop_business_ws("WiFi重置");
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(700));
    esp_wifi_start();
    ESP_LOGW(TAG, "[网络] WiFi网络栈重置已请求，正在重新启动 原因=%s", reason ? reason : "未知");
    vTaskDelete(NULL);
}

static void request_wifi_stack_reset(const char *reason)
{
    int64_t now = now_ms();
    lock_state();
    if (now - s_state.last_wifi_reset_ms < WIFI_RESET_MIN_INTERVAL_MS) {
        ESP_LOGW(TAG, "[网络] WiFi重置过于频繁，已跳过 原因=%s", reason ? reason : "未知");
        unlock_state();
        return;
    }
    s_state.last_wifi_reset_ms = now;
    s_state.wifi_stack_resets++;
    unlock_state();

    xTaskCreate(wifi_reset_task, "wifi_reset", 4096, (void *)reason, 6, NULL);
}

static int get_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

static void handle_low_memory_guard(void)
{
    uint32_t heap_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t max_now = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    bool critical = heap_now < CRITICAL_HEAP_BYTES || max_now < CRITICAL_MAX_BYTES;
    bool pressure = heap_now < RISK_HEAP_BYTES && max_now < RISK_MAX_BYTES;
    bool upload = esp_ai_get_upload_active();
    bool ai_active = esp_ai_get_busy();
    bool risk = critical || (!ai_active && pressure);
    int64_t now = now_ms();

    lock_state();
    if (s_state.network_recovery_active && s_state.wifi_connected) {
        bool recovered = heap_now > RECOVERY_HEAP_BYTES && max_now > RECOVERY_MAX_BYTES;
        bool fallback = (now - s_state.recovery_started_ms > RECOVERY_FALLBACK_MS) && heap_now > RISK_HEAP_BYTES;
        if (recovered || fallback) {
            ESP_LOGW(TAG, "[风险保护] 内存已恢复 SRAM=%lu 最大连续=%lu 模式=%s",
                     (unsigned long)heap_now, (unsigned long)max_now, recovered ? "完全恢复" : "兜底恢复");
            s_state.network_recovery_active = false;
            s_state.risk_active = false;
            s_state.low_mem_since_ms = 0;
            pause_business_ws_locked(false, "风险已恢复");
            schedule_business_ws_reconnect_locked(WS_RECONNECT_DELAY_MS, "风险已恢复");
            unlock_state();
            return;
        }
    }

    if (!risk) {
        s_state.low_mem_since_ms = 0;
        if (s_state.risk_active && !s_state.network_recovery_active) {
            ESP_LOGW(TAG, "[风险保护] 上传/内存风险已解除 SRAM=%lu 最大连续=%lu",
                     (unsigned long)heap_now, (unsigned long)max_now);
            s_state.risk_active = false;
            pause_business_ws_locked(false, "风险已解除");
            schedule_business_ws_reconnect_locked(WS_RECONNECT_DELAY_MS, "风险已解除");
        }
        unlock_state();
        return;
    }

    if (s_state.low_mem_since_ms == 0) {
        s_state.low_mem_since_ms = now;
    }
    int64_t risk_for = now - s_state.low_mem_since_ms;
    if (risk_for == 0 || risk_for % 5000 < SYSTEM_TASK_PERIOD_MS) {
        ESP_LOGW(TAG, "[风险保护] SRAM=%lu 最大连续=%lu 持续=%lldms 严重=%d 压力=%d 上传=%d AI=%d 恢复中=%d",
                 (unsigned long)heap_now,
                 (unsigned long)max_now,
                 (long long)risk_for,
                 critical ? 1 : 0,
                 pressure ? 1 : 0,
                 upload ? 1 : 0,
                 ai_active ? 1 : 0,
                 s_state.network_recovery_active ? 1 : 0);
    }

    if (!s_state.network_recovery_active && risk_for > LOW_MEM_HOLD_MS) {
        s_state.low_mem_events++;
        if (upload) {
            ESP_LOGW(TAG, "[风险保护] 上传中：暂停业务WS，跳过WiFi重置 SRAM=%lu 最大连续=%lu",
                     (unsigned long)heap_now, (unsigned long)max_now);
            pause_business_ws_locked(true, "上传期间内存偏低");
            s_state.business_ws_connected = false;
            s_state.risk_active = true;
            s_state.low_mem_since_ms = now;
            unlock_state();
            stop_business_ws("上传期间内存偏低");
            return;
        }
        if (ai_active && !critical) {
            ESP_LOGW(TAG, "[风险保护] AI忙碌中：暂停业务WS，暂缓WiFi重置 SRAM=%lu 最大连续=%lu",
                     (unsigned long)heap_now, (unsigned long)max_now);
            pause_business_ws_locked(true, "AI忙碌期间内存偏低");
            s_state.business_ws_connected = false;
            s_state.risk_active = true;
            s_state.low_mem_since_ms = now;
            unlock_state();
            stop_business_ws("AI忙碌期间内存偏低");
            return;
        }

        ESP_LOGW(TAG, "[风险保护] 低内存持续存在：暂停业务WS并重置WiFi网络栈");
        pause_business_ws_locked(true, "低内存保护");
        s_state.business_ws_connected = false;
        s_state.network_recovery_active = true;
        s_state.risk_active = true;
        s_state.recovery_started_ms = now;
        unlock_state();
        stop_business_ws("低内存保护");
        request_wifi_stack_reset("低内存保护");
        return;
    }
    unlock_state();
}

static void handle_ai_activity_pause(void)
{
    int64_t now = now_ms();
    bool hold_active = false;
    lock_state();
    hold_active = now < s_state.ai_pause_until_ms;
    unlock_state();

    bool active = esp_ai_get_busy() || esp_ai_get_upload_active() || hold_active;
    bool should_stop = false;
    bool should_resume = false;

    lock_state();
    if (active && !s_state.ai_pause_active) {
        s_state.ai_pause_active = true;
        pause_business_ws_locked(true, esp_ai_get_upload_active() ? "上传中" : "AI忙碌");
        s_state.business_ws_connected = false;
        should_stop = true;
    } else if (active && s_state.business_ws_paused &&
               (s_state.business_ws_started || s_state.business_ws_connected)) {
        s_state.business_ws_connected = false;
        should_stop = true;
    } else if (!active && s_state.ai_pause_active) {
        s_state.ai_pause_active = false;
        if (!s_state.network_recovery_active && !s_state.risk_active) {
            pause_business_ws_locked(false, "AI空闲");
            if (s_state.wifi_connected) {
                schedule_business_ws_reconnect_locked(WS_RECONNECT_DELAY_MS, "AI空闲");
            }
            should_resume = true;
        }
    }
    unlock_state();

    if (should_stop) {
        stop_business_ws("ai_or_upload_active");
    }
    if (should_resume) {
        ESP_LOGI(TAG, "[业务WS] AI/上传已空闲，允许业务WS重连");
    }
}

static void log_health_if_due(void)
{
    int64_t now = now_ms();
    otakulink_system_state_t snap;
    copy_state(&snap);
    if (now - snap.last_health_log_ms < HEALTH_LOG_INTERVAL_MS) {
        return;
    }

    lock_state();
    s_state.last_health_log_ms = now;
    unlock_state();

    ESP_LOGI(TAG,
             "[健康] SRAM=%lu 最低SRAM=%lu 最大连续=%lu PSRAM=%lu WiFi信号=%d WiFi=%d AI=%d 忙碌=%d 业务WS=%d 暂停=%d 上传=%d 恢复中=%d WiFi断开=%lu WiFi重置=%lu 业务WS连接=%lu 业务WS断开=%lu 业务WS错误=%lu 低内存=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             get_wifi_rssi(),
             snap.wifi_connected ? 1 : 0,
             esp_ai_is_connected() ? 1 : 0,
             esp_ai_get_busy() ? 1 : 0,
             snap.business_ws_connected ? 1 : 0,
             snap.business_ws_paused ? 1 : 0,
             esp_ai_get_upload_active() ? 1 : 0,
             snap.network_recovery_active ? 1 : 0,
             (unsigned long)snap.wifi_disconnects,
             (unsigned long)snap.wifi_stack_resets,
             (unsigned long)snap.business_ws_connects,
             (unsigned long)snap.business_ws_disconnects,
             (unsigned long)snap.business_ws_errors,
             (unsigned long)snap.low_mem_events);
}

static void system_task(void *arg)
{
    (void)arg;
    while (true) {
        handle_ai_activity_pause();
        handle_low_memory_guard();
        start_business_ws_if_needed();
        log_health_if_due();
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_TASK_PERIOD_MS));
    }
}

void otakulink_system_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    lock_state();
    s_state.initialized = true;
    unlock_state();

    if (!s_system_task) {
        xTaskCreate(system_task, "otakulink_sys", 4096, NULL, 4, &s_system_task);
    }
}

void otakulink_system_set_business_ws_message_cb(otakulink_business_ws_message_cb_t cb)
{
    s_business_ws_message_cb = cb;
}

esp_err_t otakulink_business_ws_send_text(const char *text)
{
    if (!text || !s_business_ws) {
        return ESP_ERR_INVALID_STATE;
    }

    otakulink_system_state_t snap;
    copy_state(&snap);
    if (!snap.business_ws_connected || snap.business_ws_paused) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = esp_websocket_client_send_text(s_business_ws, text, strlen(text), pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGW(TAG, "[业务WS] 发送失败 长度=%u", (unsigned)strlen(text));
        return ESP_FAIL;
    }
    return ESP_OK;
}

void otakulink_system_set_business_ws_config(const otakulink_business_ws_config_t *config)
{
    if (!config || !config->device_id || !config->api_key || config->api_key[0] == '\0') {
        ESP_LOGW(TAG, "[业务WS] 配置已忽略：缺少device_id或api_key");
        return;
    }

    char uri[sizeof(s_state.uri)];
    build_business_ws_uri(config, uri, sizeof(uri));

    lock_state();
    strlcpy(s_state.uri, uri, sizeof(s_state.uri));
    schedule_business_ws_reconnect_locked(WS_RECONNECT_DELAY_MS, "配置已更新");
    unlock_state();

    ESP_LOGI(TAG, "[业务WS] 配置已更新 设备=%s key=%.4s****",
             config->device_id, config->api_key);
}

void otakulink_system_on_wifi_got_ip(void)
{
    lock_state();
    s_state.wifi_connected = true;
    if (s_state.business_ws_paused && s_state.network_recovery_active) {
        bool recovered = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > RISK_HEAP_BYTES;
        if (recovered) {
            s_state.network_recovery_active = false;
            s_state.risk_active = false;
            s_state.low_mem_since_ms = 0;
            pause_business_ws_locked(false, "WiFi已恢复");
        }
    }
    if (!s_state.business_ws_paused) {
        schedule_business_ws_reconnect_locked(WS_RECONNECT_DELAY_MS, "WiFi获取IP");
    }
    unlock_state();
}

void otakulink_system_on_wifi_disconnected(void)
{
    lock_state();
    s_state.wifi_connected = false;
    s_state.business_ws_connected = false;
    s_state.business_ws_started = false;
    s_state.wifi_disconnects++;
    unlock_state();
}

void otakulink_system_pause_business_ws_for_ai(const char *reason)
{
    bool should_stop = false;
    lock_state();
    s_state.ai_pause_active = true;
    s_state.ai_pause_until_ms = now_ms() + AI_WS_PAUSE_HOLD_MS;
    if (!s_state.business_ws_paused) {
        pause_business_ws_locked(true, reason ? reason : "AI活动");
    }
    if (s_state.business_ws_started || s_state.business_ws_connected) {
        should_stop = true;
    }
    s_state.business_ws_connected = false;
    unlock_state();

    if (should_stop) {
        stop_business_ws(reason ? reason : "AI活动");
    }
}

bool otakulink_business_ws_is_connected(void)
{
    bool connected;
    lock_state();
    connected = s_state.business_ws_connected;
    unlock_state();
    return connected;
}

void otakulink_system_get_snapshot(otakulink_system_snapshot_t *out)
{
    if (!out) {
        return;
    }

    otakulink_system_state_t snap;
    copy_state(&snap);
    memset(out, 0, sizeof(*out));
    out->free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->min_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->max_sram_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->rssi = get_wifi_rssi();
    out->wifi_connected = snap.wifi_connected;
    out->ai_ws_connected = esp_ai_is_connected();
    out->business_ws_connected = snap.business_ws_connected;
    out->business_ws_paused = snap.business_ws_paused;
    out->upload_active = esp_ai_get_upload_active();
    out->network_recovery_active = snap.network_recovery_active;
    out->wifi_disconnects = snap.wifi_disconnects;
    out->wifi_stack_resets = snap.wifi_stack_resets;
    out->business_ws_connects = snap.business_ws_connects;
    out->business_ws_disconnects = snap.business_ws_disconnects;
    out->business_ws_errors = snap.business_ws_errors;
    out->low_mem_events = snap.low_mem_events;
}
