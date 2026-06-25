#include "otakulink_reminder.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp-ai.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define REMINDER_CAP 8
#define REMINDER_TEXT_MAX 384
#define REMINDER_PROMPT_MAX 640
#define REMINDER_ECHO_IGNORE_US (60LL * 1000LL * 1000LL)

typedef struct {
    bool active;
    int64_t due_us;
    char text[REMINDER_TEXT_MAX];
} reminder_item_t;

static const char *TAG = "OTAKULINK_REMINDER";
static reminder_item_t s_items[REMINDER_CAP];
static SemaphoreHandle_t s_lock;
static int64_t s_last_prompt_us;

static bool contains_any(const char *s, const char *const *words, size_t count)
{
    if (!s) return false;
    for (size_t i = 0; i < count; ++i) {
        if (strstr(s, words[i])) return true;
    }
    return false;
}

static bool looks_like_reminder_prompt_echo(const char *text)
{
    if (!text) return false;
    if (strstr(text, "你需要提醒我") || strstr(text, "只输出提醒内容") || strstr(text, "请以你当前的人设")) {
        return true;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_prompt_us > 0 && now - s_last_prompt_us < REMINDER_ECHO_IGNORE_US && strstr(text, "提醒用户")) {
        return true;
    }
    return false;
}

static int chinese_digit_value(const char *p, size_t *bytes)
{
    struct map_item { const char *ch; int value; } map[] = {
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9}, {"十", 10},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        size_t len = strlen(map[i].ch);
        if (strncmp(p, map[i].ch, len) == 0) {
            if (bytes) *bytes = len;
            return map[i].value;
        }
    }
    return -1;
}

static int parse_chinese_number(const char *s)
{
    if (!s || !*s) return -1;
    int first = -1;
    int second = -1;
    size_t n = 0;
    first = chinese_digit_value(s, &n);
    if (first < 0) return -1;
    s += n;
    if (*s == '\0') return first;
    size_t n2 = 0;
    int maybe_ten = chinese_digit_value(s, &n2);
    if (maybe_ten == 10) {
        s += n2;
        if (*s == '\0') return first * 10;
        size_t n3 = 0;
        second = chinese_digit_value(s, &n3);
        if (second >= 0 && s[n3] == '\0') return first * 10 + second;
    }
    if (first == 10) {
        if (*s == '\0') return 10;
        second = chinese_digit_value(s, &n2);
        if (second >= 0 && s[n2] == '\0') return 10 + second;
    }
    return -1;
}

static bool parse_number_before_unit(const char *text, const char *unit, const char *after_limit, int *value, bool *half)
{
    const char *unit_pos = strstr(text, unit);
    if (!unit_pos || (after_limit && unit_pos > after_limit)) return false;

    const char *start = unit_pos;
    while (start > text) {
        const unsigned char c = (unsigned char)start[-1];
        if (isdigit(c)) {
            start--;
            continue;
        }
        break;
    }
    if (start < unit_pos) {
        char tmp[16];
        size_t len = (size_t)(unit_pos - start);
        if (len >= sizeof(tmp)) return false;
        memcpy(tmp, start, len);
        tmp[len] = '\0';
        int n = atoi(tmp);
        if (n > 0) {
            *value = n;
            *half = false;
            return true;
        }
    }

    const char *scan_start = unit_pos - 18;
    if (scan_start < text) scan_start = text;
    char prefix[32];
    size_t prefix_len = (size_t)(unit_pos - scan_start);
    if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
    memcpy(prefix, scan_start, prefix_len);
    prefix[prefix_len] = '\0';
    if (strstr(prefix, "半")) {
        *value = 1;
        *half = true;
        return true;
    }

    for (const char *p = scan_start; p < unit_pos; ++p) {
        int n = parse_chinese_number(p);
        if (n > 0) {
            *value = n;
            *half = false;
            return true;
        }
    }
    return false;
}

static bool parse_relative_seconds(const char *text, uint32_t *seconds)
{
    if (!text || !seconds) return false;
    const char *after = strstr(text, "后");
    if (!after && !strstr(text, "倒计时")) return false;

    int value = 0;
    bool half = false;
    if (parse_number_before_unit(text, "小时", after, &value, &half) ||
        parse_number_before_unit(text, "时", after, &value, &half)) {
        *seconds = half ? 1800U : (uint32_t)value * 3600U;
        return *seconds > 0;
    }
    if (parse_number_before_unit(text, "分钟", after, &value, &half) ||
        parse_number_before_unit(text, "分", after, &value, &half)) {
        *seconds = half ? 30U : (uint32_t)value * 60U;
        return *seconds > 0;
    }
    if (parse_number_before_unit(text, "秒", after, &value, &half)) {
        *seconds = half ? 1U : (uint32_t)value;
        return *seconds > 0;
    }
    return false;
}

static bool parse_cron_delay_seconds(const char *cron_expr, uint32_t *seconds)
{
    if (!cron_expr || !seconds) return false;
    int sec = 0;
    int min = 0;
    int hour = 0;
    if (sscanf(cron_expr, "*/%d", &sec) == 1 && sec > 0) {
        *seconds = (uint32_t)sec;
        return true;
    }
    if (sscanf(cron_expr, "0 */%d", &min) == 1 && min > 0) {
        *seconds = (uint32_t)min * 60U;
        return true;
    }
    if (sscanf(cron_expr, "0 0 */%d", &hour) == 1 && hour > 0) {
        *seconds = (uint32_t)hour * 3600U;
        return true;
    }
    return false;
}

static bool has_reminder_intent(const char *text)
{
    static const char *const triggers[] = {"提醒我", "提醒", "闹钟", "定时", "计时", "叫我", "倒计时"};
    return contains_any(text, triggers, sizeof(triggers) / sizeof(triggers[0]));
}

static bool schedule_relative_locked(const char *text, uint32_t seconds)
{
    if (!text || !*text || seconds == 0) return false;
    for (size_t i = 0; i < REMINDER_CAP; ++i) {
        if (!s_items[i].active) {
            s_items[i].active = true;
            s_items[i].due_us = esp_timer_get_time() + (int64_t)seconds * 1000000LL;
            strlcpy(s_items[i].text, text, sizeof(s_items[i].text));
            ESP_LOGI(TAG, "提醒已创建: slot=%u +%us text=%s", (unsigned)i, (unsigned)seconds, s_items[i].text);
            return true;
        }
    }
    ESP_LOGW(TAG, "提醒创建失败: 槽位已满 text=%s", text);
    return false;
}

static bool schedule_relative(const char *text, uint32_t seconds)
{
    if (!s_lock) return false;
    bool ok = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        ok = schedule_relative_locked(text, seconds);
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGW(TAG, "提醒创建失败: lock timeout");
    }
    return ok;
}

static void reminder_prompt_task(void *arg)
{
    char *text = (char *)arg;
    if (!text || !*text) {
        if (text) heap_caps_free(text);
        vTaskDelete(NULL);
        return;
    }
    if (!esp_ai_is_connected()) {
        ESP_LOGW(TAG, "提醒到点但 AI 未连接，跳过播报: %s", text);
        heap_caps_free(text);
        return;
    }
    if (esp_ai_get_busy()) {
        ESP_LOGW(TAG, "提醒到点但 AI 正忙，跳过播报: %s", text);
        heap_caps_free(text);
        return;
    }

    char *prompt = heap_caps_malloc(REMINDER_PROMPT_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!prompt) {
        ESP_LOGW(TAG, "提醒播报失败: prompt no memory");
        heap_caps_free(text);
        return;
    }
    snprintf(prompt, REMINDER_PROMPT_MAX, "请用一句15到30字的中文口语提醒用户：%s。只输出提醒内容，不要解释，不要设置新的提醒。", text);
    esp_err_t err = esp_ai_tts(prompt);
    if (err == ESP_OK) {
        s_last_prompt_us = esp_timer_get_time();
        ESP_LOGI(TAG, "提醒播报请求已发送: %s", text);
    } else {
        ESP_LOGW(TAG, "提醒播报发送失败: %s err=%s", text, esp_err_to_name(err));
    }
    heap_caps_free(prompt);
    heap_caps_free(text);
    vTaskDelete(NULL);
}

static void schedule_reminder_prompt_task(const char *text)
{
    if (!text || !*text) return;
    char *copy = heap_caps_malloc(REMINDER_TEXT_MAX, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) {
        ESP_LOGW(TAG, "提醒播报任务创建失败: text no memory");
        return;
    }
    strlcpy(copy, text, REMINDER_TEXT_MAX);
    BaseType_t ok = xTaskCreate(reminder_prompt_task, "reminder_tts", 6144, copy, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "提醒播报任务创建失败");
        heap_caps_free(copy);
    }
}

void otakulink_reminder_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_items, 0, sizeof(s_items));
        s_last_prompt_us = 0;
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "提醒模块初始化完成 cap=%d", REMINDER_CAP);
}

void otakulink_reminder_tick(void)
{
    if (!s_lock) return;
    char due_text[REMINDER_TEXT_MAX];
    due_text[0] = '\0';
    int due_slot = -1;
    int64_t now = esp_timer_get_time();

    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return;
    for (size_t i = 0; i < REMINDER_CAP; ++i) {
        if (s_items[i].active && now >= s_items[i].due_us) {
            strlcpy(due_text, s_items[i].text, sizeof(due_text));
            s_items[i].active = false;
            s_items[i].text[0] = '\0';
            due_slot = (int)i;
            break;
        }
    }
    xSemaphoreGive(s_lock);

    if (due_slot >= 0 && due_text[0]) {
        ESP_LOGI(TAG, "提醒触发: slot=%d text=%s", due_slot, due_text);
        schedule_reminder_prompt_task(due_text);
    }
}

bool otakulink_reminder_try_create_from_text(const char *text)
{
    if (!text || !*text) return false;
    if (looks_like_reminder_prompt_echo(text)) {
        ESP_LOGI(TAG, "忽略递归提醒文本: %s", text);
        return false;
    }
    if (!has_reminder_intent(text)) return false;

    uint32_t seconds = 0;
    if (!parse_relative_seconds(text, &seconds) || seconds == 0) {
        return false;
    }
    return schedule_relative(text, seconds);
}

bool otakulink_reminder_handle_cron_task(cJSON *data)
{
    cJSON *owned = NULL;
    cJSON *obj = data;
    if (cJSON_IsString(data)) {
        owned = cJSON_Parse(data->valuestring);
        obj = owned;
    }
    if (!cJSON_IsObject(obj)) {
        ESP_LOGW(TAG, "cron_task 数据不是对象");
        if (owned) cJSON_Delete(owned);
        return false;
    }

    cJSON *payload = obj;
    cJSON *data_obj = cJSON_GetObjectItem(obj, "data");
    if (cJSON_IsObject(data_obj)) {
        payload = data_obj;
    }

    cJSON *text_item = cJSON_GetObjectItem(payload, "text");
    cJSON *clock_type_item = cJSON_GetObjectItem(payload, "clock_type");
    cJSON *cron_item = cJSON_GetObjectItem(payload, "cron");
    const char *text = cJSON_IsString(text_item) ? text_item->valuestring : "";
    const char *clock_type = cJSON_IsString(clock_type_item) ? clock_type_item->valuestring : "";
    const char *cron_expr = cJSON_IsString(cron_item) ? cron_item->valuestring : "";

    ESP_LOGI(TAG, "收到 cron_task: text=%s clock_type=%s cron=%s", text, clock_type, cron_expr);
    if (looks_like_reminder_prompt_echo(text)) {
        ESP_LOGI(TAG, "忽略递归 cron_task: %s", text);
        if (owned) cJSON_Delete(owned);
        return false;
    }

    uint32_t seconds = 0;
    bool ok = false;
    if (strcmp(clock_type, "2") == 0 && parse_relative_seconds(text, &seconds) && seconds > 0) {
        ok = schedule_relative(text, seconds);
    } else if (parse_relative_seconds(text, &seconds) && seconds > 0) {
        ok = schedule_relative(text, seconds);
    } else if (parse_cron_delay_seconds(cron_expr, &seconds) && seconds > 0) {
        ok = schedule_relative(text[0] ? text : cron_expr, seconds);
    } else {
        ESP_LOGW(TAG, "cron_task 暂不支持该时间格式: text=%s cron=%s", text, cron_expr);
    }

    if (owned) cJSON_Delete(owned);
    return ok;
}
