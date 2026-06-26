#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp-ai-sd.h"

static const char *TAG = "ESP_AI_SD";

#define SD_PIN_NUM_MISO 13
#define SD_PIN_NUM_MOSI 11
#define SD_PIN_NUM_CLK  12
#define SD_PIN_NUM_CS   10

esp_err_t esp_ai_sd_init(void) {
    esp_err_t ret;
    ESP_LOGI(TAG, "正在通过SPI初始化SD卡");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 20000;
    host.slot = SPI2_HOST; // Use SPI3 since UI uses SPI2

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_NUM_MOSI,
        .miso_io_num = SD_PIN_NUM_MISO,
        .sclk_io_num = SD_PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    
    ESP_LOGI(TAG, "正在初始化SPI总线 %d", host.slot);
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "初始化SPI总线失败");
        return ret;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "正在挂载SD文件系统");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "挂载SD文件系统失败");
        } else {
            ESP_LOGE(TAG, "初始化SD卡失败 (%s)", esp_err_to_name(ret));
        }
        return ret;
    }
    
    ESP_LOGI(TAG, "SD文件系统挂载成功");
    sdmmc_card_print_info(stdout, card);
    
    return ESP_OK;
}
