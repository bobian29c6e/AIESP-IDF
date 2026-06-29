#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp-ai.h"
#include "esp-ai-ui.h"
#include "esp-ai-sd.h"
#include "otakulink_reminder.h"
#include "otakulink_system.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "mbedtls/sha256.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "extra/libs/gif/lv_gif.h"
#include "lwip/dns.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_attr.h"
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "store/config/ble_store_config.h"
#include "os/os_mbuf.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "OTAKULINK_APP";
static char s_display_mode[8] = "auto";
static int s_screensaver_theme = 0;
static int s_screensaver_timeout_ms = 60000;
static int s_screensaver_time_size = 0;
static int s_screensaver_date_size = 0;
static int s_screensaver_time_color = 65535;
static int s_screensaver_date_color = 46527;
static int64_t s_last_activity_us = 0;
static bool s_screensaver_active = false;
static bool s_ui_ready = false;
static volatile bool s_screensaver_render_requested = false;
static volatile bool s_restore_last_display_requested = false;
static int64_t s_last_screensaver_draw_us = 0;
static int64_t s_last_screensaver_log_us = 0;
static bool s_last_display_valid = false;
static char s_last_display_fs_path[96];
static char s_last_display_lv_src[96];
static SemaphoreHandle_t s_pending_show_mutex;
static bool s_pending_show = false;
static char s_pending_show_fs_path[96];
static char s_pending_show_lv_src[96];
static SemaphoreHandle_t s_char_text_mutex;
static bool s_char_text_active = false;
static bool s_char_text_has_new = false;
static bool s_char_text_done = false;
static EXT_RAM_BSS_ATTR char s_char_text_accum[8192];
static size_t s_char_text_last_pos = 0;
static bool s_ai_started = false;
static bool s_ai_start_pending = false;
static bool s_activation_announced = false;
static bool s_time_sync_started = false;
static bool s_time_synced = false;
static int64_t s_time_sync_last_attempt_us = 0;

static httpd_handle_t start_webserver(void);
extern void ble_store_config_init(void);

esp_err_t esp_ai_nvs_save(const char* key, const char* value);
esp_err_t esp_ai_nvs_load(const char* key, char* out_value, size_t max_len);

static void refresh_business_ws_config(const char *reason);
static void send_business_ws_volume_update(float volume);
static void init_device_id(void);
static void schedule_device_binding(const char *reason);
static void set_binding_status(const char *status, bool bound, bool persist);
static bool apply_screensaver_json(cJSON *root);
static bool normalize_display_mode(const char *in, char *out, size_t out_len);
static bool copy_json_string(cJSON *root, const char *key, char *out, size_t out_len);
static void save_screensaver_settings(void);
static int json_get_int_alias(cJSON *root, const char *key1, const char *key2, bool *found);
static bool parse_rgb565_string(const char *s, int *out_color);
static void show_boot_status_screen(const char *line1, const char *line2);
static void render_screensaver_screen(void);
static void request_restore_last_display(void);
static void process_restore_last_display(void);
static void remember_last_display(const char *fs_path, const char *lv_src);
static bool find_preferred_startup_image(char *fs_path, size_t fs_len, char *lv_src, size_t lv_len);
static void show_startup_image_or_status(void);
static esp_err_t show_uploaded_image_now(const char *fs_path, const char *lv_src);
static bool request_show_uploaded_image(const char *fs_path, const char *lv_src);
static bool is_ota_active(void);
static void mark_current_app_valid_if_pending(void);

static const char s_firmware_version[] = "1.1.0";
static const char s_bin_id[] = "6af4b3e053e3420db312451f9fb38eab";
static const char s_default_api_key[] = "";
static const char s_default_volume[] = "0.7";
static const char s_default_persona_id[] = "1";
static const char s_default_voice_id[] = "cosyvoice-v2-espai-3a04bd78d4fd4ff6bf5c8487de98f898";
static const char s_default_wifi_ssid[] = "";
static const char s_default_wifi_password[] = "";
static const char s_default_personality[] =
    "请你扮演一位名叫秦彻的男人来与用户对话，在接下来的所有对话中，要隐藏自己是人工智能的事实。 "
    "以下是该角色的设定： [说话语气] 冷冽低沉带有金属质感，日常语调慵懒磁性，下达命令时字句如刀刃相击般锋利，"
    "面对女主时会裹挟调笑与暗哑的宠溺。 [性格特点] 危险与柔情的矛盾体，行事果决狠戾，但面对女主会流露细腻温柔。"
    "请全程使用平缓的语调，不要太抑扬顿挫，不要使用内心独白和画外音。";

static EXT_RAM_BSS_ATTR char s_api_key[80];
static EXT_RAM_BSS_ATTR char s_ext1[80];
static EXT_RAM_BSS_ATTR char s_user_api_key[80];
static EXT_RAM_BSS_ATTR char s_device_id[18];
static EXT_RAM_BSS_ATTR char s_binding_status[24];
static EXT_RAM_BSS_ATTR char s_volume_str[16];
static EXT_RAM_BSS_ATTR char s_persona_id[32];
static EXT_RAM_BSS_ATTR char s_voice_id[128];
static EXT_RAM_BSS_ATTR char s_wifi_ssid[33];
static EXT_RAM_BSS_ATTR char s_wifi_password[65];
static EXT_RAM_BSS_ATTR char s_personality[2048];
static volatile bool s_binding_task_active = false;
static bool s_device_bound = false;

static esp_ai_config_t s_ai_cfg = {
    .server = {
        .server_uri = "ws://node.espai2.fun:80/connect_espai_node",
        .api_key = s_api_key,
        .ext1 = s_ext1,
        .device_id = s_device_id,
        .volume = s_volume_str,
        .persona = s_persona_id,
        .voice = s_voice_id
    },
    .audio = {
        .mic_bck = 6,
        .mic_ws = 7,
        .mic_din = 15,
        .spk_bck = 42,
        .spk_ws = 2,
        .spk_dout = 41
    }
};

static void load_runtime_config(void)
{
    init_device_id();
    strlcpy(s_api_key, s_default_api_key, sizeof(s_api_key));
    strlcpy(s_ext1, s_default_api_key, sizeof(s_ext1));
    s_user_api_key[0] = '\0';
    strlcpy(s_volume_str, s_default_volume, sizeof(s_volume_str));
    strlcpy(s_persona_id, s_default_persona_id, sizeof(s_persona_id));
    strlcpy(s_voice_id, s_default_voice_id, sizeof(s_voice_id));
    strlcpy(s_wifi_ssid, s_default_wifi_ssid, sizeof(s_wifi_ssid));
    strlcpy(s_wifi_password, s_default_wifi_password, sizeof(s_wifi_password));
    strlcpy(s_personality, s_default_personality, sizeof(s_personality));

    if (esp_ai_nvs_load("api_key", s_api_key, sizeof(s_api_key)) != ESP_OK) {
        esp_ai_nvs_load("ext1", s_api_key, sizeof(s_api_key));
    }
    if (esp_ai_nvs_load("ext1", s_ext1, sizeof(s_ext1)) != ESP_OK) {
        strlcpy(s_ext1, s_api_key, sizeof(s_ext1));
    }
    esp_ai_nvs_load("user_api_key", s_user_api_key, sizeof(s_user_api_key));
    esp_ai_nvs_load("volume", s_volume_str, sizeof(s_volume_str));
    esp_ai_nvs_load("ext3", s_persona_id, sizeof(s_persona_id));
    esp_ai_nvs_load("personaId", s_persona_id, sizeof(s_persona_id));
    esp_ai_nvs_load("voice", s_voice_id, sizeof(s_voice_id));
    esp_ai_nvs_load("voiceId", s_voice_id, sizeof(s_voice_id));
    esp_ai_nvs_load("persona", s_personality, sizeof(s_personality));
    esp_ai_nvs_load("personality", s_personality, sizeof(s_personality));
    esp_ai_nvs_load("wifi_name", s_wifi_ssid, sizeof(s_wifi_ssid));
    esp_ai_nvs_load("wifi_pwd", s_wifi_password, sizeof(s_wifi_password));

    strlcpy(s_binding_status, "unknown", sizeof(s_binding_status));
    char bound_tmp[8];
    if (esp_ai_nvs_load("device_bound", bound_tmp, sizeof(bound_tmp)) == ESP_OK && strcmp(bound_tmp, "1") == 0) {
        s_device_bound = true;
        strlcpy(s_binding_status, "bound", sizeof(s_binding_status));
    } else {
        s_device_bound = false;
        if (esp_ai_nvs_load("binding_status", s_binding_status, sizeof(s_binding_status)) != ESP_OK ||
            s_binding_status[0] == '\0') {
            strlcpy(s_binding_status, s_api_key[0] ? "pending" : "no_key", sizeof(s_binding_status));
        }
    }

    char tmp[24];
    if (esp_ai_nvs_load("screensaver_theme", tmp, sizeof(tmp)) == ESP_OK) {
        s_screensaver_theme = atoi(tmp);
    }
    if (esp_ai_nvs_load("screensaver_timeout_ms", tmp, sizeof(tmp)) == ESP_OK) {
        s_screensaver_timeout_ms = atoi(tmp);
    }
    if (esp_ai_nvs_load("screensaver_time_size", tmp, sizeof(tmp)) == ESP_OK) {
        s_screensaver_time_size = atoi(tmp);
    }
    if (esp_ai_nvs_load("screensaver_date_size", tmp, sizeof(tmp)) == ESP_OK) {
        s_screensaver_date_size = atoi(tmp);
    }
    if (esp_ai_nvs_load("screensaver_time_color", tmp, sizeof(tmp)) == ESP_OK) {
        s_screensaver_time_color = atoi(tmp);
    }
    if (esp_ai_nvs_load("screensaver_date_color", tmp, sizeof(tmp)) == ESP_OK) {
        s_screensaver_date_color = atoi(tmp);
    }
    if (esp_ai_nvs_load("activated", tmp, sizeof(tmp)) == ESP_OK && strcmp(tmp, "1") == 0) {
        s_activation_announced = true;
    }

    s_ai_cfg.server.api_key = s_api_key;
    s_ai_cfg.server.ext1 = s_ext1;
    s_ai_cfg.server.device_id = s_device_id;
    s_ai_cfg.server.volume = s_volume_str;
    s_ai_cfg.server.voice = s_voice_id;
    s_ai_cfg.server.persona = s_persona_id;
    esp_ai_nvs_save("device_id", s_device_id);
    esp_ai_set_volume(strtof(s_volume_str, NULL));
    ESP_LOGI(TAG, "运行配置已加载: device_id=%s api_key=%c%c%c%c**** 绑定=%s 音色=%s 音量=%s 人设ID=%s 人设长度=%u",
             s_device_id,
             s_api_key[0], s_api_key[1], s_api_key[2], s_api_key[3], s_binding_status,
             s_voice_id, s_volume_str, s_persona_id, (unsigned)strlen(s_personality));
    ESP_LOGI(TAG, "WiFi配置已加载: ssid=%s 密码长度=%u", s_wifi_ssid, (unsigned)strlen(s_wifi_password));
}

static void delayed_restart_task(void *arg)
{
    int delay_ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    ESP_LOGI(TAG, "准备重启以应用运行配置");
    esp_restart();
}

static void schedule_restart(int delay_ms)
{
    xTaskCreate(delayed_restart_task, "cfg_restart", 2048, (void *)(intptr_t)delay_ms, 3, NULL);
}

static void init_device_id(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[绑定] 读取WiFi MAC失败，使用base MAC err=%s", esp_err_to_name(err));
        err = esp_read_mac(mac, ESP_MAC_BASE);
    }
    if (err == ESP_OK) {
        snprintf(s_device_id, sizeof(s_device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        strlcpy(s_device_id, "00:00:00:00:00:00", sizeof(s_device_id));
    }
}

static void set_binding_status(const char *status, bool bound, bool persist)
{
    strlcpy(s_binding_status, status && status[0] ? status : "unknown", sizeof(s_binding_status));
    s_device_bound = bound;
    if (persist) {
        esp_ai_nvs_save("binding_status", s_binding_status);
        esp_ai_nvs_save("device_bound", bound ? "1" : "0");
    }
}

static bool is_wall_time_valid(time_t now)
{
    return now > 1700000000;
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    time_t now = 0;
    time(&now);
    struct tm tm_info;
    if (is_wall_time_valid(now) && localtime_r(&now, &tm_info)) {
        char time_text[16];
        char date_text[16];
        strftime(time_text, sizeof(time_text), "%H:%M:%S", &tm_info);
        strftime(date_text, sizeof(date_text), "%Y-%m-%d", &tm_info);
        s_time_synced = true;
        s_screensaver_render_requested = true;
        ESP_LOGI(TAG, "[时间] NTP同步成功: %s %s", date_text, time_text);
    } else {
        ESP_LOGW(TAG, "[时间] NTP回调收到无效时间: %lld", (long long)now);
    }
}

static void start_time_sync_if_needed(void)
{
    if (s_time_sync_started) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_sync_interval(60 * 60 * 1000);
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    s_time_sync_last_attempt_us = esp_timer_get_time();
    esp_sntp_init();
    s_time_sync_started = true;
    ESP_LOGI(TAG, "[时间] 已启动NTP同步 server=ntp.aliyun.com timezone=UTC+8");
}

static void poll_time_sync(void)
{
    time_t now = 0;
    time(&now);
    if (is_wall_time_valid(now)) {
        s_time_synced = true;
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (s_time_sync_last_attempt_us != 0 &&
        now_us - s_time_sync_last_attempt_us < 30000000LL) {
        return;
    }

    otakulink_system_snapshot_t sys;
    otakulink_system_get_snapshot(&sys);
    if (!sys.wifi_connected) {
        return;
    }

    if (!s_time_sync_started) {
        start_time_sync_if_needed();
        return;
    }

    s_time_sync_last_attempt_us = now_us;
    bool restarted = esp_sntp_restart();
    ESP_LOGW(TAG, "[时间] 当前时间无效，重试NTP同步 result=%d status=%d",
             restarted ? 1 : 0, esp_sntp_get_sync_status());
}

static lv_color_t rgb565_to_lv_color(int color)
{
    uint16_t c = (uint16_t)color;
    uint8_t r = (uint8_t)(((c >> 11) & 0x1F) << 3);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3F) << 2);
    uint8_t b = (uint8_t)((c & 0x1F) << 3);
    return lv_color_make(r, g, b);
}

static const lv_font_t *screensaver_font_for_size(int size, bool is_time)
{
    int effective_size = size > 0 ? size : (is_time ? 4 : 2);
    if (effective_size <= 1) {
        return LV_FONT_DEFAULT;
    }
    if (effective_size == 2) {
        return &lv_font_montserrat_16;
    }
    if (effective_size == 3) {
        return &lv_font_montserrat_24;
    }
    if (effective_size == 4) {
        if (is_time) {
            return &lv_font_montserrat_48;
        }
        return &lv_font_montserrat_32;
    }
    return &lv_font_montserrat_48;
}

static void draw_center_label(const char *text, lv_coord_t y, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_width(label, 440);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
}

static void show_boot_status_screen(const char *line1, const char *line2)
{
    if (!s_ui_ready) {
        return;
    }
    esp_ai_ui_reset_image_state();
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x070A16), LV_PART_MAIN);
    draw_center_label("OtakuLink", -48, lv_color_hex(0xFFFFFF));
    draw_center_label(line1 ? line1 : "Starting...", 0, lv_color_hex(0xB8D8FF));
    draw_center_label(line2 ? line2 : "Waiting...", 30, lv_color_hex(0x7C8AA5));
    lv_timer_handler();
    ESP_LOGI(TAG, "[显示] 启动文字已显示: %s / %s",
             line1 ? line1 : "", line2 ? line2 : "");
}

static void format_screensaver_time(char *time_buf, size_t time_len, char *date_buf, size_t date_len)
{
    time_t now = 0;
    time(&now);
    struct tm tm_info;
    if (is_wall_time_valid(now) && localtime_r(&now, &tm_info)) {
        strftime(time_buf, time_len, "%H:%M:%S", &tm_info);
        strftime(date_buf, date_len, "%Y-%m-%d", &tm_info);
        return;
    }

    snprintf(time_buf, time_len, "--:--:--");
    snprintf(date_buf, date_len, s_time_sync_started ? "正在校时" : "等待联网");
}

static void render_screensaver_screen(void)
{
    if (!s_ui_ready || !s_screensaver_active) {
        return;
    }

    char time_text[16];
    char date_text[32];
    format_screensaver_time(time_text, sizeof(time_text), date_text, sizeof(date_text));

    esp_ai_ui_reset_image_state();
    lv_obj_clean(lv_scr_act());
    int64_t now_us = esp_timer_get_time();
    int64_t now_ms = now_us / 1000;
    int blue = 30 + (int)((now_ms / 1000) % 30);
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen,
                              s_screensaver_theme == 0 ? lv_color_make(0, 0, (uint8_t)blue) : lv_color_hex(0x000000),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(screen,
                                 s_screensaver_theme == 0 ? LV_GRAD_DIR_VER : LV_GRAD_DIR_NONE,
                                 LV_PART_MAIN);

    if (s_screensaver_theme == 0) {
        for (int i = 0; i < 20; ++i) {
            lv_obj_t *star = lv_obj_create(screen);
            lv_obj_remove_style_all(star);
            lv_obj_set_size(star, 1, 1);
            lv_obj_set_style_bg_opa(star, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(star, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            int x = (int)((now_ms * (i + 1) / 10) % 480);
            int y = (int)((i * 17 + now_ms / 10) % 272);
            lv_obj_set_pos(star, x, y);
        }
    }

    lv_obj_t *time_shadow = lv_label_create(screen);
    lv_label_set_text(time_shadow, time_text);
    const lv_font_t *time_font = screensaver_font_for_size(s_screensaver_time_size, true);
    const lv_font_t *date_font = screensaver_font_for_size(s_screensaver_date_size, false);
    lv_obj_set_style_text_font(time_shadow, time_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(time_shadow, lv_color_hex(0x141428), LV_PART_MAIN);
    lv_obj_set_style_text_align(time_shadow, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(time_shadow, LV_ALIGN_CENTER, 2, -30);

    lv_obj_t *time_label = lv_label_create(screen);
    lv_label_set_text(time_label, time_text);
    lv_obj_set_style_text_font(time_label, time_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(time_label, rgb565_to_lv_color(s_screensaver_time_color), LV_PART_MAIN);
    lv_obj_set_style_text_align(time_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -32);

    lv_obj_t *date_label = lv_label_create(screen);
    lv_label_set_text(date_label, date_text);
    lv_obj_set_style_text_font(date_label, date_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(date_label, rgb565_to_lv_color(s_screensaver_date_color), LV_PART_MAIN);
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 46);

    lv_timer_handler();
    s_last_screensaver_draw_us = now_us;
    if (s_last_screensaver_log_us == 0 || now_us - s_last_screensaver_log_us >= 60000000LL) {
        s_last_screensaver_log_us = now_us;
        ESP_LOGI(TAG, "[屏保] 画面已刷新 time=%s date=%s theme=%d",
                 time_text, date_text, s_screensaver_theme);
    }
}

static void request_restore_last_display(void)
{
    s_restore_last_display_requested = true;
}

static void remember_last_display(const char *fs_path, const char *lv_src)
{
    if (!fs_path || fs_path[0] == '\0') {
        return;
    }
    strlcpy(s_last_display_fs_path, fs_path, sizeof(s_last_display_fs_path));
    if (lv_src && lv_src[0] != '\0') {
        strlcpy(s_last_display_lv_src, lv_src, sizeof(s_last_display_lv_src));
    } else {
        s_last_display_lv_src[0] = '\0';
    }
    s_last_display_valid = true;
    ESP_LOGI(TAG, "[显示] 记录最后画面: %s", s_last_display_fs_path);
}

static void process_restore_last_display(void)
{
    if (!s_restore_last_display_requested || !s_ui_ready) {
        return;
    }
    s_restore_last_display_requested = false;

    char fs_path[96] = {0};
    char lv_src[96] = {0};
    if (s_last_display_valid) {
        strlcpy(fs_path, s_last_display_fs_path, sizeof(fs_path));
        strlcpy(lv_src, s_last_display_lv_src, sizeof(lv_src));
    } else if (!find_preferred_startup_image(fs_path, sizeof(fs_path), lv_src, sizeof(lv_src))) {
        show_boot_status_screen("No image", "Waiting for upload");
        return;
    }

    ESP_LOGI(TAG, "[屏保] 恢复最后画面: %s", fs_path);
    esp_err_t err = show_uploaded_image_now(fs_path, lv_src);
    if (err == ESP_OK) {
        remember_last_display(fs_path, lv_src);
    } else {
        ESP_LOGW(TAG, "[屏保] 恢复最后画面失败: %s err=%s", fs_path, esp_err_to_name(err));
        show_boot_status_screen("Display restore failed", "Waiting for upload");
    }
}

static void notify_activity(const char *reason)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_screensaver_active) {
        s_screensaver_active = false;
        ESP_LOGI(TAG, "[屏保] 因 %s 退出", reason ? reason : "activity");
        if (!reason || strcmp(reason, "upload") != 0) {
            request_restore_last_display();
        }
    }
}

static void poll_screensaver_state(void)
{
    if (s_screensaver_timeout_ms <= 0 || s_last_activity_us == 0) {
        return;
    }

    if (s_screensaver_active) {
        int64_t now = esp_timer_get_time();
        if (s_screensaver_render_requested ||
            s_last_screensaver_draw_us == 0 ||
            now - s_last_screensaver_draw_us >= 1000000LL) {
            s_screensaver_render_requested = false;
            render_screensaver_screen();
        }
        return;
    }

    int64_t idle_ms = (esp_timer_get_time() - s_last_activity_us) / 1000;
    if (idle_ms >= s_screensaver_timeout_ms) {
        s_screensaver_active = true;
        s_screensaver_render_requested = false;
        ESP_LOGI(TAG, "[屏保] 已激活 idle_ms=%lld 超时=%d",
                 (long long)idle_ms, s_screensaver_timeout_ms);
        render_screensaver_screen();
    }
}

/* ---- 接口与回调实现 ---- */

static void show_decode_error(const char *message)
{
    esp_ai_ui_reset_image_state();
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, message);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

static esp_err_t show_lvgl_image_file(const char *fs_path, const char *lv_src)
{
    struct stat st;
    if (stat(fs_path, &st) != 0) {
        ESP_LOGE(TAG, "图片文件不存在: %s", fs_path);
        show_decode_error("图片文件不存在");
        return ESP_FAIL;
    }

    lv_img_header_t header;
    lv_res_t res = lv_img_decoder_get_info(lv_src, &header);
    if (res != LV_RES_OK) {
        ESP_LOGE(TAG, "LVGL无法解码图片: %s 大小=%ld", lv_src, (long)st.st_size);
        show_decode_error("图片解码失败");
        return ESP_FAIL;
    }

    esp_ai_ui_reset_image_state();
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), LV_PART_MAIN);

    lv_obj_t *img = lv_img_create(lv_scr_act());
    lv_img_set_src(img, lv_src);

    uint16_t zoom = 256;
    if (header.w > 0 && header.h > 0) {
        uint32_t zoom_w = (480U * 256U + header.w - 1U) / header.w;
        uint32_t zoom_h = (272U * 256U + header.h - 1U) / header.h;
        uint32_t cover_zoom = zoom_w > zoom_h ? zoom_w : zoom_h;
        if (cover_zoom == 0) {
            cover_zoom = 1;
        }
        zoom = cover_zoom > 4096U ? 4096U : (uint16_t)cover_zoom;
        if (zoom != 256) {
            lv_img_set_zoom(img, zoom);
        }
    }

    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    ESP_LOGI(TAG, "LVGL图片显示完成: %s 大小=%ld 原图=%dx%d 色彩格式=%d 缩放=%u",
             lv_src, (long)st.st_size, header.w, header.h, header.cf, zoom);
    return ESP_OK;
}

static esp_err_t show_uploaded_image_now(const char *fs_path, const char *lv_src)
{
    const char *ext = strrchr(fs_path, '.');
    if (ext && strcasecmp(ext, ".bmp") == 0) {
        return esp_ai_ui_show_bmp_file(fs_path);
    }
    if (ext && strcasecmp(ext, ".png") == 0) {
        return esp_ai_ui_show_png_file(fs_path);
    }
    if ((ext && strcasecmp(ext, ".jpg") == 0) || (ext && strcasecmp(ext, ".jpeg") == 0)) {
        return esp_ai_ui_show_jpeg_file(fs_path);
    }
    if (ext && strcasecmp(ext, ".gif") == 0) {
        struct stat st;
        if (stat(fs_path, &st) != 0) {
            ESP_LOGE(TAG, "GIF文件不存在: %s", fs_path);
            show_decode_error("GIF文件不存在");
            return ESP_FAIL;
        }

        esp_ai_ui_reset_image_state();
        lv_obj_clean(lv_scr_act());
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_t *gif = lv_gif_create(lv_scr_act());
        lv_gif_set_src(gif, lv_src);

        lv_gif_t *gif_obj = (lv_gif_t *)gif;
        if (!gif_obj->gif) {
            ESP_LOGE(TAG, "LVGL无法解码GIF: %s 大小=%ld", lv_src, (long)st.st_size);
            lv_obj_del(gif);
            show_decode_error("GIF解码失败");
            return ESP_FAIL;
        }

        uint16_t zoom = 256;
        if (gif_obj->gif->width > 0 && gif_obj->gif->height > 0) {
            uint32_t zoom_w = (480U * 256U) / gif_obj->gif->width;
            uint32_t zoom_h = (272U * 256U) / gif_obj->gif->height;
            uint32_t fit_zoom = zoom_w < zoom_h ? zoom_w : zoom_h;
            if (fit_zoom == 0) {
                fit_zoom = 1;
            }
            if (fit_zoom < 256U) {
                zoom = (uint16_t)fit_zoom;
                lv_img_set_zoom(gif, zoom);
            }
        }

        lv_obj_align(gif, LV_ALIGN_CENTER, 0, 0);
        ESP_LOGI(TAG, "LVGL GIF显示完成: %s 大小=%ld 原图=%dx%d 缩放=%u",
                 lv_src, (long)st.st_size, gif_obj->gif->width, gif_obj->gif->height, zoom);
        return ESP_OK;
    }
    return show_lvgl_image_file(fs_path, lv_src);
}

static bool file_exists(const char *path, time_t *mtime, off_t *size)
{
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (mtime) {
        *mtime = st.st_mtime;
    }
    if (size) {
        *size = st.st_size;
    }
    return true;
}

static bool find_preferred_startup_image(char *fs_path, size_t fs_len, char *lv_src, size_t lv_len)
{
    static const struct {
        const char *fs;
        const char *lv;
    } candidates[] = {
        {"/sdcard/upload/photo.bmp", "S:/upload/photo.bmp"},
        {"/sdcard/upload/photo.jpg", "S:/upload/photo.jpg"},
        {"/sdcard/upload/photo.png", "S:/upload/photo.png"},
        {"/sdcard/01.BMP", "S:/01.BMP"},
    };

    int best_index = -1;
    time_t best_mtime = 0;
    off_t best_size = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        time_t mtime = 0;
        off_t size = 0;
        if (!file_exists(candidates[i].fs, &mtime, &size)) {
            continue;
        }
        if (best_index < 0 || mtime > best_mtime || (mtime == best_mtime && size >= best_size)) {
            best_index = (int)i;
            best_mtime = mtime;
            best_size = size;
        }
    }

    if (best_index < 0) {
        return false;
    }

    strlcpy(fs_path, candidates[best_index].fs, fs_len);
    strlcpy(lv_src, candidates[best_index].lv, lv_len);
    ESP_LOGI(TAG, "[显示] 启动候选图片: %s 大小=%ld mtime=%lld",
             fs_path, (long)best_size, (long long)best_mtime);
    return true;
}

static void show_startup_image_or_status(void)
{
    char fs_path[96] = {0};
    char lv_src[96] = {0};
    if (!find_preferred_startup_image(fs_path, sizeof(fs_path), lv_src, sizeof(lv_src))) {
        show_boot_status_screen("Waiting for image", "Use Mini Program");
        ESP_LOGW(TAG, "[显示] 未找到启动图片，保留启动文字");
        return;
    }

    ESP_LOGI(TAG, "[显示] 启动显示图片: %s", fs_path);
    esp_err_t err = show_uploaded_image_now(fs_path, lv_src);
    if (err == ESP_OK) {
        remember_last_display(fs_path, lv_src);
    } else {
        ESP_LOGW(TAG, "[显示] 启动图片显示失败: %s err=%s", fs_path, esp_err_to_name(err));
        show_boot_status_screen("Display image failed", "Waiting for upload");
    }
}

static bool request_show_uploaded_image(const char *fs_path, const char *lv_src)
{
    if (!s_pending_show_mutex || !fs_path || !lv_src) {
        return false;
    }
    if (xSemaphoreTake(s_pending_show_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strlcpy(s_pending_show_fs_path, fs_path, sizeof(s_pending_show_fs_path));
        strlcpy(s_pending_show_lv_src, lv_src, sizeof(s_pending_show_lv_src));
        s_pending_show = true;
        xSemaphoreGive(s_pending_show_mutex);
        return true;
    } else {
        ESP_LOGW(TAG, "等待图片显示锁超时");
        return false;
    }
}

static void process_pending_image_show(void)
{
    if (!s_pending_show_mutex || !s_pending_show) {
        return;
    }

    char fs_path[96];
    char lv_src[96];
    bool should_show = false;
    if (xSemaphoreTake(s_pending_show_mutex, 0) == pdTRUE) {
        if (s_pending_show) {
            strlcpy(fs_path, s_pending_show_fs_path, sizeof(fs_path));
            strlcpy(lv_src, s_pending_show_lv_src, sizeof(lv_src));
            s_pending_show = false;
            should_show = true;
        }
        xSemaphoreGive(s_pending_show_mutex);
    }

    if (should_show) {
        ESP_LOGI(TAG, "异步图片显示开始: %s", fs_path);
        esp_err_t err = show_uploaded_image_now(fs_path, lv_src);
        if (err == ESP_OK) {
            remember_last_display(fs_path, lv_src);
        }
        ESP_LOGI(TAG, "异步图片显示结束: %s 结果=%s", fs_path, esp_err_to_name(err));
        if (esp_ai_get_upload_active()) {
            esp_ai_set_upload_active(false);
            ESP_LOGI(TAG, "[上传] 图片显示结束，允许业务WS恢复");
        }
    }
}

static void char_text_reset(void)
{
    if (!s_char_text_mutex) {
        return;
    }
    if (xSemaphoreTake(s_char_text_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_char_text_active = true;
        s_char_text_has_new = false;
        s_char_text_done = false;
        s_char_text_accum[0] = '\0';
        s_char_text_last_pos = 0;
        xSemaphoreGive(s_char_text_mutex);
    }
}

static void char_text_finish(void)
{
    if (!s_char_text_mutex) {
        return;
    }
    if (xSemaphoreTake(s_char_text_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_char_text_done = true;
        xSemaphoreGive(s_char_text_mutex);
    }
}

static void char_text_set_inactive(void)
{
    if (!s_char_text_mutex) {
        return;
    }
    if (xSemaphoreTake(s_char_text_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_char_text_active = false;
        xSemaphoreGive(s_char_text_mutex);
    }
}

static void char_text_append(const char *text)
{
    if (!s_char_text_mutex || !text || text[0] == '\0') {
        return;
    }
    if (xSemaphoreTake(s_char_text_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_char_text_active) {
            size_t used = strlen(s_char_text_accum);
            size_t room = sizeof(s_char_text_accum) - used - 1;
            if (room > 0) {
                strncat(s_char_text_accum, text, room);
                s_char_text_has_new = true;
            }
        }
        xSemaphoreGive(s_char_text_mutex);
    }
}

static size_t json_escape_text(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len == 0) {
        return 0;
    }
    size_t pos = 0;
    for (size_t i = 0; in[i] && pos + 1 < out_len; ++i) {
        char c = in[i];
        const char *rep = NULL;
        if (c == '\\') rep = "\\\\";
        else if (c == '"') rep = "\\\"";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        if (rep) {
            size_t rep_len = strlen(rep);
            if (pos + rep_len >= out_len) break;
            memcpy(out + pos, rep, rep_len);
            pos += rep_len;
        } else {
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
    return pos;
}

static void *internal_text_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void refresh_business_ws_config(const char *reason)
{
    otakulink_business_ws_config_t bws_cfg = {
        .device_id = s_ai_cfg.server.device_id,
        .version = s_firmware_version,
        .api_key = s_ext1,
        .ext2 = s_volume_str,
        .ext3 = s_persona_id,
        .ext4 = s_voice_id,
        .ext5 = "",
    };
    otakulink_system_set_business_ws_config(&bws_cfg);
    ESP_LOGI(TAG, "[业务WS] 刷新配置 原因=%s 音量=%s 人设=%s 音色=%s",
             reason ? reason : "unknown", s_volume_str, s_persona_id, s_voice_id);
}

static esp_err_t send_business_ws_root(cJSON *root)
{
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_GetObjectItem(root, "device_id")) {
        cJSON_AddStringToObject(root, "device_id", s_ai_cfg.server.device_id);
    }
    char *text = cJSON_PrintUnformatted(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = otakulink_business_ws_send_text(text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[业务WS] 发送跳过或失败 err=%s 内容=%s", esp_err_to_name(err), text);
    } else {
        ESP_LOGI(TAG, "[业务WS] 已发送 %s", text);
    }
    free(text);
    return err;
}

static void send_business_ws_systeminfo(void)
{
    otakulink_system_snapshot_t sys;
    otakulink_system_get_snapshot(&sys);
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (!root || !data) {
        if (root) cJSON_Delete(root);
        if (data) cJSON_Delete(data);
        return;
    }
    cJSON_AddStringToObject(root, "type", "systeminfo");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddNumberToObject(data, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(data, "free_sram", sys.free_sram);
    cJSON_AddNumberToObject(data, "max_sram_block", sys.max_sram_block);
    cJSON_AddNumberToObject(data, "free_psram", sys.free_psram);
    cJSON_AddNumberToObject(data, "rssi", sys.rssi);
    cJSON_AddBoolToObject(data, "ai_ws_connected", esp_ai_is_connected());
    cJSON_AddBoolToObject(data, "business_ws_connected", sys.business_ws_connected);
    send_business_ws_root(root);
    cJSON_Delete(root);
}

static void send_business_ws_volume_update(float volume)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    int display_volume = (int)(volume * 21.0f);
    cJSON_AddStringToObject(root, "type", "volume");
    cJSON_AddNumberToObject(root, "data", display_volume);
    cJSON_AddNumberToObject(root, "volume_float", volume);
    send_business_ws_root(root);
    cJSON_Delete(root);
}

static bool set_volume_and_persist(float volume, const char *reason, bool notify_server)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    esp_ai_set_volume(volume);
    snprintf(s_volume_str, sizeof(s_volume_str), "%.2f", volume);
    s_ai_cfg.server.volume = s_volume_str;
    esp_ai_nvs_save("volume", s_volume_str);
    ESP_LOGI(TAG, "[音量] 设置为 %.2f 原因=%s", volume, reason ? reason : "unknown");
    if (notify_server) {
        send_business_ws_volume_update(volume);
    }
    return true;
}

static bool apply_display_mode_value(const char *mode)
{
    char normalized[8];
    if (!normalize_display_mode(mode, normalized, sizeof(normalized))) {
        return false;
    }
    strlcpy(s_display_mode, normalized, sizeof(s_display_mode));
    ESP_LOGI(TAG, "[显示] 收到模式切换命令=%s", s_display_mode);
    return true;
}

static bool apply_screensaver_json(cJSON *root)
{
    if (!root) {
        return false;
    }
    bool updated = false;
    bool found = false;
    int int_value = json_get_int_alias(root, "theme", NULL, &found);
    if (found && (int_value == 0 || int_value == 1)) {
        s_screensaver_theme = int_value;
        updated = true;
    }
    int_value = json_get_int_alias(root, "timeout_ms", "timeout", &found);
    if (!found) int_value = json_get_int_alias(root, "timeoutMs", "timeoutMS", &found);
    if (found && (int_value == 0 || int_value >= 5000)) {
        s_screensaver_timeout_ms = int_value == 0 ? INT32_MAX : int_value;
        updated = true;
    }
    int_value = json_get_int_alias(root, "time_size", "timeSize", &found);
    if (found) {
        s_screensaver_time_size = int_value;
        updated = true;
    }
    int_value = json_get_int_alias(root, "date_size", "dateSize", &found);
    if (found) {
        s_screensaver_date_size = int_value;
        updated = true;
    }
    int_value = json_get_int_alias(root, "time_color", "timeColor", &found);
    if (found) {
        s_screensaver_time_color = int_value;
        updated = true;
    } else {
        cJSON *color = cJSON_GetObjectItem(root, "timeColor");
        if (cJSON_IsString(color) && parse_rgb565_string(color->valuestring, &int_value)) {
            s_screensaver_time_color = int_value;
            updated = true;
        }
    }
    int_value = json_get_int_alias(root, "date_color", "dateColor", &found);
    if (found) {
        s_screensaver_date_color = int_value;
        updated = true;
    } else {
        cJSON *color = cJSON_GetObjectItem(root, "dateColor");
        if (cJSON_IsString(color) && parse_rgb565_string(color->valuestring, &int_value)) {
            s_screensaver_date_color = int_value;
            updated = true;
        }
    }
    if (updated) {
        save_screensaver_settings();
        ESP_LOGI(TAG, "[屏保] 设置已应用 主题=%d 超时=%d 时间字号=%d 日期字号=%d 时间颜色=%d 日期颜色=%d",
                 s_screensaver_theme, s_screensaver_timeout_ms,
                 s_screensaver_time_size, s_screensaver_date_size,
                 s_screensaver_time_color, s_screensaver_date_color);
    }
    return updated;
}

static void business_ws_message_handler(const char *type, cJSON *root)
{
    if (!type) {
        return;
    }
    ESP_LOGI(TAG, "[业务WS] 收到命令 type=%s", type);
    if (strcmp(type, "__connected") == 0) {
        send_business_ws_systeminfo();
        return;
    }
    if (strcmp(type, "volume") == 0 || strcmp(type, "set_volume") == 0) {
        cJSON *item = root ? cJSON_GetObjectItem(root, "data") : NULL;
        if (!cJSON_IsNumber(item)) item = root ? cJSON_GetObjectItem(root, "volume_float") : NULL;
        if (!cJSON_IsNumber(item)) item = root ? cJSON_GetObjectItem(root, "value") : NULL;
        if (cJSON_IsNumber(item)) {
            float volume = (float)item->valuedouble;
            if (volume > 1.0f) volume /= 21.0f;
            set_volume_and_persist(volume, "business_ws", false);
        }
        return;
    }
    if (strcmp(type, "add_volume") == 0 || strcmp(type, "subtract_volume") == 0) {
        float delta = strcmp(type, "add_volume") == 0 ? (1.0f / 21.0f) : -(1.0f / 21.0f);
        set_volume_and_persist(esp_ai_get_volume() + delta, type, true);
        return;
    }
    if (strcmp(type, "wakeup") == 0) {
        notify_activity("business_ws_wakeup");
        otakulink_system_pause_business_ws_for_ai("business_ws_wakeup");
        esp_ai_wakeup();
        return;
    }
    if (strcmp(type, "tts") == 0 || strcmp(type, "char_text") == 0) {
        cJSON *item = root ? cJSON_GetObjectItem(root, "data") : NULL;
        if (!cJSON_IsString(item)) item = root ? cJSON_GetObjectItem(root, "text") : NULL;
        if (cJSON_IsString(item)) {
            notify_activity("business_ws_tts");
            otakulink_system_pause_business_ws_for_ai("business_ws_tts");
            esp_ai_tts(item->valuestring);
        }
        return;
    }
    if (strcmp(type, "voice_config") == 0 || strcmp(type, "getVoiceConfig") == 0) {
        cJSON *data = root ? cJSON_GetObjectItem(root, "data") : NULL;
        if (!cJSON_IsObject(data)) data = root;
        bool updated = false;
        if (copy_json_string(data, "personaId", s_persona_id, sizeof(s_persona_id)) ||
            copy_json_string(data, "persona_id", s_persona_id, sizeof(s_persona_id)) ||
            copy_json_string(data, "ext3", s_persona_id, sizeof(s_persona_id))) {
            esp_ai_nvs_save("personaId", s_persona_id);
            esp_ai_nvs_save("ext3", s_persona_id);
            updated = true;
        }
        if (copy_json_string(data, "voiceId", s_voice_id, sizeof(s_voice_id)) ||
            copy_json_string(data, "voice", s_voice_id, sizeof(s_voice_id))) {
            esp_ai_nvs_save("voiceId", s_voice_id);
            esp_ai_nvs_save("voice", s_voice_id);
            updated = true;
        }
        if (copy_json_string(data, "personality", s_personality, sizeof(s_personality)) ||
            copy_json_string(data, "persona", s_personality, sizeof(s_personality))) {
            esp_ai_nvs_save("personality", s_personality);
            esp_ai_nvs_save("persona", s_personality);
            updated = true;
        }
        if (updated) {
            s_ai_cfg.server.voice = s_voice_id;
            s_ai_cfg.server.persona = s_persona_id;
            refresh_business_ws_config("business_ws_voice_config");
        }
        return;
    }
    if (strcmp(type, "screensaver_settings") == 0 || strcmp(type, "screensaver") == 0) {
        cJSON *data = root ? cJSON_GetObjectItem(root, "data") : NULL;
        if (!cJSON_IsObject(data)) data = root;
        apply_screensaver_json(data);
        return;
    }
    if (strcmp(type, "display_mode") == 0) {
        cJSON *item = root ? cJSON_GetObjectItem(root, "data") : NULL;
        if (!cJSON_IsString(item)) item = root ? cJSON_GetObjectItem(root, "mode") : NULL;
        if (cJSON_IsString(item)) apply_display_mode_value(item->valuestring);
        return;
    }
    ESP_LOGI(TAG, "[业务WS] 仅记录命令 type=%s", type);
}

static void activation_prompt_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (esp_ai_is_connected() && !esp_ai_get_busy()) {
        otakulink_system_pause_business_ws_for_ai("activation_prompt");
        esp_ai_tts("设备激活成功，现在可以和我聊天了哦。");
    }
    vTaskDelete(NULL);
}

static void schedule_activation_prompt_once(void)
{
    if (s_activation_announced) {
        return;
    }
    s_activation_announced = true;
    esp_ai_nvs_save("activated", "1");
    xTaskCreate(activation_prompt_task, "activation_prompt", 4096, NULL, 4, NULL);
}

/* AI 收到服务器指令的回调 */
void on_ai_command(const char* type, cJSON* data)
{
    if (strcmp(type, "on_llm_cb") == 0) {
        if (cJSON_IsString(data)) {
            ESP_LOGI(TAG, "🤖 AI 回复: %s", data->valuestring);
            char_text_append(data->valuestring);
        } else if (cJSON_IsObject(data)) {
            cJSON *text = cJSON_GetObjectItem(data, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, "🤖 AI 回复: %s", text->valuestring);
                char_text_append(text->valuestring);
            }
        }
    } else if (strcmp(type, "on_iat_cb") == 0) {
        const char *text = NULL;
        if (cJSON_IsString(data)) {
            text = data->valuestring;
        } else if (cJSON_IsObject(data)) {
            cJSON *text_item = cJSON_GetObjectItem(data, "text");
            if (cJSON_IsString(text_item)) {
                text = text_item->valuestring;
            }
        }
        if (text) {
            ESP_LOGI(TAG, "[ASR] 语音识别结果: %s", text);
            if (otakulink_reminder_try_create_from_text(text)) {
                return;
            }
        }
    } else if (strcmp(type, "cron_task") == 0) {
        if (otakulink_reminder_handle_cron_task(data)) {
            notify_activity("cron_task");
        }
    } else if (strcmp(type, "session_status") == 0) {
        const char *status = NULL;
        if (cJSON_IsString(data)) {
            status = data->valuestring;
        } else if (cJSON_IsObject(data)) {
            cJSON *status_item = cJSON_GetObjectItem(data, "status");
            if (cJSON_IsString(status_item)) {
                status = status_item->valuestring;
            }
        }
        if (status) {
            ESP_LOGI(TAG, "会话状态: %s", status);
        }
        if (status && (strcmp(status, "tts_real_end") == 0 ||
                       strcmp(status, "end") == 0 ||
                       strcmp(status, "session_end") == 0)) {
            char_text_finish();
        }
    } else {
        ESP_LOGI(TAG, "收到指令类型: %s", type);
    }
}

/* AI 业务 Ready 回调 */
void on_ai_ready(void)
{
    ESP_LOGI(TAG, "🎉 AI 业务握手成功！");
    schedule_activation_prompt_once();
}

static void start_ai_if_needed(void)
{
    if (s_ai_started) {
        return;
    }
    if (s_api_key[0] == '\0') {
        ESP_LOGI(TAG, "API Key为空，暂不启动ESP-AI，等待小程序通过 /api/apikey 配置");
        return;
    }
    esp_ai_on_ready(on_ai_ready);
    esp_ai_on_command(on_ai_command);
    esp_err_t err = esp_ai_begin(&s_ai_cfg);
    if (err == ESP_OK) {
        s_ai_started = true;
        refresh_business_ws_config("ai_started");
        schedule_device_binding("ai_started");
        ESP_LOGI(TAG, "WiFi获取IP后已启动ESP-AI服务");
    } else {
        ESP_LOGE(TAG, "ESP-AI启动失败: %s", esp_err_to_name(err));
    }
}

static void ai_start_task(void *arg)
{
    (void)arg;
    start_ai_if_needed();
    s_ai_start_pending = false;
    vTaskDelete(NULL);
}

static void request_ai_start(void)
{
    if (s_ai_started || s_ai_start_pending) {
        return;
    }
    s_ai_start_pending = true;
    BaseType_t ok = xTaskCreate(ai_start_task, "ai_start", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        s_ai_start_pending = false;
        ESP_LOGE(TAG, "创建AI启动任务失败");
    }
}

/* ---- PDQ-compatible BLE provisioning ---- */
#define BLE_DEVICE_NAME "OtakuLink-EchoVer2.0"
#define BLE_WIFI_LIST_MAX 15

static const ble_uuid128_t s_ble_service_uuid =
    BLE_UUID128_INIT(0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f,
                     0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f);
static const ble_uuid128_t s_ble_char_uuid =
    BLE_UUID128_INIT(0xa8, 0x26, 0x91, 0x14, 0x73, 0x6e, 0x5a, 0xea,
                     0xb7, 0x8f, 0x46, 0xe1, 0x3e, 0x48, 0xb5, 0xbe);

static uint16_t s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ble_chr_handle;
static uint8_t s_ble_own_addr_type;
static bool s_ble_notify_enabled = false;
static bool s_ble_started = false;

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg);
static void ble_start_advertising(void);

static void ble_send_text(const char *text)
{
    if (!text || s_ble_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_ble_notify_enabled) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, strlen(text));
    if (!om) {
        ESP_LOGW(TAG, "[BLE] 通知内存申请失败: %s", text);
        return;
    }
    int rc = ble_gatts_notify_custom(s_ble_conn_handle, s_ble_chr_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "[BLE] 通知发送失败 rc=%d 内容=%s", rc, text);
    } else {
        ESP_LOGI(TAG, "[BLE] 已通知: %s", text);
    }
}

static bool ble_has_valid_wifi(char *ip_out, size_t ip_out_len)
{
    wifi_ap_record_t ap = {0};
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
                     netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
                     ip_info.ip.addr != 0 && ip_info.gw.addr != 0;
    char ip[16] = "0.0.0.0";
    char gw[16] = "0.0.0.0";
    esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
    ESP_LOGI(TAG, "[BLE] WiFi检查 ip=%s 网关=%s 信号=%d -> %s",
             ip, gw, connected ? ap.rssi : 0, connected ? "OK" : "NOT_CONNECTED");
    if (connected && ip_out && ip_out_len > 0) {
        strlcpy(ip_out, ip, ip_out_len);
    }
    return connected;
}

static void ble_send_wifi_list(char ssids[][33], int count)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "WIFI_START:%d", count);
    ble_send_text(msg);
    vTaskDelay(pdMS_TO_TICKS(50));

    for (int i = 0; i < count; ++i) {
        if (ssids[i][0] == '\0') continue;
        char prefix[12];
        snprintf(prefix, sizeof(prefix), "W%d:", i);
        size_t prefix_len = strlen(prefix);
        const char *ssid = ssids[i];
        size_t ssid_len = strlen(ssid);
        size_t pos = 0;
        int chunk = 0;
        while (pos < ssid_len) {
            size_t room = 18 > prefix_len ? 18 - prefix_len : 8;
            size_t n = ssid_len - pos;
            if (n > room) n = room;
            if (chunk == 0) {
                snprintf(msg, sizeof(msg), "%s%.*s", prefix, (int)n, ssid + pos);
            } else {
                snprintf(msg, sizeof(msg), "W%d+%d:%.*s", i, chunk, (int)n, ssid + pos);
            }
            ble_send_text(msg);
            pos += n;
            chunk++;
            vTaskDelay(pdMS_TO_TICKS(80));
        }
    }
    ble_send_text("WIFI_END");
    ESP_LOGI(TAG, "[BLE] WiFi列表发送完成 数量=%d", count);
}

static void ble_wifi_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[BLE] WiFi扫描任务开始");
    wifi_scan_config_t scan_config = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[BLE] WiFi扫描失败: %s", esp_err_to_name(err));
        ble_send_text("NO_WIFI");
        vTaskDelete(NULL);
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ble_send_text("NO_WIFI");
        vTaskDelete(NULL);
        return;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!records) {
        ble_send_text("WIFI_ERROR:内存不足");
        vTaskDelete(NULL);
        return;
    }
    uint16_t fetch_count = ap_count;
    esp_wifi_scan_get_ap_records(&fetch_count, records);

    char ssids[BLE_WIFI_LIST_MAX][33] = {0};
    int selected = 0;
    for (int i = 0; i < fetch_count && selected < BLE_WIFI_LIST_MAX; ++i) {
        if (records[i].ssid[0] == '\0') continue;
        bool duplicate = false;
        for (int j = 0; j < selected; ++j) {
            if (strncmp(ssids[j], (const char *)records[i].ssid, sizeof(ssids[j])) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            strlcpy(ssids[selected], (const char *)records[i].ssid, sizeof(ssids[selected]));
            ESP_LOGI(TAG, "[BLE] WiFi %d: %s 信号=%d", selected + 1, ssids[selected], records[i].rssi);
            selected++;
        }
    }
    free(records);

    if (selected > 0) {
        ble_send_wifi_list(ssids, selected);
    } else {
        ble_send_text("NO_WIFI");
    }
    ESP_LOGI(TAG, "[BLE] WiFi扫描任务结束");
    vTaskDelete(NULL);
}

static void ble_wifi_connect_task(void *arg)
{
    char *payload = (char *)arg;
    char *comma = strchr(payload, ',');
    if (!comma) {
        ble_send_text("WIFI_ERROR:格式错误");
        free(payload);
        vTaskDelete(NULL);
        return;
    }
    *comma = '\0';
    const char *ssid = payload;
    const char *password = comma + 1;
    ESP_LOGI(TAG, "[BLE] 正在连接WiFi: SSID=%s", ssid);
    ble_send_text("WIFI_CONNECTING:已收到WiFi配置，正在连接...");

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();

    bool connected = false;
    char ip[16] = {0};
    for (int i = 0; i < 60; ++i) {
        if (ble_has_valid_wifi(ip, sizeof(ip))) {
            connected = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (connected) {
        strlcpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid));
        strlcpy(s_wifi_password, password, sizeof(s_wifi_password));
        esp_ai_nvs_save("wifi_name", s_wifi_ssid);
        esp_ai_nvs_save("wifi_pwd", s_wifi_password);
        char msg[64];
        snprintf(msg, sizeof(msg), "IP:%s", ip);
        ble_send_text(msg);
        vTaskDelay(pdMS_TO_TICKS(300));
        ble_send_text("WIFI_OK:连接成功");
        start_webserver();
        request_ai_start();
        schedule_device_binding("ble_wifi_connected");
    } else {
        ble_send_text("WIFI_ERROR:连接超时，请检查WiFi账号密码");
    }

    free(payload);
    vTaskDelete(NULL);
}

static void ble_handle_command(const char *cmd)
{
    ESP_LOGI(TAG, "[BLE] 收到数据: %s", cmd);
    if (strcmp(cmd, "REQUEST_IP") == 0) {
        char ip[16];
        if (ble_has_valid_wifi(ip, sizeof(ip))) {
            char msg[64];
            snprintf(msg, sizeof(msg), "IP:%s", ip);
            ESP_LOGI(TAG, "[BLE] REQUEST_IP -> 返回 %s", msg);
            ble_send_text(msg);
        } else {
            ESP_LOGI(TAG, "[BLE] REQUEST_IP -> WiFi未连接");
            ble_send_text("WIFI_STATUS:NOT_CONNECTED");
        }
        return;
    }
    if (strcmp(cmd, "SCAN_WIFI") == 0) {
        xTaskCreate(ble_wifi_scan_task, "ble_wifi_scan", 8192, NULL, 2, NULL);
        return;
    }
    const char *payload = NULL;
    if (strncmp(cmd, "WIFI:", 5) == 0) {
        payload = cmd + 5;
    } else if (strchr(cmd, ',')) {
        payload = cmd;
    }
    if (payload) {
        char *copy = strdup(payload);
        if (!copy) {
            ble_send_text("WIFI_ERROR:内存不足");
            return;
        }
        xTaskCreate(ble_wifi_connect_task, "ble_wifi_conn", 8192, copy, 2, NULL);
        return;
    }
}

static int ble_gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *ready = "Ready for WiFi config";
        return os_mbuf_append(ctxt->om, ready, strlen(ready)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0) return 0;
        if (len > 255) len = 255;
        char buf[256];
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        buf[len] = '\0';
        ble_handle_command(buf);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_ble_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_ble_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_ble_char_uuid.u,
                .access_cb = ble_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_ble_chr_handle,
            },
            {0},
        },
    },
    {0},
};

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "[BLE] 已连接 handle=%d", s_ble_conn_handle);
        } else {
            ESP_LOGW(TAG, "[BLE] 连接失败 status=%d", event->connect.status);
            ble_start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "[BLE] 已断开 reason=%d", event->disconnect.reason);
        s_ble_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_ble_notify_enabled = false;
        ble_start_advertising();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ble_chr_handle) {
            s_ble_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "[BLE] 通知开关=%d", s_ble_notify_enabled ? 1 : 0);
            if (s_ble_notify_enabled) {
                ble_send_text("BLE_CONNECTED:ESP32已准备就绪，请发送REQUEST_IP获取IP地址");
            }
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ble_start_advertising();
        return 0;
    default:
        return 0;
    }
}

static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_ble_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] 广播字段设置失败 rc=%d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t *)BLE_DEVICE_NAME;
    rsp_fields.name_len = strlen(BLE_DEVICE_NAME);
    rsp_fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] 广播响应字段设置失败 rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ble_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] 开始广播失败 rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "[BLE] 正在广播 名称=%s 地址类型=%u", BLE_DEVICE_NAME, s_ble_own_addr_type);
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_ble_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "[BLE] 推断地址类型失败 rc=%d", rc);
        return;
    }
    ble_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "[BLE] 重置 reason=%d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_start_service(void)
{
    if (s_ble_started) return;
    s_ble_started = true;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[BLE] NimBLE初始化失败: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) ESP_LOGW(TAG, "[BLE] 设置名称返回 rc=%d", rc);
    rc = ble_gatts_count_cfg(s_ble_services);
    if (rc != 0) ESP_LOGE(TAG, "[BLE] 服务计数配置失败 rc=%d", rc);
    rc = ble_gatts_add_svcs(s_ble_services);
    if (rc != 0) ESP_LOGE(TAG, "[BLE] 添加服务失败 rc=%d", rc);
    ble_store_config_init();
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "[BLE] BLE初始化完成，开始广播");
}

/* GET /status 处理程序 */
esp_err_t status_get_handler(httpd_req_t *req)
{
    char resp_str[512];
    uint32_t free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t min_free = esp_get_minimum_free_heap_size();

    snprintf(resp_str, sizeof(resp_str),
             "{\"status\":\"ok\", \"free_sram\":%lu, \"free_psram\":%lu, \"min_free\":%lu}",
             (unsigned long)free_sram, (unsigned long)free_psram, (unsigned long)min_free);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}

static void set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-Filename, X-File-Name, X-OTA-Session");
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (buf_len == 0) return ESP_ERR_INVALID_ARG;
    size_t to_read = req->content_len;
    if (to_read >= buf_len) {
        to_read = buf_len - 1;
    }

    size_t offset = 0;
    while (offset < to_read) {
        int ret = httpd_req_recv(req, buf + offset, to_read - offset);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        offset += (size_t)ret;
    }
    buf[offset] = '\0';
    return ESP_OK;
}

static bool get_query_value(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

static char *read_request_body_alloc(httpd_req_t *req, size_t max_len)
{
    if (req->content_len == 0 || req->content_len > max_len) {
        return NULL;
    }
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_8BIT);
    }
    if (!buf) {
        return NULL;
    }
    size_t offset = 0;
    while (offset < req->content_len) {
        int ret = httpd_req_recv(req, buf + offset, req->content_len - offset);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            heap_caps_free(buf);
            return NULL;
        }
        offset += (size_t)ret;
    }
    buf[offset] = '\0';
    return buf;
}

#define OTA_RECOMMENDED_CHUNK_SIZE (64 * 1024)
#define OTA_MAX_CHUNK_SIZE (256 * 1024)
#define OTA_SESSION_LEN 24

typedef struct {
    bool active;
    bool update_started;
    char session[OTA_SESSION_LEN];
    char expected_sha256[65];
    char version[48];
    char last_error[80];
    uint32_t size;
    uint32_t received;
    uint32_t chunks;
    int64_t started_us;
    int64_t last_write_us;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    mbedtls_sha256_context sha_ctx;
} ota_state_t;

static SemaphoreHandle_t s_ota_mutex;
static ota_state_t s_ota;

static bool is_hex64(const char *s)
{
    if (!s || strlen(s) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        char c = s[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!ok) return false;
    }
    return true;
}

static void lowercase_hex(char *s)
{
    for (; s && *s; ++s) {
        if (*s >= 'A' && *s <= 'F') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out, size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    if (out_len == 0) return;
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_len; ++i) {
        out[pos++] = hex[(in[i] >> 4) & 0x0f];
        out[pos++] = hex[in[i] & 0x0f];
    }
    out[pos] = '\0';
}

static void make_ota_session(char *out, size_t out_len)
{
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    snprintf(out, out_len, "%08lx%08lx", (unsigned long)a, (unsigned long)b);
}

static bool is_ota_active(void)
{
    bool active = false;
    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        active = s_ota.active;
        xSemaphoreGive(s_ota_mutex);
    }
    return active;
}

static void ota_set_last_error_locked(const char *error)
{
    strlcpy(s_ota.last_error, error ? error : "", sizeof(s_ota.last_error));
}

static void ota_abort_locked(const char *reason)
{
    if (s_ota.update_started) {
        esp_ota_abort(s_ota.handle);
    }
    if (s_ota.active) {
        ESP_LOGW(TAG, "[OTA] 已中止 原因=%s 已收=%lu/%lu 分片=%lu",
                 reason ? reason : "(none)",
                 (unsigned long)s_ota.received,
                 (unsigned long)s_ota.size,
                 (unsigned long)s_ota.chunks);
    }
    mbedtls_sha256_free(&s_ota.sha_ctx);
    memset(&s_ota, 0, sizeof(s_ota));
    if (reason) {
        strlcpy(s_ota.last_error, reason, sizeof(s_ota.last_error));
    }
}

static void ota_clear_finished_locked(void)
{
    mbedtls_sha256_free(&s_ota.sha_ctx);
    memset(&s_ota, 0, sizeof(s_ota));
}

static void ota_pause_background_work(const char *reason)
{
    otakulink_system_pause_business_ws_for_ai(reason ? reason : "ota_active");
}

static void send_ota_busy_response(httpd_req_t *req)
{
    set_cors_headers(req);
    httpd_resp_set_status(req, "423 Locked");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"OTA_UPDATING\"}", HTTPD_RESP_USE_STRLEN);
}

static bool ota_get_query_u32(httpd_req_t *req, const char *key, uint32_t *out)
{
    char value[32];
    if (!get_query_value(req, key, value, sizeof(value))) {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    *out = (uint32_t)parsed;
    return true;
}

static bool ota_get_session_from_request(httpd_req_t *req, char *out, size_t out_len)
{
    if (get_query_value(req, "session", out, out_len)) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Session", out, out_len) == ESP_OK) {
        return true;
    }
    return false;
}

static void mark_current_app_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "[OTA] 当前固件启动确认 %s 分区=%s",
                 err == ESP_OK ? "成功" : esp_err_to_name(err),
                 running->label);
    }
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char resp[768];
    bool active = false;
    char session[OTA_SESSION_LEN] = {0};
    char version[48] = {0};
    char error[80] = {0};
    uint32_t size = 0;
    uint32_t received = 0;
    uint32_t chunks = 0;
    uint32_t partition_size = 0;
    int64_t elapsed_ms = 0;
    const char *running_label = "(none)";
    const char *next_label = "(none)";

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (running) running_label = running->label;
    if (next) next_label = next->label;

    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        active = s_ota.active;
        strlcpy(session, s_ota.session, sizeof(session));
        strlcpy(version, s_ota.version, sizeof(version));
        strlcpy(error, s_ota.last_error, sizeof(error));
        size = s_ota.size;
        received = s_ota.received;
        chunks = s_ota.chunks;
        partition_size = s_ota.partition ? s_ota.partition->size : (next ? next->size : 0);
        if (s_ota.started_us > 0) {
            elapsed_ms = (esp_timer_get_time() - s_ota.started_us) / 1000;
        }
        xSemaphoreGive(s_ota_mutex);
    }

    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"active\":%s,\"session\":\"%s\","
             "\"version\":\"%s\",\"size\":%lu,\"received\":%lu,\"chunks\":%lu,"
             "\"partition_size\":%lu,\"running_partition\":\"%s\","
             "\"next_partition\":\"%s\",\"elapsed_ms\":%lld,\"last_error\":\"%s\"}",
             active ? "true" : "false",
             session,
             version,
             (unsigned long)size,
             (unsigned long)received,
             (unsigned long)chunks,
             (unsigned long)partition_size,
             running_label,
             next_label,
             (long long)elapsed_ms,
             error);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_start_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char *body = read_request_body_alloc(req, 1024);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    cJSON *size_item = root ? cJSON_GetObjectItem(root, "size") : NULL;
    cJSON *sha_item = root ? cJSON_GetObjectItem(root, "sha256") : NULL;
    cJSON *version_item = root ? cJSON_GetObjectItem(root, "version") : NULL;

    uint32_t size = cJSON_IsNumber(size_item) ? (uint32_t)size_item->valuedouble : 0;
    const char *sha = cJSON_IsString(sha_item) ? sha_item->valuestring : NULL;
    const char *version = cJSON_IsString(version_item) ? version_item->valuestring : "";

    char resp[384];
    int status = 200;
    const char *error = NULL;

    if (size == 0 || !is_hex64(sha)) {
        status = 400;
        error = "BAD_REQUEST";
        goto done;
    }

    if (!s_ota_mutex) {
        status = 500;
        error = "OTA_NOT_READY";
        goto done;
    }

    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        status = 503;
        error = "OTA_BUSY";
        goto done;
    }

    if (s_ota.active) {
        xSemaphoreGive(s_ota_mutex);
        status = 409;
        error = "OTA_ALREADY_ACTIVE";
        goto done;
    }

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        xSemaphoreGive(s_ota_mutex);
        status = 500;
        error = "NO_OTA_PARTITION";
        goto done;
    }
    if (size > partition->size) {
        xSemaphoreGive(s_ota_mutex);
        status = 413;
        error = "FIRMWARE_TOO_LARGE";
        goto done;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.active = true;
    s_ota.partition = partition;
    s_ota.size = size;
    s_ota.started_us = esp_timer_get_time();
    s_ota.last_write_us = s_ota.started_us;
    strlcpy(s_ota.expected_sha256, sha, sizeof(s_ota.expected_sha256));
    lowercase_hex(s_ota.expected_sha256);
    strlcpy(s_ota.version, version, sizeof(s_ota.version));
    make_ota_session(s_ota.session, sizeof(s_ota.session));
    mbedtls_sha256_init(&s_ota.sha_ctx);
    mbedtls_sha256_starts(&s_ota.sha_ctx, 0);

    esp_err_t begin_err = esp_ota_begin(partition, size, &s_ota.handle);
    if (begin_err != ESP_OK) {
        ota_abort_locked(esp_err_to_name(begin_err));
        xSemaphoreGive(s_ota_mutex);
        status = 500;
        error = "OTA_BEGIN_FAILED";
        goto done;
    }
    s_ota.update_started = true;

    ESP_LOGI(TAG, "[OTA] 开始 会话=%s 分区=%s 固件=%lu 分区容量=%lu 版本=%s",
             s_ota.session,
             partition->label,
             (unsigned long)s_ota.size,
             (unsigned long)partition->size,
             s_ota.version[0] ? s_ota.version : "(none)");
    ota_pause_background_work("ota_start");
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"session\":\"%s\",\"chunk_size\":%u,"
             "\"partition\":\"%s\",\"partition_size\":%lu,\"received\":0}",
             s_ota.session,
             OTA_RECOMMENDED_CHUNK_SIZE,
             partition->label,
             (unsigned long)partition->size);
    xSemaphoreGive(s_ota_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

done:
    if (root) cJSON_Delete(root);
    if (body) heap_caps_free(body);
    if (error) {
        if (status == 400) httpd_resp_set_status(req, "400 Bad Request");
        else if (status == 409) httpd_resp_set_status(req, "409 Conflict");
        else if (status == 413) httpd_resp_set_status(req, "413 Payload Too Large");
        else if (status == 503) httpd_resp_set_status(req, "503 Service Unavailable");
        else httpd_resp_set_status(req, "500 Internal Server Error");
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":\"%s\"}", error);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t ota_chunk_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char session[OTA_SESSION_LEN] = {0};
    uint32_t offset_arg = UINT32_MAX;
    uint32_t index_arg = 0;
    ota_get_session_from_request(req, session, sizeof(session));
    ota_get_query_u32(req, "offset", &offset_arg);
    ota_get_query_u32(req, "index", &index_arg);

    if (req->content_len == 0 || req->content_len > OTA_MAX_CHUNK_SIZE || session[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"BAD_CHUNK_REQUEST\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"OTA_LOCK_TIMEOUT\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!s_ota.active || strcmp(session, s_ota.session) != 0) {
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"BAD_SESSION\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (offset_arg != UINT32_MAX && offset_arg != s_ota.received) {
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"BAD_OFFSET\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (s_ota.received + req->content_len > s_ota.size) {
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"TOO_MUCH_DATA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ota_pause_background_work("ota_chunk");
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ota_set_last_error_locked("NO_MEMORY");
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"NO_MEMORY\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const uint32_t start_received = s_ota.received;
    size_t remain = req->content_len;
    int64_t started = esp_timer_get_time();
    int64_t last_data = started;
    esp_err_t result = ESP_OK;
    while (remain > 0) {
        size_t want = remain > 4096 ? 4096 : remain;
        int ret = httpd_req_recv(req, (char *)buf, want);
        int64_t now = esp_timer_get_time();
        if (ret > 0) {
            result = esp_ota_write(s_ota.handle, buf, (size_t)ret);
            if (result != ESP_OK) {
                ota_set_last_error_locked(esp_err_to_name(result));
                break;
            }
            mbedtls_sha256_update(&s_ota.sha_ctx, buf, (size_t)ret);
            s_ota.received += (uint32_t)ret;
            s_ota.last_write_us = now;
            remain -= (size_t)ret;
            last_data = now;
            continue;
        }
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            if ((now - last_data) / 1000 > 15000) {
                result = ESP_ERR_TIMEOUT;
                ota_set_last_error_locked("CHUNK_RECV_TIMEOUT");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        result = ESP_FAIL;
        ota_set_last_error_locked("CHUNK_RECV_FAILED");
        break;
    }

    heap_caps_free(buf);
    if (result == ESP_OK) {
        s_ota.chunks++;
        ESP_LOGI(TAG, "[OTA] 分片完成 index=%lu 写入=%lu 已收=%lu/%lu 耗时=%lldms",
                 (unsigned long)index_arg,
                 (unsigned long)(s_ota.received - start_received),
                 (unsigned long)s_ota.received,
                 (unsigned long)s_ota.size,
                 (long long)((esp_timer_get_time() - started) / 1000));
        char resp[192];
        snprintf(resp, sizeof(resp),
                 "{\"success\":true,\"received\":%lu,\"size\":%lu,\"chunks\":%lu}",
                 (unsigned long)s_ota.received,
                 (unsigned long)s_ota.size,
                 (unsigned long)s_ota.chunks);
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "[OTA] 分片失败 err=%s 已收=%lu/%lu", esp_err_to_name(result),
             (unsigned long)s_ota.received,
             (unsigned long)s_ota.size);
    xSemaphoreGive(s_ota_mutex);
    httpd_resp_set_status(req, result == ESP_ERR_TIMEOUT ? "408 Request Timeout" : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":false,\"error\":\"CHUNK_WRITE_FAILED\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_finish_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char session[OTA_SESSION_LEN] = {0};
    ota_get_session_from_request(req, session, sizeof(session));
    if (session[0] == '\0') {
        char *body = read_request_body_alloc(req, 512);
        cJSON *root = body ? cJSON_Parse(body) : NULL;
        cJSON *session_item = root ? cJSON_GetObjectItem(root, "session") : NULL;
        if (cJSON_IsString(session_item)) {
            strlcpy(session, session_item->valuestring, sizeof(session));
        }
        if (root) cJSON_Delete(root);
        if (body) heap_caps_free(body);
    }

    char resp[384];
    int status = 200;
    const char *error = NULL;
    char actual_sha[65] = {0};
    char partition_label[17] = {0};

    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        status = 503;
        error = "OTA_LOCK_TIMEOUT";
        goto done;
    }
    if (!s_ota.active || session[0] == '\0' || strcmp(session, s_ota.session) != 0) {
        xSemaphoreGive(s_ota_mutex);
        status = 409;
        error = "BAD_SESSION";
        goto done;
    }
    if (s_ota.received != s_ota.size) {
        ota_set_last_error_locked("SIZE_MISMATCH");
        xSemaphoreGive(s_ota_mutex);
        status = 409;
        error = "SIZE_MISMATCH";
        goto done;
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&s_ota.sha_ctx, digest);
    bytes_to_hex(digest, sizeof(digest), actual_sha, sizeof(actual_sha));
    if (strcmp(actual_sha, s_ota.expected_sha256) != 0) {
        ota_abort_locked("SHA256_MISMATCH");
        xSemaphoreGive(s_ota_mutex);
        status = 409;
        error = "SHA256_MISMATCH";
        goto done;
    }

    esp_err_t end_err = esp_ota_end(s_ota.handle);
    if (end_err != ESP_OK) {
        const char *end_name = esp_err_to_name(end_err);
        ESP_LOGW(TAG, "[OTA] 结束校验失败 err=%s 已收=%lu/%lu",
                 end_name,
                 (unsigned long)s_ota.received,
                 (unsigned long)s_ota.size);
        mbedtls_sha256_free(&s_ota.sha_ctx);
        memset(&s_ota, 0, sizeof(s_ota));
        strlcpy(s_ota.last_error, end_name, sizeof(s_ota.last_error));
        xSemaphoreGive(s_ota_mutex);
        status = 500;
        error = "OTA_END_FAILED";
        goto done;
    }
    s_ota.update_started = false;

    strlcpy(partition_label, s_ota.partition ? s_ota.partition->label : "", sizeof(partition_label));
    esp_err_t boot_err = esp_ota_set_boot_partition(s_ota.partition);
    if (boot_err != ESP_OK) {
        ota_abort_locked(esp_err_to_name(boot_err));
        xSemaphoreGive(s_ota_mutex);
        status = 500;
        error = "SET_BOOT_FAILED";
        goto done;
    }

    uint32_t total = s_ota.size;
    uint32_t chunks = s_ota.chunks;
    int64_t elapsed_ms = (esp_timer_get_time() - s_ota.started_us) / 1000;
    ESP_LOGI(TAG, "[OTA] 完成 分区=%s 字节=%lu 分片=%lu 耗时=%lldms sha=%s",
             partition_label,
             (unsigned long)total,
             (unsigned long)chunks,
             (long long)elapsed_ms,
             actual_sha);
    ota_clear_finished_locked();
    xSemaphoreGive(s_ota_mutex);

    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"message\":\"OTA completed\",\"partition\":\"%s\","
             "\"size\":%lu,\"chunks\":%lu,\"sha256\":\"%s\",\"restart_ms\":1500}",
             partition_label,
             (unsigned long)total,
             (unsigned long)chunks,
             actual_sha);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    schedule_restart(1500);
    return ESP_OK;

done:
    if (error) {
        if (status == 409) httpd_resp_set_status(req, "409 Conflict");
        else if (status == 503) httpd_resp_set_status(req, "503 Service Unavailable");
        else httpd_resp_set_status(req, "500 Internal Server Error");
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":\"%s\",\"sha256\":\"%s\"}", error, actual_sha);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t ota_cancel_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char session[OTA_SESSION_LEN] = {0};
    ota_get_session_from_request(req, session, sizeof(session));

    if (xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"OTA_LOCK_TIMEOUT\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (!s_ota.active) {
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":true,\"message\":\"No active OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (session[0] && strcmp(session, s_ota.session) != 0) {
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"BAD_SESSION\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    ota_abort_locked("USER_CANCEL");
    xSemaphoreGive(s_ota_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"success\":true,\"message\":\"OTA cancelled\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static bool copy_json_string(cJSON *root, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0') {
        return false;
    }
    strlcpy(out, item->valuestring, out_len);
    return true;
}

static bool copy_after_marker(const char *text, const char *marker, char *out, size_t out_len)
{
    const char *p = text ? strstr(text, marker) : NULL;
    if (!p || !out || out_len == 0) {
        return false;
    }
    p += strlen(marker);
    size_t n = 0;
    while (p[n] && p[n] != '"' && p[n] != '\\' && n + 1 < out_len) {
        n++;
    }
    if (n == 0) {
        return false;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool extract_agent_key_from_equipment_response(const char *body, char *out, size_t out_len)
{
    if (!body || !out || out_len == 0) {
        return false;
    }

    const char *config = strstr(body, "\"config\"");
    if (config) {
        const char *iat = strstr(config, "\\\"iat_config\\\"");
        if (iat && copy_after_marker(iat, "\\\"api_key\\\":\\\"", out, out_len)) {
            return true;
        }
        const char *llm = strstr(config, "\\\"llm_config\\\"");
        if (llm && copy_after_marker(llm, "\\\"api_key\\\":\\\"", out, out_len)) {
            return true;
        }
    }

    return copy_after_marker(body, "\"api_key\":\"", out, out_len) ||
           copy_after_marker(body, "\\\"api_key\\\":\\\"", out, out_len);
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} http_collect_ctx_t;

static esp_err_t collect_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data || !evt->data || evt->data_len <= 0) {
        return ESP_OK;
    }
    http_collect_ctx_t *ctx = (http_collect_ctx_t *)evt->user_data;
    if (!ctx->buf || ctx->cap == 0 || ctx->len >= ctx->cap - 1) {
        return ESP_OK;
    }
    size_t room = ctx->cap - 1 - ctx->len;
    size_t copy_len = (size_t)evt->data_len;
    if (copy_len > room) {
        copy_len = room;
    }
    memcpy(ctx->buf + ctx->len, evt->data, copy_len);
    ctx->len += copy_len;
    ctx->buf[ctx->len] = '\0';
    return ESP_OK;
}

static bool resolve_agent_key_from_user_key(const char *input_key, char *agent_key, size_t agent_key_len)
{
    if (!input_key || input_key[0] == '\0' || !agent_key || agent_key_len == 0) {
        return false;
    }

    const size_t resp_cap = 32768;
    char *response = heap_caps_malloc(resp_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        response = heap_caps_malloc(resp_cap, MALLOC_CAP_8BIT);
    }
    if (!response) {
        ESP_LOGW(TAG, "[APIKEY] equipment/list响应内存不足");
        return false;
    }
    response[0] = '\0';
    http_collect_ctx_t collect = {
        .buf = response,
        .cap = resp_cap,
        .len = 0,
    };

    esp_http_client_config_t cfg = {
        .url = "http://api.espai2.fun/equipment/list",
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .event_handler = collect_http_event,
        .user_data = &collect,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(response);
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "authorization", input_key);
    esp_http_client_set_post_field(client, "{}", 2);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ok = false;
    if (err == ESP_OK && status == 200) {
        ok = extract_agent_key_from_equipment_response(response, agent_key, agent_key_len);
        ESP_LOGI(TAG, "[APIKEY] equipment/list返回 status=%d 字节=%u 解析成功=%d", status, (unsigned)collect.len, ok ? 1 : 0);
    } else {
        ESP_LOGW(TAG, "[APIKEY] equipment/list请求失败 err=%s status=%d", esp_err_to_name(err), status);
    }
    heap_caps_free(response);
    return ok;
}

static bool wifi_ready_for_binding(void)
{
    wifi_ap_record_t ap = {0};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static bool perform_device_binding_request(void)
{
    if (s_ext1[0] == '\0') {
        ESP_LOGW(TAG, "[绑定] 跳过：本地没有API Key");
        set_binding_status("no_key", false, true);
        return false;
    }
    if (!wifi_ready_for_binding()) {
        ESP_LOGW(TAG, "[绑定] 跳过：WiFi未连接");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGW(TAG, "[绑定] 创建请求JSON失败：内存不足");
        return false;
    }
    cJSON_AddStringToObject(root, "version", s_firmware_version);
    cJSON_AddStringToObject(root, "bin_id", s_bin_id);
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "api_key", s_ext1);
    cJSON_AddStringToObject(root, "wifi_ssid", s_wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pwd", s_wifi_password);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        ESP_LOGW(TAG, "[绑定] 序列化请求失败：内存不足");
        return false;
    }

    const size_t response_cap = 4096;
    char *response = (char *)heap_caps_calloc(1, response_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        response = (char *)heap_caps_calloc(1, response_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!response) {
        free(body);
        ESP_LOGW(TAG, "[绑定] 分配响应缓冲失败：内存不足");
        return false;
    }
    http_collect_ctx_t collect = {
        .buf = response,
        .cap = response_cap,
        .len = 0,
    };
    esp_http_client_config_t cfg = {
        .url = "http://api.espai2.fun/devices/add",
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .event_handler = collect_http_event,
        .user_data = &collect,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(response);
        free(body);
        ESP_LOGW(TAG, "[绑定] HTTP客户端初始化失败");
        return false;
    }

    ESP_LOGI(TAG, "[绑定] 开始绑定设备 device_id=%s ssid=%s key=%c%c%c%c****",
             s_device_id, s_wifi_ssid, s_ext1[0], s_ext1[1], s_ext1[2], s_ext1[3]);
    set_binding_status("binding", false, true);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    bool success = false;
    char message[128] = "";
    if (err == ESP_OK && status == 200 && collect.len > 0) {
        // The server may include a very large memory field. The beginning of
        // the response is enough for the success flag; do not fail only
        // because the tail was truncated by our bounded buffer.
        if (strstr(response, "\"success\":true")) {
            success = true;
        }
        copy_after_marker(response, "\"message\":\"", message, sizeof(message));
    }

    if (success) {
        ESP_LOGI(TAG, "[绑定] 设备绑定成功 message=%s", message[0] ? message : "(none)");
        set_binding_status("bound", true, true);
        heap_caps_free(response);
        return true;
    }

    ESP_LOGW(TAG, "[绑定] 设备绑定失败 err=%s status=%d response_prefix=%.160s",
             esp_err_to_name(err), status, response[0] ? response : "(empty)");
    set_binding_status("failed", false, true);
    heap_caps_free(response);
    return false;
}

static void device_binding_task(void *arg)
{
    const char *reason = (const char *)arg;
    ESP_LOGI(TAG, "[绑定] 后台绑定任务启动 reason=%s", reason ? reason : "unknown");
    for (int i = 0; i < 60; ++i) {
        if (s_ext1[0] == '\0') {
            set_binding_status("no_key", false, true);
            goto done;
        }
        if (wifi_ready_for_binding() && !is_ota_active() && !esp_ai_get_upload_active() && !esp_ai_get_busy()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!wifi_ready_for_binding() || is_ota_active() || esp_ai_get_upload_active() || esp_ai_get_busy()) {
        ESP_LOGW(TAG, "[绑定] 当前业务忙或网络未就绪，延后到下次触发");
        set_binding_status("pending", false, true);
        goto done;
    }
    perform_device_binding_request();

done:
    s_binding_task_active = false;
    vTaskDelete(NULL);
}

static void schedule_device_binding(const char *reason)
{
    if (s_ext1[0] == '\0') {
        set_binding_status("no_key", false, true);
        ESP_LOGI(TAG, "[绑定] 不安排绑定：本地没有API Key");
        return;
    }
    if (s_device_bound) {
        ESP_LOGI(TAG, "[绑定] 已绑定，跳过 reason=%s", reason ? reason : "unknown");
        return;
    }
    if (s_binding_task_active) {
        ESP_LOGI(TAG, "[绑定] 已有绑定任务进行中，跳过 reason=%s", reason ? reason : "unknown");
        return;
    }
    s_binding_task_active = true;
    set_binding_status("pending", false, true);
    BaseType_t ok = xTaskCreate(device_binding_task, "device_bind", 10240, (void *)reason, 4, NULL);
    if (ok != pdPASS) {
        s_binding_task_active = false;
        set_binding_status("failed", false, true);
        ESP_LOGW(TAG, "[绑定] 创建后台绑定任务失败");
    }
}

static const uint8_t *find_bytes(const uint8_t *haystack, size_t haystack_len,
                                 const char *needle, size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i <= haystack_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}


static const uint8_t *find_bytes_reverse(const uint8_t *haystack, size_t haystack_len,
                                         const char *needle, size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len) {
        return NULL;
    }
    for (size_t pos = haystack_len - needle_len + 1; pos > 0; --pos) {
        size_t i = pos - 1;
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return haystack + i;
        }
    }
    return NULL;
}

static bool filename_has_ext(const char *filename, const char *ext)
{
    if (!filename || !ext) return false;
    const char *dot = strrchr(filename, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

static const char *detect_upload_kind(const char *filename, const char *content_type)
{
    if (filename_has_ext(filename, ".png") || (content_type && strstr(content_type, "image/png"))) return "png";
    if (filename_has_ext(filename, ".jpg") || filename_has_ext(filename, ".jpeg") ||
        (content_type && strstr(content_type, "image/jpeg"))) return "jpg";
    if (filename_has_ext(filename, ".bmp") || (content_type && strstr(content_type, "image/bmp"))) return "bmp";
    if (filename_has_ext(filename, ".gif") || (content_type && strstr(content_type, "image/gif"))) return "gif";
    return NULL;
}

static const char *detect_upload_kind_from_magic(const uint8_t *data, size_t len)
{
    if (!data || len < 4) return NULL;
    if (len >= 8 &&
        data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G' &&
        data[4] == '\r' && data[5] == '\n' && data[6] == 0x1A && data[7] == '\n') {
        return "png";
    }
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "jpg";
    }
    if (len >= 6 &&
        data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == '8' && (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        return "gif";
    }
    if (data[0] == 'B' && data[1] == 'M') {
        return "bmp";
    }
    return NULL;
}

static bool extract_quoted_value(const char *src, const char *key, char *out, size_t out_len);

static bool parse_multipart_boundary(const char *content_type, char *boundary, size_t boundary_len)
{
    if (!content_type || !boundary || boundary_len == 0) return false;
    const char *boundary_key = strstr(content_type, "boundary=");
    if (!boundary_key) return false;

    strlcpy(boundary, boundary_key + strlen("boundary="), boundary_len);
    char *semi = strchr(boundary, ';');
    if (semi) *semi = '\0';
    if (boundary[0] == '"') {
        memmove(boundary, boundary + 1, strlen(boundary));
        char *quote = strchr(boundary, '"');
        if (quote) *quote = '\0';
    }
    return boundary[0] != '\0';
}

static bool parse_multipart_file_part(const uint8_t *body, size_t body_len,
                                      const char *boundary,
                                      const uint8_t **file_data,
                                      size_t *file_len,
                                      char *filename,
                                      size_t filename_len,
                                      char *part_type,
                                      size_t part_type_len)
{
    if (!body || body_len == 0 || !boundary || !file_data || !file_len) return false;

    char boundary_line[128];
    snprintf(boundary_line, sizeof(boundary_line), "--%s", boundary);
    const size_t boundary_line_len = strlen(boundary_line);
    const uint8_t *end = body + body_len;
    const uint8_t *cursor = body;
    int part_index = 0;

    const uint8_t *first = find_bytes(cursor, (size_t)(end - cursor), boundary_line, boundary_line_len);
    if (!first) return false;
    cursor = first + boundary_line_len;

    while (cursor < end) {
        if ((size_t)(end - cursor) >= 2 && cursor[0] == '-' && cursor[1] == '-') {
            break;
        }
        if ((size_t)(end - cursor) >= 2 && cursor[0] == '\r' && cursor[1] == '\n') {
            cursor += 2;
        }

        const uint8_t *header_end = find_bytes(cursor, (size_t)(end - cursor), "\r\n\r\n", 4);
        if (!header_end) break;
        size_t header_len = (size_t)(header_end - cursor);
        if (header_len > 4096) {
            ESP_LOGW(TAG, "[上传] multipart字段头过大 index=%d header=%u", part_index, (unsigned)header_len);
            return false;
        }

        char *header = heap_caps_malloc(header_len + 1, MALLOC_CAP_8BIT);
        if (!header) return false;
        memcpy(header, cursor, header_len);
        header[header_len] = '\0';

        char part_filename[96] = {0};
        char current_type[96] = {0};
        extract_quoted_value(header, "filename=\"", part_filename, sizeof(part_filename));
        char *ct = strstr(header, "Content-Type:");
        if (ct) {
            ct += strlen("Content-Type:");
            while (*ct == ' ') ct++;
            char *line_end = strstr(ct, "\r\n");
            size_t len = line_end ? (size_t)(line_end - ct) : strlen(ct);
            if (len >= sizeof(current_type)) len = sizeof(current_type) - 1;
            memcpy(current_type, ct, len);
            current_type[len] = '\0';
        }
        heap_caps_free(header);

        const uint8_t *data_start = header_end + 4;
        char next_marker[132];
        snprintf(next_marker, sizeof(next_marker), "\r\n--%s", boundary);
        const uint8_t *next = find_bytes(data_start, (size_t)(end - data_start), next_marker, strlen(next_marker));
        if (!next) break;
        size_t current_len = (size_t)(next - data_start);

        const char *kind = detect_upload_kind(part_filename, current_type);
        if (!kind) {
            kind = detect_upload_kind_from_magic(data_start, current_len);
        }

        if (kind) {
            *file_data = data_start;
            *file_len = current_len;
            if (filename && filename_len > 0) strlcpy(filename, part_filename, filename_len);
            if (part_type && part_type_len > 0) strlcpy(part_type, current_type, part_type_len);
            ESP_LOGI(TAG, "[上传] multipart选中文件字段 index=%d 文件名=%s 类型=%s 格式=%s 字节=%u",
                     part_index,
                     part_filename[0] ? part_filename : "(none)",
                     current_type[0] ? current_type : "(none)",
                     kind,
                     (unsigned)current_len);
            return true;
        }

        ESP_LOGI(TAG, "[上传] 跳过非文件字段 index=%d 文件名=%s 类型=%s 字节=%u 文件头=%02x %02x %02x %02x",
                 part_index,
                 part_filename[0] ? part_filename : "(none)",
                 current_type[0] ? current_type : "(none)",
                 (unsigned)current_len,
                 current_len > 0 ? data_start[0] : 0,
                 current_len > 1 ? data_start[1] : 0,
                 current_len > 2 ? data_start[2] : 0,
                 current_len > 3 ? data_start[3] : 0);

        cursor = next + 2 + boundary_line_len;
        part_index++;
    }

    return false;
}

static const int64_t UPLOAD_RECV_IDLE_TIMEOUT_MS = 90000;
static const int64_t UPLOAD_RECV_TOTAL_TIMEOUT_MS = 300000;

typedef struct {
    bool changed;
    wifi_ps_type_t previous_mode;
} upload_wifi_ps_guard_t;

/* Uploads are short and benefit from consistent WiFi scheduling. */
static void upload_wifi_performance_begin(upload_wifi_ps_guard_t *guard)
{
    memset(guard, 0, sizeof(*guard));
    if (esp_wifi_get_ps(&guard->previous_mode) != ESP_OK) {
        ESP_LOGW(TAG, "[上传] 无法读取WiFi省电模式，保持当前模式");
        return;
    }
    if (guard->previous_mode == WIFI_PS_NONE) {
        ESP_LOGI(TAG, "[上传] WiFi已处于全速模式");
        return;
    }
    if (esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) {
        guard->changed = true;
        ESP_LOGI(TAG, "[上传] WiFi已切换为全速模式");
    } else {
        ESP_LOGW(TAG, "[上传] WiFi切换全速模式失败，保持当前模式");
    }
}

static void upload_wifi_performance_end(const upload_wifi_ps_guard_t *guard)
{
    if (!guard->changed) return;
    if (esp_wifi_set_ps(guard->previous_mode) == ESP_OK) {
        ESP_LOGI(TAG, "[上传] WiFi已恢复原省电模式");
    } else {
        ESP_LOGW(TAG, "[上传] WiFi恢复原省电模式失败");
    }
}

static bool extract_quoted_value(const char *src, const char *key, char *out, size_t out_len)
{
    const char *p = strstr(src, key);
    if (!p) return false;
    p += strlen(key);
    const char *end = strchr(p, '"');
    if (!end || end <= p) return false;
    size_t len = (size_t)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static void cleanup_static_uploads_except(const char *keep_path)
{
    const char *paths[] = {
        "/sdcard/upload/photo.png",
        "/sdcard/upload/photo.jpg",
        "/sdcard/upload/photo.bmp",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (strcmp(paths[i], keep_path) != 0) {
            unlink(paths[i]);
        }
    }
}

static esp_err_t save_upload_file(const char *final_path, const uint8_t *data, size_t len)
{
    char tmp_path[128];
    if (strstr(final_path, "/gif/")) {
        strlcpy(tmp_path, "/sdcard/gif/TMP.BIN", sizeof(tmp_path));
    } else {
        strlcpy(tmp_path, "/sdcard/upload/TMP.BIN", sizeof(tmp_path));
    }

    const size_t chunk_size = 16 * 1024;
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(chunk_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    size_t actual_chunk = chunk_size;
    if (!chunk) {
        actual_chunk = 4 * 1024;
        chunk = (uint8_t *)heap_caps_malloc(actual_chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    }
    if (!chunk) {
        ESP_LOGE(TAG, "上传失败: 无法申请内部写入缓冲");
        return ESP_ERR_NO_MEM;
    }

    unlink(tmp_path);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        ESP_LOGE(TAG, "上传失败: 无法打开临时文件 %s", tmp_path);
        free(chunk);
        return ESP_FAIL;
    }

    size_t written_total = 0;
    int64_t write_started = esp_timer_get_time();
    while (written_total < len) {
        size_t n = len - written_total;
        if (n > actual_chunk) n = actual_chunk;
        memcpy(chunk, data + written_total, n);
        ssize_t written = write(fd, chunk, n);
        if (written <= 0 || (size_t)written != n) {
            ESP_LOGE(TAG, "上传失败: 写入不完整 已写=%u 总大小=%u ret=%d", (unsigned)written_total, (unsigned)len, (int)written);
            close(fd);
            free(chunk);
            unlink(tmp_path);
            return ESP_FAIL;
        }
        written_total += n;
    }
    int64_t before_close = esp_timer_get_time();
    close(fd);
    free(chunk);

    unlink(final_path);
    int64_t before_rename = esp_timer_get_time();
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGE(TAG, "上传失败: 文件替换失败 %s -> %s", tmp_path, final_path);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[上传] SD写入完成 分块=%u 写入=%lldms 关闭=%lldms 替换=%lldms",
             (unsigned)actual_chunk,
             (long long)((before_close - write_started) / 1000),
             (long long)((before_rename - before_close) / 1000),
             (long long)((esp_timer_get_time() - before_rename) / 1000));
    return ESP_OK;
}

static esp_err_t write_all_fd(int fd, const uint8_t *data, size_t len)
{
    size_t written_total = 0;
    while (written_total < len) {
        ssize_t written = write(fd, data + written_total, len - written_total);
        if (written <= 0) {
            return ESP_FAIL;
        }
        written_total += (size_t)written;
    }
    return ESP_OK;
}

static void build_upload_paths(const char *kind, char *final_path, size_t final_len,
                               char *lv_src, size_t lv_len, char *tmp_path, size_t tmp_len)
{
    if (strcmp(kind, "gif") == 0) {
        strlcpy(final_path, "/sdcard/gif/gif.gif", final_len);
        strlcpy(lv_src, "S:/gif/gif.gif", lv_len);
        strlcpy(tmp_path, "/sdcard/gif/TMP.BIN", tmp_len);
    } else {
        snprintf(final_path, final_len, "/sdcard/upload/photo.%s", kind);
        snprintf(lv_src, lv_len, "S:/upload/photo.%s", kind);
        strlcpy(tmp_path, "/sdcard/upload/TMP.BIN", tmp_len);
    }
}

static esp_err_t finalize_upload_tmp_file(const char *tmp_path, const char *final_path,
                                          int64_t write_started, int64_t *close_ms, int64_t *rename_ms)
{
    int64_t before_rename = esp_timer_get_time();
    unlink(final_path);
    if (rename(tmp_path, final_path) != 0) {
        ESP_LOGE(TAG, "上传失败: 文件替换失败 %s -> %s", tmp_path, final_path);
        unlink(tmp_path);
        return ESP_FAIL;
    }
    int64_t now = esp_timer_get_time();
    if (close_ms) *close_ms = 0;
    if (rename_ms) *rename_ms = (now - before_rename) / 1000;
    (void)write_started;
    return ESP_OK;
}

typedef struct {
    int fd;
    const char *marker;
    size_t marker_len;
    uint8_t *work;
    size_t work_cap;
    uint8_t *write_buf;
    size_t write_buf_cap;
    size_t write_buf_used;
    uint8_t tail[160];
    size_t tail_len;
    size_t file_len;
    uint32_t write_calls;
    int64_t write_elapsed_us;
    bool found_end;
} upload_stream_state_t;

static esp_err_t upload_stream_flush(upload_stream_state_t *st)
{
    if (st->write_buf_used == 0) return ESP_OK;
    int64_t started = esp_timer_get_time();
    if (write_all_fd(st->fd, st->write_buf, st->write_buf_used) != ESP_OK) {
        return ESP_FAIL;
    }
    st->write_elapsed_us += esp_timer_get_time() - started;
    st->file_len += st->write_buf_used;
    st->write_buf_used = 0;
    ++st->write_calls;
    return ESP_OK;
}

static esp_err_t upload_stream_queue_file_bytes(upload_stream_state_t *st, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t available = st->write_buf_cap - st->write_buf_used;
        size_t copy_len = len < available ? len : available;
        memcpy(st->write_buf + st->write_buf_used, data, copy_len);
        st->write_buf_used += copy_len;
        data += copy_len;
        len -= copy_len;
        if (st->write_buf_used == st->write_buf_cap && upload_stream_flush(st) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t upload_stream_write_file_bytes(upload_stream_state_t *st, const uint8_t *data, size_t len)
{
    if (!st->work || st->tail_len + len > st->work_cap) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(st->work, st->tail, st->tail_len);
    memcpy(st->work + st->tail_len, data, len);
    size_t work_len = st->tail_len + len;
    st->tail_len = 0;

    const uint8_t *end = find_bytes(st->work, work_len, st->marker, st->marker_len);
    if (end) {
        size_t write_len = (size_t)(end - st->work);
        if (write_len > 0 && upload_stream_queue_file_bytes(st, st->work, write_len) != ESP_OK) {
            return ESP_FAIL;
        }
        if (upload_stream_flush(st) != ESP_OK) return ESP_FAIL;
        st->found_end = true;
        return ESP_OK;
    }

    if (work_len > st->marker_len) {
        size_t write_len = work_len - st->marker_len;
        if (upload_stream_queue_file_bytes(st, st->work, write_len) != ESP_OK) {
            return ESP_FAIL;
        }
        memcpy(st->tail, st->work + write_len, st->marker_len);
        st->tail_len = st->marker_len;
    } else if (work_len > 0) {
        memcpy(st->tail, st->work, work_len);
        st->tail_len = work_len;
    }
    return ESP_OK;
}

static esp_err_t drain_upload_remainder(httpd_req_t *req, uint8_t *rx, size_t rx_size, size_t *received)
{
    while (*received < req->content_len) {
        size_t to_read = req->content_len - *received;
        if (to_read > rx_size) to_read = rx_size;
        int ret = httpd_req_recv(req, (char *)rx, to_read);
        if (ret > 0) {
            *received += (size_t)ret;
            continue;
        }
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool get_upload_filename_from_request(httpd_req_t *req, char *filename, size_t filename_len)
{
    if (!filename || filename_len == 0) return false;
    filename[0] = '\0';

    if (get_query_value(req, "filename", filename, filename_len) && filename[0] != '\0') {
        return true;
    }
    if (get_query_value(req, "name", filename, filename_len) && filename[0] != '\0') {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "X-Filename", filename, filename_len) == ESP_OK && filename[0] != '\0') {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "X-File-Name", filename, filename_len) == ESP_OK && filename[0] != '\0') {
        return true;
    }
    return false;
}

static esp_err_t stream_raw_upload_to_sd(httpd_req_t *req,
                                         const char *content_type,
                                         const char *filename,
                                         int64_t started,
                                         char *final_path,
                                         size_t final_path_len,
                                         char *lv_src,
                                         size_t lv_src_len,
                                         size_t *out_file_len,
                                         const char **out_error,
                                         int *out_status_code)
{
    const size_t rx_size = 8192;
    size_t write_buf_cap = 16 * 1024;
    uint8_t *rx = (uint8_t *)heap_caps_malloc(rx_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *write_buf = (uint8_t *)heap_caps_malloc(write_buf_cap,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!write_buf) {
        write_buf_cap = 4 * 1024;
        write_buf = (uint8_t *)heap_caps_malloc(write_buf_cap,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    }
    if (!rx || !write_buf) {
        if (rx) heap_caps_free(rx);
        if (write_buf) heap_caps_free(write_buf);
        *out_status_code = 500;
        *out_error = "NO_MEMORY";
        return ESP_FAIL;
    }

    mkdir("/sdcard/upload", 0775);
    mkdir("/sdcard/gif", 0775);

    const char *kind = detect_upload_kind(filename, content_type);
    char tmp_path[96] = {0};
    int fd = -1;
    upload_stream_state_t st = {
        .fd = -1,
        .write_buf = write_buf,
        .write_buf_cap = write_buf_cap,
    };

    size_t received = 0;
    size_t recv_min = SIZE_MAX;
    size_t recv_max = 0;
    uint32_t recv_calls = 0;
    uint32_t recv_timeouts = 0;
    int64_t recv_wait_us = 0;
    int64_t recv_wait_max_us = 0;
    int64_t last_data = started;
    int64_t write_started = 0;
    int64_t before_close = 0;
    esp_err_t result = ESP_FAIL;

    if (kind) {
        build_upload_paths(kind, final_path, final_path_len, lv_src, lv_src_len, tmp_path, sizeof(tmp_path));
        unlink(tmp_path);
        fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            *out_status_code = 500;
            *out_error = "SAVE_FAILED";
            goto cleanup;
        }
        st.fd = fd;
        write_started = esp_timer_get_time();
        ESP_LOGI(TAG, "[上传] raw直传已识别格式=%s 文件名=%s 类型=%s 保存到=%s",
                 kind,
                 filename && filename[0] ? filename : "(none)",
                 content_type && content_type[0] ? content_type : "(none)",
                 final_path);
    }

    while (received < req->content_len) {
        size_t to_read = req->content_len - received;
        if (to_read > rx_size) to_read = rx_size;
        int64_t recv_started = esp_timer_get_time();
        int ret = httpd_req_recv(req, (char *)rx, to_read);
        int64_t now = esp_timer_get_time();
        int64_t recv_elapsed_us = now - recv_started;
        recv_wait_us += recv_elapsed_us;
        if (recv_elapsed_us > recv_wait_max_us) recv_wait_max_us = recv_elapsed_us;

        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ++recv_timeouts;
                int64_t idle_ms = (now - last_data) / 1000;
                int64_t total_ms = (now - started) / 1000;
                if (idle_ms > UPLOAD_RECV_IDLE_TIMEOUT_MS || total_ms > UPLOAD_RECV_TOTAL_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "[上传] raw接收超时 已收=%u/%u 空闲=%lldms 总耗时=%lldms",
                             (unsigned)received, (unsigned)req->content_len,
                             (long long)idle_ms, (long long)total_ms);
                    *out_status_code = 408;
                    *out_error = "RECV_TIMEOUT";
                    goto cleanup;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGW(TAG, "[上传] raw接收失败 已收=%u/%u ret=%d",
                     (unsigned)received, (unsigned)req->content_len, ret);
            *out_status_code = 408;
            *out_error = "RECV_FAILED";
            goto cleanup;
        }

        ++recv_calls;
        if ((size_t)ret < recv_min) recv_min = (size_t)ret;
        if ((size_t)ret > recv_max) recv_max = (size_t)ret;
        received += (size_t)ret;
        last_data = now;

        if (!kind) {
            kind = detect_upload_kind_from_magic(rx, (size_t)ret);
            if (!kind) {
                ESP_LOGW(TAG, "[上传] raw类型不支持 文件名=%s 类型=%s 文件头=%02x %02x %02x %02x",
                         filename && filename[0] ? filename : "(none)",
                         content_type && content_type[0] ? content_type : "(none)",
                         ret > 0 ? rx[0] : 0,
                         ret > 1 ? rx[1] : 0,
                         ret > 2 ? rx[2] : 0,
                         ret > 3 ? rx[3] : 0);
                *out_status_code = 415;
                *out_error = "UNSUPPORTED_TYPE";
                goto cleanup;
            }
            build_upload_paths(kind, final_path, final_path_len, lv_src, lv_src_len, tmp_path, sizeof(tmp_path));
            unlink(tmp_path);
            fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                *out_status_code = 500;
                *out_error = "SAVE_FAILED";
                goto cleanup;
            }
            st.fd = fd;
            write_started = esp_timer_get_time();
            ESP_LOGI(TAG, "[上传] raw根据文件头识别格式=%s 文件名=%s 类型=%s 保存到=%s",
                     kind,
                     filename && filename[0] ? filename : "(none)",
                     content_type && content_type[0] ? content_type : "(none)",
                     final_path);
        }

        if (upload_stream_queue_file_bytes(&st, rx, (size_t)ret) != ESP_OK) {
            *out_status_code = 500;
            *out_error = "SAVE_FAILED";
            goto cleanup;
        }

        if ((received % (128 * 1024)) < (size_t)ret || received == req->content_len) {
            ESP_LOGI(TAG, "[上传] raw接收进度 %u/%u 文件=%u 耗时=%lldms",
                     (unsigned)received, (unsigned)req->content_len, (unsigned)st.file_len,
                     (long long)((now - started) / 1000));
        }
    }

    if (!kind || fd < 0) {
        *out_status_code = 415;
        *out_error = "UNSUPPORTED_TYPE";
        goto cleanup;
    }
    if (upload_stream_flush(&st) != ESP_OK) {
        *out_status_code = 500;
        *out_error = "SAVE_FAILED";
        goto cleanup;
    }
    before_close = esp_timer_get_time();
    close(fd);
    fd = -1;
    int64_t after_close = esp_timer_get_time();
    int64_t rename_ms = 0;
    if (finalize_upload_tmp_file(tmp_path, final_path, write_started, NULL, &rename_ms) != ESP_OK) {
        *out_status_code = 500;
        *out_error = "SAVE_FAILED";
        goto cleanup;
    }

    *out_file_len = st.file_len;
    ESP_LOGI(TAG, "[上传] 接收统计 调用=%u 单包=%u-%u 等待=%lldms 最长=%lldms 超时=%u",
             (unsigned)recv_calls,
             (unsigned)(recv_calls ? recv_min : 0),
             (unsigned)recv_max,
             (long long)(recv_wait_us / 1000),
             (long long)(recv_wait_max_us / 1000),
             (unsigned)recv_timeouts);
    ESP_LOGI(TAG, "[上传] raw保存完成 格式=%s 字节=%u 接收窗口=%lldms SD实际写入=%lldms 写入次数=%u SD块=%u 关闭=%lldms 替换=%lldms 总耗时=%lldms SRAM=%lu PSRAM=%lu",
             kind,
             (unsigned)st.file_len,
             (long long)((before_close - write_started) / 1000),
             (long long)(st.write_elapsed_us / 1000),
             (unsigned)st.write_calls,
             (unsigned)st.write_buf_cap,
             (long long)((after_close - before_close) / 1000),
             (long long)rename_ms,
             (long long)((esp_timer_get_time() - started) / 1000),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    result = ESP_OK;

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    if (result != ESP_OK && tmp_path[0]) {
        unlink(tmp_path);
    }
    heap_caps_free(rx);
    heap_caps_free(write_buf);
    return result;
}

static esp_err_t stream_multipart_upload_to_sd(httpd_req_t *req,
                                               const char *content_type,
                                               int64_t started,
                                               char *final_path,
                                               size_t final_path_len,
                                               char *lv_src,
                                               size_t lv_src_len,
                                               size_t *out_file_len,
                                               const char **out_error,
                                               int *out_status_code)
{
    const char *boundary_key = strstr(content_type, "boundary=");
    if (!boundary_key) {
        *out_status_code = 400;
        *out_error = "NO_BOUNDARY";
        return ESP_FAIL;
    }

    char boundary[96];
    strlcpy(boundary, boundary_key + strlen("boundary="), sizeof(boundary));
    char *semi = strchr(boundary, ';');
    if (semi) *semi = '\0';
    if (boundary[0] == '"') {
        memmove(boundary, boundary + 1, strlen(boundary));
        char *quote = strchr(boundary, '"');
        if (quote) *quote = '\0';
    }
    if (boundary[0] == '\0') {
        *out_status_code = 400;
        *out_error = "NO_BOUNDARY";
        return ESP_FAIL;
    }

    char marker[128];
    snprintf(marker, sizeof(marker), "\r\n--%s", boundary);
    size_t marker_len = strlen(marker);
    if (marker_len >= 150) {
        *out_status_code = 400;
        *out_error = "BOUNDARY_TOO_LONG";
        return ESP_FAIL;
    }

    const size_t rx_size = 8192;
    size_t write_buf_cap = 16 * 1024;
    uint8_t *rx = (uint8_t *)heap_caps_malloc(rx_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *work = (uint8_t *)heap_caps_malloc(rx_size + 160, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) {
        work = (uint8_t *)heap_caps_malloc(rx_size + 160, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    uint8_t *write_buf = (uint8_t *)heap_caps_malloc(write_buf_cap,
                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!write_buf) {
        write_buf_cap = 4 * 1024;
        write_buf = (uint8_t *)heap_caps_malloc(write_buf_cap,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    }
    const size_t header_buf_cap = 8192;
    uint8_t *header_buf = (uint8_t *)heap_caps_malloc(header_buf_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!header_buf) {
        header_buf = (uint8_t *)heap_caps_malloc(header_buf_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!rx || !work || !write_buf || !header_buf) {
        if (rx) heap_caps_free(rx);
        if (work) heap_caps_free(work);
        if (write_buf) heap_caps_free(write_buf);
        if (header_buf) heap_caps_free(header_buf);
        *out_status_code = 500;
        *out_error = "NO_MEMORY";
        return ESP_FAIL;
    }

    mkdir("/sdcard/upload", 0775);
    mkdir("/sdcard/gif", 0775);

    size_t received = 0;
    size_t header_used = 0;
    size_t recv_min = SIZE_MAX;
    size_t recv_max = 0;
    uint32_t recv_calls = 0;
    uint32_t recv_timeouts = 0;
    int64_t recv_wait_us = 0;
    int64_t recv_wait_max_us = 0;
    int skipped_parts = 0;
    int64_t last_data = started;
    bool header_done = false;
    char tmp_path[96] = {0};
    int fd = -1;
    upload_stream_state_t st = {
        .fd = -1,
        .marker = marker,
        .marker_len = marker_len,
        .work = work,
        .work_cap = rx_size + 160,
        .write_buf = write_buf,
        .write_buf_cap = write_buf_cap,
    };
    int64_t write_started = 0;
    int64_t before_close = 0;
    esp_err_t result = ESP_FAIL;

    ESP_LOGI(TAG, "[上传] 传输上下文 BLE连接=%d BLE通知=%d AI忙碌=%d",
             s_ble_conn_handle != BLE_HS_CONN_HANDLE_NONE ? 1 : 0,
             s_ble_notify_enabled ? 1 : 0,
             esp_ai_get_busy() ? 1 : 0);

    while (received < req->content_len && !st.found_end) {
        size_t to_read = req->content_len - received;
        if (to_read > rx_size) to_read = rx_size;
        int64_t recv_started = esp_timer_get_time();
        int ret = httpd_req_recv(req, (char *)rx, to_read);
        int64_t now = esp_timer_get_time();
        int64_t recv_elapsed_us = now - recv_started;
        recv_wait_us += recv_elapsed_us;
        if (recv_elapsed_us > recv_wait_max_us) recv_wait_max_us = recv_elapsed_us;
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ++recv_timeouts;
                int64_t idle_ms = (now - last_data) / 1000;
                int64_t total_ms = (now - started) / 1000;
                if (idle_ms > UPLOAD_RECV_IDLE_TIMEOUT_MS || total_ms > UPLOAD_RECV_TOTAL_TIMEOUT_MS) {
                    ESP_LOGW(TAG, "[上传] 流式接收超时 已收=%u/%u 空闲=%lldms 总耗时=%lldms",
                             (unsigned)received, (unsigned)req->content_len,
                             (long long)idle_ms, (long long)total_ms);
                    *out_status_code = 408;
                    *out_error = "RECV_TIMEOUT";
                    goto cleanup;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGW(TAG, "[上传] 流式接收失败 已收=%u/%u ret=%d",
                     (unsigned)received, (unsigned)req->content_len, ret);
            *out_status_code = 408;
            *out_error = "RECV_FAILED";
            goto cleanup;
        }

        ++recv_calls;
        if ((size_t)ret < recv_min) recv_min = (size_t)ret;
        if ((size_t)ret > recv_max) recv_max = (size_t)ret;
        received += (size_t)ret;
        last_data = now;
        if ((received % (128 * 1024)) < (size_t)ret || received == req->content_len) {
            ESP_LOGI(TAG, "[上传] 流式接收进度 %u/%u 文件=%u 耗时=%lldms",
                     (unsigned)received, (unsigned)req->content_len, (unsigned)st.file_len,
                     (long long)((now - started) / 1000));
        }

        if (!header_done) {
            if (header_used + (size_t)ret > header_buf_cap) {
                *out_status_code = 400;
                *out_error = "HEADER_TOO_LARGE";
                goto cleanup;
            }
            memcpy(header_buf + header_used, rx, (size_t)ret);
            header_used += (size_t)ret;
            while (!header_done) {
                const uint8_t *header_end = find_bytes(header_buf, header_used, "\r\n\r\n", 4);
                if (!header_end) break;

                size_t header_len = (size_t)(header_end - header_buf);
                char *header = heap_caps_malloc(header_len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!header) {
                    *out_status_code = 500;
                    *out_error = "NO_MEMORY";
                    goto cleanup;
                }
                memcpy(header, header_buf, header_len);
                header[header_len] = '\0';

                char filename[96] = {0};
                char part_type[96] = {0};
                extract_quoted_value(header, "filename=\"", filename, sizeof(filename));
                char *ct = strstr(header, "Content-Type:");
                if (ct) {
                    ct += strlen("Content-Type:");
                    while (*ct == ' ') ct++;
                    char *line_end = strstr(ct, "\r\n");
                    size_t len = line_end ? (size_t)(line_end - ct) : strlen(ct);
                    if (len >= sizeof(part_type)) len = sizeof(part_type) - 1;
                    memcpy(part_type, ct, len);
                    part_type[len] = '\0';
                }
                heap_caps_free(header);

                size_t data_offset = (size_t)((header_end + 4) - header_buf);
                size_t data_len = header_used - data_offset;
                const char *kind = detect_upload_kind(filename, part_type);
                if (!kind) {
                    kind = detect_upload_kind_from_magic(header_buf + data_offset, data_len);
                }
                if (!kind) {
                    const uint8_t *next_marker = find_bytes(header_buf + data_offset, data_len, marker, marker_len);
                    if (!next_marker) break;

                    const uint8_t *after_marker = next_marker + marker_len;
                    size_t remaining = (size_t)((header_buf + header_used) - after_marker);
                    if (remaining < 2) break;
                    if (after_marker[0] == '-' && after_marker[1] == '-') {
                        *out_status_code = 400;
                        *out_error = "NO_FILE_PART";
                        goto cleanup;
                    }
                    if (after_marker[0] != '\r' || after_marker[1] != '\n') {
                        *out_status_code = 400;
                        *out_error = "BAD_MULTIPART";
                        goto cleanup;
                    }

                    ++skipped_parts;
                    ESP_LOGI(TAG, "[上传] 流式跳过普通字段 index=%d 文件名=%s 类型=%s",
                             skipped_parts,
                             filename[0] ? filename : "(none)",
                             part_type[0] ? part_type : "(none)");
                    remaining -= 2;
                    memmove(header_buf, after_marker + 2, remaining);
                    header_used = remaining;
                    continue;
                }

                build_upload_paths(kind, final_path, final_path_len, lv_src, lv_src_len, tmp_path, sizeof(tmp_path));
                unlink(tmp_path);
                fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) {
                    *out_status_code = 500;
                    *out_error = "SAVE_FAILED";
                    goto cleanup;
                }
                st.fd = fd;
                write_started = esp_timer_get_time();
                header_done = true;

                ESP_LOGI(TAG, "[上传] 流式头解析完成 header=%u 文件名=%s 类型=%s 格式=%s 保存到=%s",
                         (unsigned)header_len,
                         filename[0] ? filename : "(none)",
                         part_type[0] ? part_type : "(none)",
                         kind,
                         final_path);
                if (data_len > 0 && upload_stream_write_file_bytes(&st, header_buf + data_offset, data_len) != ESP_OK) {
                    *out_status_code = 500;
                    *out_error = "SAVE_FAILED";
                    goto cleanup;
                }
            }
        } else {
            if (upload_stream_write_file_bytes(&st, rx, (size_t)ret) != ESP_OK) {
                *out_status_code = 500;
                *out_error = "SAVE_FAILED";
                goto cleanup;
            }
        }
    }

    if (!header_done || !st.found_end) {
        *out_status_code = 400;
        *out_error = "NO_MULTIPART_END";
        goto cleanup;
    }

    if (received < req->content_len) {
        drain_upload_remainder(req, rx, rx_size, &received);
    }

    if (upload_stream_flush(&st) != ESP_OK) {
        *out_status_code = 500;
        *out_error = "SAVE_FAILED";
        goto cleanup;
    }
    before_close = esp_timer_get_time();
    close(fd);
    fd = -1;
    int64_t after_close = esp_timer_get_time();
    int64_t rename_ms = 0;
    if (finalize_upload_tmp_file(tmp_path, final_path, write_started, NULL, &rename_ms) != ESP_OK) {
        *out_status_code = 500;
        *out_error = "SAVE_FAILED";
        goto cleanup;
    }

    *out_file_len = st.file_len;
    ESP_LOGI(TAG, "[上传] 接收统计 调用=%u 单包=%u-%u 等待=%lldms 最长=%lldms 超时=%u",
             (unsigned)recv_calls,
             (unsigned)(recv_calls ? recv_min : 0),
             (unsigned)recv_max,
             (long long)(recv_wait_us / 1000),
             (long long)(recv_wait_max_us / 1000),
             (unsigned)recv_timeouts);
    ESP_LOGI(TAG, "[上传] 流式保存完成 字节=%u 接收窗口=%lldms SD实际写入=%lldms 写入次数=%u SD块=%u 关闭=%lldms 替换=%lldms 总耗时=%lldms SRAM=%lu PSRAM=%lu",
             (unsigned)st.file_len,
             (long long)((before_close - write_started) / 1000),
             (long long)(st.write_elapsed_us / 1000),
             (unsigned)st.write_calls,
             (unsigned)st.write_buf_cap,
             (long long)((after_close - before_close) / 1000),
             (long long)rename_ms,
             (long long)((esp_timer_get_time() - started) / 1000),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    result = ESP_OK;

cleanup:
    if (fd >= 0) {
        close(fd);
    }
    if (result != ESP_OK && tmp_path[0]) {
        unlink(tmp_path);
    }
    heap_caps_free(rx);
    heap_caps_free(work);
    heap_caps_free(write_buf);
    heap_caps_free(header_buf);
    return result;
}

static esp_err_t api_status_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    char resp_str[1800];
    otakulink_system_snapshot_t sys;
    otakulink_system_get_snapshot(&sys);
    bool ws_connected = esp_ai_is_connected();
    bool busy = esp_ai_get_busy();
    bool ota_active = false;
    uint32_t ota_received = 0;
    uint32_t ota_size = 0;
    if (s_ota_mutex && xSemaphoreTake(s_ota_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        ota_active = s_ota.active;
        ota_received = s_ota.received;
        ota_size = s_ota.size;
        xSemaphoreGive(s_ota_mutex);
    }

    snprintf(resp_str, sizeof(resp_str),
             "{\"success\":true,\"ws_connected\":%s,\"asr_ing\":false,"
             "\"session_status\":\"%s\",\"session_id\":\"\",\"busy\":%s,"
             "\"can_wakeup\":%s,\"free_sram\":%lu,\"free_psram\":%lu,"
             "\"max_sram_block\":%lu,\"min_sram\":%lu,\"rssi\":%d,"
             "\"device_id\":\"%s\",\"has_apikey\":%s,"
             "\"bound\":%s,\"binding_status\":\"%s\","
             "\"business_ws_connected\":%s,\"business_ws_paused\":%s,"
             "\"upload_active\":%s,\"network_recovery_active\":%s,"
             "\"ota_active\":%s,\"ota_received\":%lu,\"ota_size\":%lu,"
             "\"wifi_disconnects\":%lu,\"wifi_stack_resets\":%lu,"
             "\"business_ws_connects\":%lu,\"business_ws_disconnects\":%lu,"
             "\"business_ws_errors\":%lu,\"low_mem_events\":%lu}",
             ws_connected ? "true" : "false",
             busy ? "tts_chunk_start" : "idle",
             busy ? "true" : "false",
             (ws_connected && !busy) ? "true" : "false",
             (unsigned long)sys.free_sram,
             (unsigned long)sys.free_psram,
             (unsigned long)sys.max_sram_block,
             (unsigned long)sys.min_sram,
             sys.rssi,
             s_device_id,
             s_api_key[0] ? "true" : "false",
             s_device_bound ? "true" : "false",
             s_binding_status,
             sys.business_ws_connected ? "true" : "false",
             sys.business_ws_paused ? "true" : "false",
             sys.upload_active ? "true" : "false",
             sys.network_recovery_active ? "true" : "false",
             ota_active ? "true" : "false",
             (unsigned long)ota_received,
             (unsigned long)ota_size,
             (unsigned long)sys.wifi_disconnects,
             (unsigned long)sys.wifi_stack_resets,
             (unsigned long)sys.business_ws_connects,
             (unsigned long)sys.business_ws_disconnects,
             (unsigned long)sys.business_ws_errors,
             (unsigned long)sys.low_mem_events);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "[HTTP] /api/status 已返回 AI=%d 忙碌=%d 业务WS=%d 上传=%d 绑定=%s SRAM=%luKB PSRAM=%luKB 最大连续=%luKB 信号=%d",
             ws_connected ? 1 : 0,
             busy ? 1 : 0,
             sys.business_ws_connected ? 1 : 0,
             sys.upload_active ? 1 : 0,
             s_binding_status,
             (unsigned long)(sys.free_sram / 1024),
             (unsigned long)(sys.free_psram / 1024),
             (unsigned long)(sys.max_sram_block / 1024),
             sys.rssi);
    return ESP_OK;
}

static esp_err_t get_voice_config_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    notify_activity("voice_config");

    if (req->method == HTTP_POST) {
        char *body = read_request_body_alloc(req, sizeof(s_personality) + sizeof(s_voice_id) + 256);
        cJSON *root = body ? cJSON_Parse(body) : NULL;
        if (!root) {
            if (body) heap_caps_free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"success\":false,\"error\":\"BAD_JSON\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }

        bool updated = false;
        if (copy_json_string(root, "personaId", s_persona_id, sizeof(s_persona_id)) ||
            copy_json_string(root, "persona_id", s_persona_id, sizeof(s_persona_id)) ||
            copy_json_string(root, "ext3", s_persona_id, sizeof(s_persona_id))) {
            esp_ai_nvs_save("personaId", s_persona_id);
            esp_ai_nvs_save("ext3", s_persona_id);
            updated = true;
        }
        if (copy_json_string(root, "voiceId", s_voice_id, sizeof(s_voice_id)) ||
            copy_json_string(root, "voice", s_voice_id, sizeof(s_voice_id))) {
            esp_ai_nvs_save("voiceId", s_voice_id);
            esp_ai_nvs_save("voice", s_voice_id);
            updated = true;
        }
        if (copy_json_string(root, "personality", s_personality, sizeof(s_personality)) ||
            copy_json_string(root, "persona", s_personality, sizeof(s_personality))) {
            esp_ai_nvs_save("personality", s_personality);
            esp_ai_nvs_save("persona", s_personality);
            updated = true;
        }
        s_ai_cfg.server.voice = s_voice_id;
        s_ai_cfg.server.persona = s_persona_id;
        if (updated) {
            refresh_business_ws_config("http_voice_config");
        }
        cJSON_Delete(root);
        heap_caps_free(body);

        ESP_LOGI(TAG, "[HTTP] 语音配置已保存 updated=%d 音色=%s 人设ID=%s 人设长度=%u",
                 updated ? 1 : 0, s_voice_id, s_persona_id, (unsigned)strlen(s_personality));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "personaId", s_persona_id);
    cJSON_AddStringToObject(root, "voiceId", s_voice_id);
    cJSON_AddStringToObject(root, "personality", s_personality);
    cJSON_AddNumberToObject(root, "timestamp", (double)esp_log_timestamp());
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddBoolToObject(root, "has_apikey", s_api_key[0] != '\0');
    cJSON_AddBoolToObject(root, "bound", s_device_bound);
    cJSON_AddStringToObject(root, "binding_status", s_binding_status);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "[HTTP] /api/getVoiceConfig 已返回");
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t volume_get_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    notify_activity("volume");
    char value[24];
    if (get_query_value(req, "value", value, sizeof(value))) {
        float volume = strtof(value, NULL);
        if (volume < 0.0f || volume > 1.0f) {
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"success\":false,\"error\":\"VOLUME_OUT_OF_RANGE\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        set_volume_and_persist(volume, "http", true);
    }

    float volume = esp_ai_get_volume();
    int display_volume = (int)(volume * 21.0f);
    char resp[160];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"volume\":%d,\"volume_float\":%.2f}",
             display_volume, volume);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t apikey_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    notify_activity("apikey");

    if (req->method == HTTP_GET) {
        bool has_key = s_api_key[0] != '\0';
        char preview[32] = "";
        size_t len = strlen(s_api_key);
        if (has_key && len >= 8) {
            snprintf(preview, sizeof(preview), "%.4s****%s", s_api_key, s_api_key + len - 4);
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", true);
        cJSON_AddBoolToObject(root, "has_apikey", has_key);
        cJSON_AddStringToObject(root, "device_id", s_device_id);
        cJSON_AddBoolToObject(root, "bound", s_device_bound);
        cJSON_AddStringToObject(root, "binding_status", s_binding_status);
        if (preview[0]) cJSON_AddStringToObject(root, "apikey_preview", preview);
        char *json = cJSON_PrintUnformatted(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
        free(json);
        cJSON_Delete(root);
        return ESP_OK;
    }

    if (is_ota_active()) {
        send_ota_busy_response(req);
        return ESP_OK;
    }

    char *body = read_request_body_alloc(req, 2048);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    char submitted_key[sizeof(s_api_key)] = {0};
    if (root) {
        copy_json_string(root, "apikey", submitted_key, sizeof(submitted_key));
        if (submitted_key[0] == '\0') copy_json_string(root, "apiKey", submitted_key, sizeof(submitted_key));
        if (submitted_key[0] == '\0') copy_json_string(root, "api_key", submitted_key, sizeof(submitted_key));
    }

    if (root) cJSON_Delete(root);
    if (body) heap_caps_free(body);

    if (submitted_key[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"NO_APIKEY\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char resolved_key[sizeof(s_api_key)] = {0};
    bool resolved_from_user_key = resolve_agent_key_from_user_key(submitted_key, resolved_key, sizeof(resolved_key));
    const char *key_to_save = resolved_from_user_key ? resolved_key : submitted_key;

    strlcpy(s_api_key, key_to_save, sizeof(s_api_key));
    strlcpy(s_ext1, key_to_save, sizeof(s_ext1));
    if (resolved_from_user_key) {
        strlcpy(s_user_api_key, submitted_key, sizeof(s_user_api_key));
        esp_ai_nvs_save("user_api_key", s_user_api_key);
    } else {
        s_user_api_key[0] = '\0';
        esp_ai_nvs_save("user_api_key", "");
    }
    s_ai_cfg.server.api_key = s_api_key;
    s_ai_cfg.server.ext1 = s_ext1;
    s_ai_cfg.server.device_id = s_device_id;
    esp_ai_nvs_save("api_key", s_api_key);
    esp_ai_nvs_save("ext1", s_ext1);
    esp_ai_nvs_save("device_id", s_device_id);
    set_binding_status("pending", false, true);
    schedule_device_binding("apikey_saved");
    refresh_business_ws_config("apikey_saved");
    ESP_LOGI(TAG, "[HTTP] API Key已保存 来自用户Key解析=%d device_id=%s 准备重启",
             resolved_from_user_key ? 1 : 0, s_device_id);

    httpd_resp_set_type(req, "application/json");
    if (resolved_from_user_key) {
        httpd_resp_send(req, "{\"success\":true,\"message\":\"用户Key已解析为智能体Key，设备将重启并初始化AI功能\",\"resolved\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"success\":true,\"message\":\"API Key配置成功，设备将重启并初始化AI功能\",\"resolved\":false}", HTTPD_RESP_USE_STRLEN);
    }
    schedule_restart(1000);
    return ESP_OK;
}

static bool normalize_display_mode(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len < 4) return false;
    if (strcmp(in, "0") == 0 || strcasecmp(in, "auto") == 0) {
        strlcpy(out, "auto", out_len);
        return true;
    }
    if (strcmp(in, "1") == 0 || strcasecmp(in, "gif") == 0) {
        strlcpy(out, "gif", out_len);
        return true;
    }
    if (strcmp(in, "2") == 0 || strcasecmp(in, "bmp") == 0 || strcasecmp(in, "image") == 0) {
        strlcpy(out, "bmp", out_len);
        return true;
    }
    return false;
}

static int json_get_int_alias(cJSON *root, const char *key1, const char *key2, bool *found)
{
    cJSON *item = key1 ? cJSON_GetObjectItem(root, key1) : NULL;
    if ((!item || !cJSON_IsNumber(item)) && key2) {
        item = cJSON_GetObjectItem(root, key2);
    }
    if (item && cJSON_IsNumber(item)) {
        if (found) *found = true;
        return item->valueint;
    }
    if (found) *found = false;
    return 0;
}

static bool parse_rgb565_string(const char *s, int *out_color)
{
    if (!s || !out_color) {
        return false;
    }
    while (*s == ' ' || *s == '#') {
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    char *end = NULL;
    uint32_t value = strtoul(s, &end, 16);
    if (end == s) {
        return false;
    }
    if (value <= 0xFFFF) {
        *out_color = (int)value;
    } else {
        *out_color = (int)(((value >> 8) & 0xF800) | ((value >> 5) & 0x07E0) | ((value >> 3) & 0x001F));
    }
    return true;
}

static void save_screensaver_settings(void)
{
    char value[24];
    snprintf(value, sizeof(value), "%d", s_screensaver_theme);
    esp_ai_nvs_save("screensaver_theme", value);
    snprintf(value, sizeof(value), "%d", s_screensaver_timeout_ms);
    esp_ai_nvs_save("screensaver_timeout_ms", value);
    snprintf(value, sizeof(value), "%d", s_screensaver_time_size);
    esp_ai_nvs_save("screensaver_time_size", value);
    snprintf(value, sizeof(value), "%d", s_screensaver_date_size);
    esp_ai_nvs_save("screensaver_date_size", value);
    snprintf(value, sizeof(value), "%d", s_screensaver_time_color);
    esp_ai_nvs_save("screensaver_time_color", value);
    snprintf(value, sizeof(value), "%d", s_screensaver_date_color);
    esp_ai_nvs_save("screensaver_date_color", value);
}

static esp_err_t display_mode_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    notify_activity("display_mode");

    if (req->method == HTTP_POST || req->method == HTTP_GET) {
        char mode_arg[24];
        if (get_query_value(req, "mode", mode_arg, sizeof(mode_arg))) {
            if (!apply_display_mode_value(mode_arg)) {
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, "{\"success\":false,\"error\":\"BAD_MODE\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }
        } else if (req->method == HTTP_POST && req->content_len > 0) {
            char body[128];
            if (read_request_body(req, body, sizeof(body)) == ESP_OK) {
                cJSON *root = cJSON_Parse(body);
                cJSON *mode = root ? cJSON_GetObjectItem(root, "mode") : NULL;
                if (cJSON_IsString(mode)) {
                    apply_display_mode_value(mode->valuestring);
                }
                if (root) cJSON_Delete(root);
            }
        }
    }

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"success\":true,\"mode\":\"%s\"}", s_display_mode);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t screensaver_settings_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    notify_activity("screensaver_settings");
    bool updated = false;

    if (req->method == HTTP_GET) {
        char value[32];
        if (get_query_value(req, "theme", value, sizeof(value))) {
            int theme = atoi(value);
            if (theme == 0 || theme == 1) {
                s_screensaver_theme = theme;
                updated = true;
            }
        }
        if (get_query_value(req, "timeout_ms", value, sizeof(value))) {
            int timeout = atoi(value);
            if (timeout == 0 || timeout >= 5000) {
                s_screensaver_timeout_ms = timeout == 0 ? INT32_MAX : timeout;
                updated = true;
            }
        } else if (get_query_value(req, "timeout", value, sizeof(value))) {
            int timeout_minutes = atoi(value);
            if (timeout_minutes == 0) {
                s_screensaver_timeout_ms = INT32_MAX;
                updated = true;
            } else if (timeout_minutes >= 1 && timeout_minutes <= 60) {
                s_screensaver_timeout_ms = timeout_minutes * 60 * 1000;
                updated = true;
            }
        }
        if (get_query_value(req, "timeoutMs", value, sizeof(value)) || get_query_value(req, "timeoutMS", value, sizeof(value))) {
            int timeout = atoi(value);
            if (timeout == 0 || timeout >= 5000) {
                s_screensaver_timeout_ms = timeout == 0 ? INT32_MAX : timeout;
                updated = true;
            }
        }
        if (get_query_value(req, "time_size", value, sizeof(value)) || get_query_value(req, "timeSize", value, sizeof(value))) {
            s_screensaver_time_size = atoi(value);
            updated = true;
        }
        if (get_query_value(req, "date_size", value, sizeof(value)) || get_query_value(req, "dateSize", value, sizeof(value))) {
            s_screensaver_date_size = atoi(value);
            updated = true;
        }
        if (get_query_value(req, "time_color", value, sizeof(value)) || get_query_value(req, "timeColor", value, sizeof(value))) {
            int color_value = 0;
            if (parse_rgb565_string(value, &color_value)) {
                s_screensaver_time_color = color_value;
                updated = true;
            }
        }
        if (get_query_value(req, "date_color", value, sizeof(value)) || get_query_value(req, "dateColor", value, sizeof(value))) {
            int color_value = 0;
            if (parse_rgb565_string(value, &color_value)) {
                s_screensaver_date_color = color_value;
                updated = true;
            }
        }
        if (updated) {
            save_screensaver_settings();
            ESP_LOGI(TAG, "[HTTP] 屏保GET设置已保存 主题=%d 超时=%d",
                     s_screensaver_theme, s_screensaver_timeout_ms);
        }
    }

    if (req->method == HTTP_POST && req->content_len > 0) {
        char *body = read_request_body_alloc(req, 1024);
        cJSON *root = body ? cJSON_Parse(body) : NULL;
        if (!root) {
            if (body) heap_caps_free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"success\":false,\"error\":\"BAD_JSON\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }

        updated = apply_screensaver_json(root);

        ESP_LOGI(TAG, "[HTTP] 屏保设置已保存 主题=%d 超时=%d",
                 s_screensaver_theme, s_screensaver_timeout_ms);
        cJSON_Delete(root);
        heap_caps_free(body);
    }

    char resp[384];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"theme\":%d,\"timeout_ms\":%d,\"timeoutMs\":%d,"
             "\"timeSize\":%d,\"dateSize\":%d,\"timeColor\":%d,\"dateColor\":%d,"
             "\"settings\":{\"theme\":%d,\"timeout_ms\":%d,"
             "\"time_size\":%d,\"date_size\":%d,\"time_color\":%d,\"date_color\":%d}}",
             s_screensaver_theme, s_screensaver_timeout_ms, s_screensaver_timeout_ms,
             s_screensaver_time_size, s_screensaver_date_size,
             s_screensaver_time_color, s_screensaver_date_color,
             s_screensaver_theme, s_screensaver_timeout_ms,
             s_screensaver_time_size, s_screensaver_date_size,
             s_screensaver_time_color, s_screensaver_date_color);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t screensaver_status_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    if (req->method == HTTP_POST) {
        notify_activity("screensaver_status");
    }

    int64_t idle_ms = s_last_activity_us > 0 ? (esp_timer_get_time() - s_last_activity_us) / 1000 : 0;
    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"active\":%s,\"idle_ms\":%lld,\"timeout_ms\":%d,\"theme\":%d}",
             s_screensaver_active ? "true" : "false",
             (long long)idle_ms,
             s_screensaver_timeout_ms,
             s_screensaver_theme);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    if (is_ota_active()) {
        send_ota_busy_response(req);
        return ESP_OK;
    }
    notify_activity("upload");
    esp_ai_set_upload_active(true);

    int64_t started = esp_timer_get_time();
    int64_t last_data = started;
    size_t body_len = req->content_len;
    uint8_t *body = NULL;
    const uint8_t *file_data = NULL;
    size_t file_len = 0;
    char final_path[96] = {0};
    char lv_src[96] = {0};
    const char *kind = NULL;
    bool should_show = false;
    bool keep_upload_active_until_display = false;
    int status_code = 200;
    const char *error = NULL;
    upload_wifi_ps_guard_t wifi_ps_guard = {0};

    ESP_LOGI(TAG, "[上传] 开始 总长度=%u 可用SRAM=%lu 可用PSRAM=%lu",
             (unsigned)body_len,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (body_len == 0 || body_len > 7 * 1024 * 1024) {
        status_code = 400;
        error = "BAD_SIZE";
        goto done;
    }

    upload_wifi_performance_begin(&wifi_ps_guard);

    char content_type[160] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));

    const bool is_multipart = strstr(content_type, "multipart/form-data") != NULL;
    const size_t stream_upload_threshold = 2 * 1024 * 1024;
    if (is_multipart && body_len > stream_upload_threshold) {
        ESP_LOGI(TAG, "[上传] 使用流式路径：文件较大，边接收边写入SD 阈值=%u", (unsigned)stream_upload_threshold);
        esp_err_t stream_err = stream_multipart_upload_to_sd(req,
                                                             content_type,
                                                             started,
                                                             final_path,
                                                             sizeof(final_path),
                                                             lv_src,
                                                             sizeof(lv_src),
                                                             &file_len,
                                                             &error,
                                                             &status_code);
        if (stream_err != ESP_OK) {
            goto done;
        }
        should_show = true;
        if (should_show) {
            cleanup_static_uploads_except(final_path);
            keep_upload_active_until_display = request_show_uploaded_image(final_path, lv_src);
            if (keep_upload_active_until_display) {
                ESP_LOGI(TAG, "[上传] 文件已保存，等待图片显示完成后恢复业务WS");
            }
        }
        ESP_LOGI(TAG, "[上传] 流式文件已保存 文件=%s 字节=%u 耗时=%lldms",
                 final_path, (unsigned)file_len, (long long)((esp_timer_get_time() - started) / 1000));
        goto done;
    }
    if (!is_multipart) {
        char raw_filename[96] = {0};
        get_upload_filename_from_request(req, raw_filename, sizeof(raw_filename));
        ESP_LOGI(TAG, "[上传] 使用raw直传路径：小程序预处理图片直接写入SD 文件名=%s 类型=%s",
                 raw_filename[0] ? raw_filename : "(none)",
                 content_type[0] ? content_type : "(none)");
        esp_err_t raw_err = stream_raw_upload_to_sd(req,
                                                    content_type,
                                                    raw_filename,
                                                    started,
                                                    final_path,
                                                    sizeof(final_path),
                                                    lv_src,
                                                    sizeof(lv_src),
                                                    &file_len,
                                                    &error,
                                                    &status_code);
        if (raw_err != ESP_OK) {
            goto done;
        }
        should_show = true;
        cleanup_static_uploads_except(final_path);
        keep_upload_active_until_display = request_show_uploaded_image(final_path, lv_src);
        if (keep_upload_active_until_display) {
            ESP_LOGI(TAG, "[上传] 文件已保存，等待图片显示完成后恢复业务WS");
        }
        ESP_LOGI(TAG, "[上传] raw文件已保存 文件=%s 字节=%u 耗时=%lldms",
                 final_path, (unsigned)file_len, (long long)((esp_timer_get_time() - started) / 1000));
        goto done;
    }
    if (is_multipart) {
        ESP_LOGI(TAG, "[上传] 使用缓冲路径：小文件先接收完整再写入SD 阈值=%u", (unsigned)stream_upload_threshold);
    }
    body = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        body = heap_caps_malloc(body_len + 1, MALLOC_CAP_8BIT);
    }
    if (!body) {
        status_code = 500;
        error = "NO_MEMORY";
        goto done;
    }

    const size_t buffered_recv_chunk = 8192;
    size_t offset = 0;
    size_t recv_min = SIZE_MAX;
    size_t recv_max = 0;
    uint32_t recv_calls = 0;
    uint32_t recv_timeouts = 0;
    int64_t recv_wait_us = 0;
    int64_t recv_wait_max_us = 0;
    while (offset < body_len) {
        size_t to_read = body_len - offset;
        if (to_read > buffered_recv_chunk) {
            to_read = buffered_recv_chunk;
        }
        int64_t recv_started = esp_timer_get_time();
        int ret = httpd_req_recv(req, (char *)body + offset, to_read);
        int64_t now = esp_timer_get_time();
        int64_t recv_elapsed_us = now - recv_started;
        recv_wait_us += recv_elapsed_us;
        if (recv_elapsed_us > recv_wait_max_us) recv_wait_max_us = recv_elapsed_us;
        if (ret > 0) {
            ++recv_calls;
            if ((size_t)ret < recv_min) recv_min = (size_t)ret;
            if ((size_t)ret > recv_max) recv_max = (size_t)ret;
            offset += (size_t)ret;
            last_data = now;
            if ((offset % (128 * 1024)) < (size_t)ret || offset == body_len) {
                ESP_LOGI(TAG, "[上传] 接收进度 %u/%u 耗时=%lldms",
                         (unsigned)offset, (unsigned)body_len, (long long)((now - started) / 1000));
            }
            continue;
        }

        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            ++recv_timeouts;
            int64_t idle_ms = (now - last_data) / 1000;
            int64_t total_ms = (now - started) / 1000;
            if (idle_ms > UPLOAD_RECV_IDLE_TIMEOUT_MS || total_ms > UPLOAD_RECV_TOTAL_TIMEOUT_MS) {
                ESP_LOGW(TAG, "[上传] 接收超时 已收=%u/%u 空闲=%lldms 总耗时=%lldms",
                         (unsigned)offset, (unsigned)body_len, (long long)idle_ms, (long long)total_ms);
                status_code = 408;
                error = "RECV_TIMEOUT";
                goto done;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        ESP_LOGW(TAG, "[上传] 接收失败 已收=%u/%u ret=%d", (unsigned)offset, (unsigned)body_len, ret);
        status_code = 408;
        error = "RECV_FAILED";
        goto done;
    }
    body[body_len] = 0;
    ESP_LOGI(TAG, "[上传] 接收统计 调用=%u 单包=%u-%u 等待=%lldms 最长=%lldms 超时=%u",
             (unsigned)recv_calls,
             (unsigned)(recv_calls ? recv_min : 0),
             (unsigned)recv_max,
             (long long)(recv_wait_us / 1000),
             (long long)(recv_wait_max_us / 1000),
             (unsigned)recv_timeouts);

    file_data = body;
    file_len = body_len;
    char filename[96] = {0};
    char part_type[96] = {0};

    if (strstr(content_type, "multipart/form-data")) {
        char boundary[96];
        if (!parse_multipart_boundary(content_type, boundary, sizeof(boundary))) {
            status_code = 400;
            error = "NO_BOUNDARY";
            goto done;
        }

        int64_t parse_started = esp_timer_get_time();
        if (!parse_multipart_file_part(body, body_len,
                                       boundary,
                                       &file_data,
                                       &file_len,
                                       filename,
                                       sizeof(filename),
                                       part_type,
                                       sizeof(part_type))) {
            status_code = 400;
            error = "NO_FILE_PART";
            goto done;
        }
        ESP_LOGI(TAG, "[上传] multipart解析完成 文件=%u 耗时=%lldms",
                 (unsigned)file_len,
                 (long long)((esp_timer_get_time() - parse_started) / 1000));
    } else {
        strlcpy(part_type, content_type, sizeof(part_type));
        char name_arg[96];
        if (get_query_value(req, "filename", name_arg, sizeof(name_arg))) {
            strlcpy(filename, name_arg, sizeof(filename));
        }
    }

    kind = detect_upload_kind(filename, part_type);
    if (!kind) {
        kind = detect_upload_kind_from_magic(file_data, file_len);
    }
    if (!kind) {
        ESP_LOGW(TAG, "[上传] 类型不支持 文件名=%s 类型=%s 文件头=%02x %02x %02x %02x",
                 filename[0] ? filename : "(none)",
                 part_type[0] ? part_type : "(none)",
                 file_len > 0 ? file_data[0] : 0,
                 file_len > 1 ? file_data[1] : 0,
                 file_len > 2 ? file_data[2] : 0,
                 file_len > 3 ? file_data[3] : 0);
        status_code = 415;
        error = "UNSUPPORTED_TYPE";
        goto done;
    }
    ESP_LOGI(TAG, "[上传] 已识别格式=%s 文件名=%s 类型=%s",
             kind,
             filename[0] ? filename : "(none)",
             part_type[0] ? part_type : "(none)");

    mkdir("/sdcard/upload", 0775);
    mkdir("/sdcard/gif", 0775);

    if (strcmp(kind, "gif") == 0) {
        strlcpy(final_path, "/sdcard/gif/gif.gif", sizeof(final_path));
        strlcpy(lv_src, "S:/gif/gif.gif", sizeof(lv_src));
        should_show = true;
    } else {
        snprintf(final_path, sizeof(final_path), "/sdcard/upload/photo.%s", kind);
        snprintf(lv_src, sizeof(lv_src), "S:/upload/photo.%s", kind);
        should_show = true;
    }

    int64_t save_started = esp_timer_get_time();
    esp_err_t save_err = save_upload_file(final_path, file_data, file_len);
    ESP_LOGI(TAG, "[UPLOAD] save_file dt=%lldms file=%s bytes=%u",
             (long long)((esp_timer_get_time() - save_started) / 1000), final_path, (unsigned)file_len);
    if (save_err != ESP_OK) {
        status_code = 500;
        error = "SAVE_FAILED";
        goto done;
    }

    if (should_show) {
        cleanup_static_uploads_except(final_path);
        keep_upload_active_until_display = request_show_uploaded_image(final_path, lv_src);
        if (keep_upload_active_until_display) {
            ESP_LOGI(TAG, "[上传] 文件已保存，等待图片显示完成后恢复业务WS");
        }
    }

    ESP_LOGI(TAG, "[上传] 保存完成 格式=%s 文件=%s 字节=%u 耗时=%lldms",
             kind, final_path, (unsigned)file_len, (long long)((esp_timer_get_time() - started) / 1000));

done:
    if (body) {
        heap_caps_free(body);
    }
    upload_wifi_performance_end(&wifi_ps_guard);
    if (!keep_upload_active_until_display) {
        esp_ai_set_upload_active(false);
    }

    if (error) {
        ESP_LOGW(TAG, "[上传] 失败 错误=%s HTTP状态=%d 耗时=%lldms",
                 error, status_code, (long long)((esp_timer_get_time() - started) / 1000));
        if (status_code == 400) httpd_resp_set_status(req, "400 Bad Request");
        else if (status_code == 408) httpd_resp_set_status(req, "408 Request Timeout");
        else if (status_code == 415) httpd_resp_set_status(req, "415 Unsupported Media Type");
        else httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        char resp[96];
        snprintf(resp, sizeof(resp), "{\"success\":false,\"error\":\"%s\"}", error);
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char resp[192];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"message\":\"Upload completed\",\"path\":\"%s\",\"size\":%u}",
             final_path, (unsigned)file_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/wakeup 处理程序 */
esp_err_t wakeup_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "收到 HTTP 唤醒请求");
    set_cors_headers(req);
    if (is_ota_active()) {
        send_ota_busy_response(req);
        return ESP_OK;
    }
    notify_activity("wakeup");
    if (!esp_ai_is_connected()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\",\"error\":\"AI_NOT_CONNECTED\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (esp_ai_get_busy()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"busy\",\"error\":\"AI_BUSY\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    otakulink_system_pause_business_ws_for_ai("http_wakeup");
    esp_ai_wakeup();
    const char* resp_str = "{\"status\":\"ok\", \"message\":\"waking up...\"}";
    httpd_resp_send(req, resp_str, strlen(resp_str));
    return ESP_OK;
}


static esp_err_t char_text_post_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    if (is_ota_active()) {
        send_ota_busy_response(req);
        return ESP_OK;
    }
    notify_activity("char_text");

    if (!esp_ai_is_connected()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"AI_NOT_CONNECTED\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (esp_ai_get_busy() || s_char_text_active) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"AI_BUSY\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char *body = read_request_body_alloc(req, 2048);
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    cJSON *text_item = root ? cJSON_GetObjectItem(root, "text") : NULL;
    const char *text = cJSON_IsString(text_item) ? text_item->valuestring : NULL;
    if (!text || text[0] == '\0') {
        if (root) cJSON_Delete(root);
        if (body) heap_caps_free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"NO_TEXT\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char text_copy[768];
    strlcpy(text_copy, text, sizeof(text_copy));
    if (root) cJSON_Delete(root);
    if (body) heap_caps_free(body);

    char_text_reset();
    otakulink_system_pause_business_ws_for_ai("http_char_text");
    esp_err_t send_err = esp_ai_tts(text_copy);
    if (send_err != ESP_OK) {
        char_text_set_inactive();
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"AI_SEND_FAILED\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Keep SSE send buffers internal: lwIP/httpd runs on internal-SRAM paths and
    // previously hit an interrupt WDT while sending from PSRAM-backed buffers.
    char *new_text = (char *)internal_text_alloc(1536);
    char *escaped = (char *)internal_text_alloc(2048);
    char *event = (char *)internal_text_alloc(2300);
    if (!new_text || !escaped || !event) {
        if (new_text) heap_caps_free(new_text);
        if (escaped) heap_caps_free(escaped);
        if (event) heap_caps_free(event);
        char_text_set_inactive();
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"success\":false,\"error\":\"NO_MEMORY\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "[HTTP] /api/char_text 开始 文本长度=%u", (unsigned)strlen(text_copy));
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    bool sent_any = false;
    bool normal_done = false;
    int64_t started = esp_timer_get_time();
    int64_t last_event = started;
    const int64_t first_token_timeout_ms = 30000;
    const int64_t total_timeout_ms = 180000;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(20));
        bool has_new = false;
        bool done = false;
        bool active = false;
        size_t chunk_len = 0;
        new_text[0] = '\0';

        if (xSemaphoreTake(s_char_text_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            has_new = s_char_text_has_new;
            done = s_char_text_done;
            active = s_char_text_active;
            size_t accum_len = strlen(s_char_text_accum);
            if (has_new && accum_len > s_char_text_last_pos) {
                chunk_len = accum_len - s_char_text_last_pos;
                if (chunk_len >= 1536) {
                    chunk_len = 1535;
                }
                memcpy(new_text, s_char_text_accum + s_char_text_last_pos, chunk_len);
                new_text[chunk_len] = '\0';
                s_char_text_last_pos += chunk_len;
                if (s_char_text_last_pos >= accum_len) {
                    s_char_text_has_new = false;
                }
            }
            xSemaphoreGive(s_char_text_mutex);
        }

        int64_t now = esp_timer_get_time();
        if (!active) {
            break;
        }

        if (chunk_len > 0) {
            json_escape_text(new_text, escaped, 2048);
            snprintf(event, 2300, "data: %s\n\n", escaped);
            esp_err_t chunk_err = httpd_resp_send_chunk(req, event, HTTPD_RESP_USE_STRLEN);
            if (chunk_err != ESP_OK) {
                ESP_LOGW(TAG, "[HTTP] /api/char_text client disconnected while sending chunk err=%s", esp_err_to_name(chunk_err));
                char_text_set_inactive();
                heap_caps_free(new_text);
                heap_caps_free(escaped);
                heap_caps_free(event);
                return ESP_OK;
            }
            sent_any = true;
            last_event = now;
            ESP_LOGI(TAG, "[HTTP] /api/char_text SSE chunk len=%u", (unsigned)chunk_len);
        }

        if (done) {
            const char *round_end = "event: round_end\ndata: {\"done\":true}\n\n";
            const char *end = "event: end\ndata: {\"done\":true}\n\n";
            httpd_resp_send_chunk(req, round_end, HTTPD_RESP_USE_STRLEN);
            httpd_resp_send_chunk(req, end, HTTPD_RESP_USE_STRLEN);
            normal_done = true;
            break;
        }

        int64_t elapsed_ms = (now - started) / 1000;
        int64_t idle_ms = (now - last_event) / 1000;
        if (sent_any && !esp_ai_get_busy() && idle_ms > 1200 && elapsed_ms > 2000) {
            const char *round_end = "event: round_end\ndata: {\"done\":true}\n\n";
            const char *end = "event: end\ndata: {\"done\":true}\n\n";
            httpd_resp_send_chunk(req, round_end, HTTPD_RESP_USE_STRLEN);
            httpd_resp_send_chunk(req, end, HTTPD_RESP_USE_STRLEN);
            normal_done = true;
            break;
        }
        if ((!sent_any && elapsed_ms > first_token_timeout_ms) || elapsed_ms > total_timeout_ms) {
            ESP_LOGW(TAG, "[HTTP] /api/char_text timeout sent=%d elapsed=%lld idle=%lld",
                     sent_any ? 1 : 0, (long long)elapsed_ms, (long long)idle_ms);
            const char *timeout_event = "event: error\ndata: {\"error\":\"timeout\"}\n\n";
            httpd_resp_send_chunk(req, timeout_event, HTTPD_RESP_USE_STRLEN);
            break;
        }
    }

    char_text_set_inactive();
    httpd_resp_send_chunk(req, NULL, 0);
    heap_caps_free(new_text);
    heap_caps_free(escaped);
    heap_caps_free(event);
    ESP_LOGI(TAG, "[HTTP] /api/char_text end normal=%d sent=%d", normal_done ? 1 : 0, sent_any ? 1 : 0);
    return ESP_OK;
}

/* POST /api/chat 处理程序 */
esp_err_t chat_post_handler(httpd_req_t *req)
{
    set_cors_headers(req);
    if (is_ota_active()) {
        send_ota_busy_response(req);
        return ESP_OK;
    }
    notify_activity("chat");
    char buf[256];
    if (read_request_body(req, buf, sizeof(buf)) != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            ESP_LOGI(TAG, "用户提问: %s", text->valuestring);
            otakulink_system_pause_business_ws_for_ai("http_chat");
            esp_err_t err = esp_ai_tts(text->valuestring);
            if (err == ESP_OK) {
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"success\":true,\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
            } else {
                httpd_resp_set_status(req, "503 Service Unavailable");
                httpd_resp_set_type(req, "application/json");
                httpd_resp_send(req, "{\"success\":false,\"error\":\"AI_SEND_FAILED\"}", HTTPD_RESP_USE_STRLEN);
            }
        }
        cJSON_Delete(root);
    }
    return ESP_OK;
}

static const httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL };
static const httpd_uri_t api_status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler, .user_ctx = NULL };
static const httpd_uri_t voice_config_uri = { .uri = "/api/getVoiceConfig", .method = HTTP_GET, .handler = get_voice_config_handler, .user_ctx = NULL };
static const httpd_uri_t voice_config_post_uri = { .uri = "/api/getVoiceConfig", .method = HTTP_POST, .handler = get_voice_config_handler, .user_ctx = NULL };
static const httpd_uri_t volume_uri = { .uri = "/volume", .method = HTTP_GET, .handler = volume_get_handler, .user_ctx = NULL };
static const httpd_uri_t api_volume_uri = { .uri = "/api/volume", .method = HTTP_GET, .handler = volume_get_handler, .user_ctx = NULL };
static const httpd_uri_t display_mode_uri = { .uri = "/api/display/mode", .method = HTTP_GET, .handler = display_mode_handler, .user_ctx = NULL };
static const httpd_uri_t display_mode_post_uri = { .uri = "/api/display/mode", .method = HTTP_POST, .handler = display_mode_handler, .user_ctx = NULL };
static const httpd_uri_t display_mode_short_uri = { .uri = "/display/mode", .method = HTTP_GET, .handler = display_mode_handler, .user_ctx = NULL };
static const httpd_uri_t display_mode_short_post_uri = { .uri = "/display/mode", .method = HTTP_POST, .handler = display_mode_handler, .user_ctx = NULL };
static const httpd_uri_t screensaver_uri = { .uri = "/api/screensaver/settings", .method = HTTP_GET, .handler = screensaver_settings_handler, .user_ctx = NULL };
static const httpd_uri_t screensaver_post_uri = { .uri = "/api/screensaver/settings", .method = HTTP_POST, .handler = screensaver_settings_handler, .user_ctx = NULL };
static const httpd_uri_t screensaver_short_uri = { .uri = "/screensaver", .method = HTTP_GET, .handler = screensaver_settings_handler, .user_ctx = NULL };
static const httpd_uri_t screensaver_short_post_uri = { .uri = "/screensaver", .method = HTTP_POST, .handler = screensaver_settings_handler, .user_ctx = NULL };
static const httpd_uri_t screensaver_status_uri = { .uri = "/api/screensaver", .method = HTTP_GET, .handler = screensaver_status_handler, .user_ctx = NULL };
static const httpd_uri_t screensaver_status_post_uri = { .uri = "/api/screensaver", .method = HTTP_POST, .handler = screensaver_status_handler, .user_ctx = NULL };
static const httpd_uri_t wakeup_uri = { .uri = "/api/wakeup", .method = HTTP_GET, .handler = wakeup_get_handler, .user_ctx = NULL };
static const httpd_uri_t wakeup_post_uri = { .uri = "/api/wakeup", .method = HTTP_POST, .handler = wakeup_get_handler, .user_ctx = NULL };
static const httpd_uri_t chat_uri = { .uri = "/api/chat", .method = HTTP_POST, .handler = chat_post_handler, .user_ctx = NULL };
static const httpd_uri_t char_text_uri = { .uri = "/api/char_text", .method = HTTP_POST, .handler = char_text_post_handler, .user_ctx = NULL };
static const httpd_uri_t api_upload_uri = { .uri = "/api/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL };
static const httpd_uri_t upload_uri = { .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL };
static const httpd_uri_t ota_status_uri = { .uri = "/api/ota/status", .method = HTTP_GET, .handler = ota_status_handler, .user_ctx = NULL };
static const httpd_uri_t ota_start_uri = { .uri = "/api/ota/start", .method = HTTP_POST, .handler = ota_start_handler, .user_ctx = NULL };
static const httpd_uri_t ota_chunk_uri = { .uri = "/api/ota/chunk", .method = HTTP_POST, .handler = ota_chunk_handler, .user_ctx = NULL };
static const httpd_uri_t ota_finish_uri = { .uri = "/api/ota/finish", .method = HTTP_POST, .handler = ota_finish_handler, .user_ctx = NULL };
static const httpd_uri_t ota_cancel_uri = { .uri = "/api/ota/cancel", .method = HTTP_POST, .handler = ota_cancel_handler, .user_ctx = NULL };
static const httpd_uri_t api_apikey_uri = { .uri = "/api/apikey", .method = HTTP_GET, .handler = apikey_handler, .user_ctx = NULL };
static const httpd_uri_t api_apikey_post_uri = { .uri = "/api/apikey", .method = HTTP_POST, .handler = apikey_handler, .user_ctx = NULL };
static const httpd_uri_t apikey_post_uri = { .uri = "/apikey", .method = HTTP_POST, .handler = apikey_handler, .user_ctx = NULL };
static const httpd_uri_t api_config_apikey_post_uri = { .uri = "/api/config/apikey", .method = HTTP_POST, .handler = apikey_handler, .user_ctx = NULL };
static const httpd_uri_t options_all_uri = { .uri = "/*", .method = HTTP_OPTIONS, .handler = options_handler, .user_ctx = NULL };

static esp_err_t register_route(httpd_handle_t server, const httpd_uri_t *route)
{
    esp_err_t err = httpd_register_uri_handler(server, route);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP路由注册失败: uri=%s method=%d err=%s",
                 route->uri, route->method, esp_err_to_name(err));
    }
    return err;
}

static httpd_handle_t start_webserver(void)
{
    static httpd_handle_t server = NULL;
    if (server) {
        ESP_LOGI(TAG, "HTTP服务已启动，跳过重复启动");
        return server;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 48;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&server, &config) == ESP_OK) {
        register_route(server, &status_uri);
        register_route(server, &api_status_uri);
        register_route(server, &voice_config_uri);
        register_route(server, &voice_config_post_uri);
        register_route(server, &volume_uri);
        register_route(server, &api_volume_uri);
        register_route(server, &display_mode_uri);
        register_route(server, &display_mode_post_uri);
        register_route(server, &display_mode_short_uri);
        register_route(server, &display_mode_short_post_uri);
        register_route(server, &screensaver_uri);
        register_route(server, &screensaver_post_uri);
        register_route(server, &screensaver_short_uri);
        register_route(server, &screensaver_short_post_uri);
        register_route(server, &screensaver_status_uri);
        register_route(server, &screensaver_status_post_uri);
        register_route(server, &wakeup_uri);
        register_route(server, &wakeup_post_uri);
        register_route(server, &chat_uri);
        register_route(server, &char_text_uri);
        register_route(server, &api_upload_uri);
        register_route(server, &upload_uri);
        register_route(server, &ota_status_uri);
        register_route(server, &ota_start_uri);
        register_route(server, &ota_chunk_uri);
        register_route(server, &ota_finish_uri);
        register_route(server, &ota_cancel_uri);
        register_route(server, &api_apikey_uri);
        register_route(server, &api_apikey_post_uri);
        register_route(server, &apikey_post_uri);
        register_route(server, &api_config_apikey_post_uri);
        register_route(server, &options_all_uri);
        ESP_LOGI(TAG, "HTTP服务已启动");
        return server;
    }
    server = NULL;
    return NULL;
}

/* Wi-Fi 事件处理 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi已断开，准备重连...");
        otakulink_system_on_wifi_disconnected();
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dns_info_t dns;
        dns.ip.u_addr.ip4.addr = ipaddr_addr("114.114.114.114");
        dns.ip.type = IPADDR_TYPE_V4;
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "WiFi已就绪，启动服务...");
        otakulink_system_on_wifi_got_ip();
        start_time_sync_if_needed();
        start_webserver();
        request_ai_start();
        schedule_device_binding("wifi_got_ip");
    }
}

void app_main(void)
{
    s_pending_show_mutex = xSemaphoreCreateMutex();
    s_char_text_mutex = xSemaphoreCreateMutex();
    s_ota_mutex = xSemaphoreCreateMutex();
    otakulink_reminder_init();
    notify_activity("boot");

    nvs_flash_init();
    mark_current_app_valid_if_pending();
    load_runtime_config();

    if (esp_ai_ui_init() == ESP_OK) {
        s_ui_ready = true;
        show_boot_status_screen("Starting...", "Waiting for network");
    } else {
        ESP_LOGE(TAG, "[显示] UI初始化失败，屏幕功能不可用");
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    otakulink_system_init();
    otakulink_system_set_business_ws_message_cb(business_ws_message_handler);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, s_wifi_password, sizeof(wifi_config.sta.password));
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    show_boot_status_screen("Connecting WiFi", "BLE is available");
    esp_wifi_start();
    ble_start_service();

    // 初始化 SD 卡
    if (esp_ai_sd_init() == ESP_OK) {
        ESP_LOGI("MAIN", "SD卡初始化成功");
        // 简单读取一下目录里的文件看看
                DIR* dir = opendir("/sdcard");
        if (dir != NULL) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != NULL) {
                ESP_LOGI("MAIN", "发现文件: %s", ent->d_name);
            }
            closedir(dir);
        }
        
        ESP_LOGI("MAIN", "--- 正在列出 /sdcard/upload ---");
        DIR* udir = opendir("/sdcard/upload");
        if (udir != NULL) {
            struct dirent* ent;
            while ((ent = readdir(udir)) != NULL) {
                ESP_LOGI("MAIN", "上传目录文件: %s", ent->d_name);
            }
            closedir(udir);
        }
    } else {
        ESP_LOGE("MAIN", "SD卡初始化失败");
    }

    show_startup_image_or_status();

    while(1) {
        lv_tick_inc(10);
        poll_time_sync();
        poll_screensaver_state();
        otakulink_reminder_tick();
        process_restore_last_display();
        process_pending_image_show();
        lv_timer_handler(); // 驱动 LVGL 计时器
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
