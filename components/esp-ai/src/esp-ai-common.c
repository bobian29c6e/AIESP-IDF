/* 全局状态管理 (待后续补全) */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"

static bool s_is_busy = false;
static bool s_iat_active = false;
static bool s_start_send_audio = false;
static char s_session_id[64] = {0};
static float s_volume = 0.7f;
static int64_t s_busy_cleared_at_us = 0;

bool esp_ai_get_busy(void) { return s_is_busy; }
void esp_ai_set_busy(bool busy)
{
    bool was_busy = s_is_busy;
    s_is_busy = busy;
    if (was_busy && !busy) {
        s_busy_cleared_at_us = esp_timer_get_time();
    }
}

int64_t esp_ai_get_busy_cleared_at_us(void) { return s_busy_cleared_at_us; }

bool esp_ai_is_iat_active(void) { return s_iat_active; }

void esp_ai_set_iat_active(bool active)
{
    s_iat_active = active;
    if (!active) {
        s_start_send_audio = false;
        s_session_id[0] = '\0';
    }
}

void esp_ai_set_session_id(const char* session_id)
{
    if (!session_id || session_id[0] == '\0') {
        s_session_id[0] = '\0';
        return;
    }
    snprintf(s_session_id, sizeof(s_session_id), "%s", session_id);
}

void esp_ai_set_audio_upload_enabled(bool enabled, const char* reason)
{
    (void)reason;
    if (enabled && s_session_id[0] == '\0') {
        enabled = false;
    }
    s_start_send_audio = enabled;
}

bool esp_ai_should_send_audio(void)
{
    return s_iat_active && s_start_send_audio && s_session_id[0] != '\0';
}

float esp_ai_get_volume(void) { return s_volume; }

void esp_ai_set_volume(float volume)
{
    if (volume < 0.0f) {
        volume = 0.0f;
    } else if (volume > 1.0f) {
        volume = 1.0f;
    }
    s_volume = volume;
}
