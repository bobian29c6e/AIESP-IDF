#ifndef OTAKULINK_SYSTEM_H
#define OTAKULINK_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_id;
    const char *version;
    const char *api_key;
    const char *ext2;
    const char *ext3;
    const char *ext4;
    const char *ext5;
} otakulink_business_ws_config_t;

typedef struct {
    uint32_t free_sram;
    uint32_t min_sram;
    uint32_t max_sram_block;
    uint32_t free_psram;
    int rssi;
    bool wifi_connected;
    bool ai_ws_connected;
    bool business_ws_connected;
    bool business_ws_paused;
    bool upload_active;
    bool network_recovery_active;
    uint32_t wifi_disconnects;
    uint32_t wifi_stack_resets;
    uint32_t business_ws_connects;
    uint32_t business_ws_disconnects;
    uint32_t business_ws_errors;
    uint32_t low_mem_events;
} otakulink_system_snapshot_t;

typedef void (*otakulink_business_ws_message_cb_t)(const char *type, cJSON *root);

void otakulink_system_init(void);
void otakulink_system_set_business_ws_config(const otakulink_business_ws_config_t *config);
void otakulink_system_set_business_ws_message_cb(otakulink_business_ws_message_cb_t cb);
esp_err_t otakulink_business_ws_send_text(const char *text);
void otakulink_system_on_wifi_got_ip(void);
void otakulink_system_on_wifi_disconnected(void);
void otakulink_system_pause_business_ws_for_ai(const char *reason);
bool otakulink_business_ws_is_connected(void);
void otakulink_system_get_snapshot(otakulink_system_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif
