#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ESP_AI_NVS";
#define STORAGE_NAMESPACE "esp_ai_storage"

/* 保存配置到 NVS */
esp_err_t esp_ai_nvs_save(const char* key, const char* value)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(my_handle, key, value);
    if (err == ESP_OK) err = nvs_commit(my_handle);
    nvs_close(my_handle);
    return err;
}

/* 从 NVS 读取配置 */
esp_err_t esp_ai_nvs_load(const char* key, char* out_value, size_t max_len)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) return err;

    size_t required_size;
    err = nvs_get_str(my_handle, key, NULL, &required_size);
    if (err == ESP_OK) {
        if (required_size > max_len) {
            err = ESP_ERR_NVS_INVALID_LENGTH;
        } else {
            err = nvs_get_str(my_handle, key, out_value, &required_size);
        }
    }
    nvs_close(my_handle);
    return err;
}
