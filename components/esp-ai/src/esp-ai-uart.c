#include "driver/uart.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "esp-ai.h"
#include <string.h>

#define ASR_UART_NUM     UART_NUM_1
#define ASR_RX_PIN       44  // Match Arduino esp-ai default hardware wiring
#define ASR_TX_PIN       43
#define ASR_BUF_SIZE     1024

static const char *TAG = "ESP_AI_UART";

static void asrpro_task(void *pvParameters)
{
    uint8_t* data = (uint8_t*) heap_caps_malloc(ASR_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = (uint8_t*) heap_caps_malloc(ASR_BUF_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!data) {
        ESP_LOGE(TAG, "ASRPro接收缓冲申请失败");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        int len = uart_read_bytes(ASR_UART_NUM, data, ASR_BUF_SIZE - 1, 100 / portTICK_PERIOD_MS);
        if (len > 0) {
            data[len] = '\0';
            ESP_LOGI(TAG, "ASRPro 串口收到数据: %s", (char*)data);

            // Arduino 项目里的 ASRPro 默认发送 "start"，保留 "wakeup" 兼容旧测试。
            if (strstr((char*)data, "start") || strstr((char*)data, "wakeup")) {
                ESP_LOGI(TAG, ">>> 匹配到唤醒指令，正在触发 AI...");
                esp_ai_wakeup();
            }
        }
    }
    heap_caps_free(data);
    vTaskDelete(NULL);
}

esp_err_t esp_ai_uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(ASR_UART_NUM, ASR_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(ASR_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ASR_UART_NUM, ASR_TX_PIN, ASR_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    BaseType_t task_ok = xTaskCreateWithCaps(asrpro_task, "asrpro_task", 4096, NULL, 5, NULL,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "ASRPro PSRAM任务栈申请失败，回退到内部SRAM任务栈");
        xTaskCreate(asrpro_task, "asrpro_task", 4096, NULL, 5, NULL);
    }
    ESP_LOGI(TAG, "ASRPro 串口监听任务已启动 (RX:%d, TX:%d)", ASR_RX_PIN, ASR_TX_PIN);
    return ESP_OK;
}
