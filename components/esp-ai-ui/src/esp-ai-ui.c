#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "esp-ai-ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ESP_AI_UI";

#define LCD_QSPI_CS    45
#define LCD_QSPI_SCK   47
#define LCD_QSPI_D0    21
#define LCD_QSPI_D1    48
#define LCD_QSPI_D2    40
#define LCD_QSPI_D3    39
#define LCD_POWER_EN   2   // 既是背光/屏幕电源，也是扬声器 WS 时钟

#define LCD_H_RES      480
#define LCD_V_RES      272

static esp_lcd_panel_io_handle_t global_io_handle = NULL;
static lv_obj_t *s_image_obj = NULL;
static lv_img_dsc_t s_image_dsc;
static lv_color_t *s_image_buf = NULL;

void esp_ai_ui_reset_image_state(void)
{
    s_image_obj = NULL;
    if (s_image_buf) {
        free(s_image_buf);
        s_image_buf = NULL;
    }
    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t read_le32_signed(const uint8_t *p)
{
    return (int32_t)read_le32(p);
}

static esp_err_t lcd_send_cmd_log(esp_lcd_panel_io_handle_t io, uint32_t cmd, const void *param, size_t param_len)
{
    // NV3041A QSPI 硬件要求：
    // Command = 0x02 (Write), Address = 0x00, c, 0x00
    // 这里将原来的 0xXX000000 转换为 0x0200XX00
    uint8_t c = (cmd >> 24) & 0xFF;
    uint32_t real_cmd = 0x02000000 | (c << 8); 
    return esp_lcd_panel_io_tx_param(io, real_cmd, param, param_len);
}


static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void nv3041a_init_sequence(esp_lcd_panel_io_handle_t io)

{
    ESP_LOGI(TAG, "正在发送 NV3041A 初始化序列 (32-bit Command)...");
    
    lcd_send_cmd_log(io, 0x01000000, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_send_cmd_log(io, 0xFF000000, (uint8_t[]){0xA5}, 1);
    lcd_send_cmd_log(io, 0x36000000, (uint8_t[]){0xC0}, 1);
    lcd_send_cmd_log(io, 0x3A000000, (uint8_t[]){0x01}, 1);
    lcd_send_cmd_log(io, 0x41000000, (uint8_t[]){0x03}, 1);
    lcd_send_cmd_log(io, 0x44000000, (uint8_t[]){0x15}, 1);
    lcd_send_cmd_log(io, 0x45000000, (uint8_t[]){0x15}, 1);
    lcd_send_cmd_log(io, 0x7D000000, (uint8_t[]){0x03}, 1);
    lcd_send_cmd_log(io, 0xC1000000, (uint8_t[]){0xAB}, 1);
    lcd_send_cmd_log(io, 0xC2000000, (uint8_t[]){0x17}, 1);
    lcd_send_cmd_log(io, 0xC3000000, (uint8_t[]){0x10}, 1);
    lcd_send_cmd_log(io, 0xC6000000, (uint8_t[]){0x3A}, 1);
    lcd_send_cmd_log(io, 0xC7000000, (uint8_t[]){0x25}, 1);
    lcd_send_cmd_log(io, 0xC8000000, (uint8_t[]){0x11}, 1);
    lcd_send_cmd_log(io, 0x7A000000, (uint8_t[]){0x49}, 1);
    lcd_send_cmd_log(io, 0x6F000000, (uint8_t[]){0x2F}, 1);
    lcd_send_cmd_log(io, 0x78000000, (uint8_t[]){0x4B}, 1);
    lcd_send_cmd_log(io, 0xC9000000, (uint8_t[]){0x00}, 1);
    lcd_send_cmd_log(io, 0x67000000, (uint8_t[]){0x33}, 1);
    lcd_send_cmd_log(io, 0x51000000, (uint8_t[]){0x4B}, 1);
    lcd_send_cmd_log(io, 0x52000000, (uint8_t[]){0x7C}, 1);
    lcd_send_cmd_log(io, 0x53000000, (uint8_t[]){0x1C}, 1);
    lcd_send_cmd_log(io, 0x54000000, (uint8_t[]){0x77}, 1);
    lcd_send_cmd_log(io, 0x46000000, (uint8_t[]){0x0A}, 1);
    lcd_send_cmd_log(io, 0x47000000, (uint8_t[]){0x2A}, 1);
    lcd_send_cmd_log(io, 0x48000000, (uint8_t[]){0x0A}, 1);
    lcd_send_cmd_log(io, 0x49000000, (uint8_t[]){0x1A}, 1);
    lcd_send_cmd_log(io, 0x56000000, (uint8_t[]){0x43}, 1);
    lcd_send_cmd_log(io, 0x57000000, (uint8_t[]){0x42}, 1);
    lcd_send_cmd_log(io, 0x58000000, (uint8_t[]){0x3C}, 1);
    lcd_send_cmd_log(io, 0x59000000, (uint8_t[]){0x64}, 1);
    lcd_send_cmd_log(io, 0x5A000000, (uint8_t[]){0x41}, 1);
    lcd_send_cmd_log(io, 0x5B000000, (uint8_t[]){0x3C}, 1);
    lcd_send_cmd_log(io, 0x5C000000, (uint8_t[]){0x02}, 1);
    lcd_send_cmd_log(io, 0x5D000000, (uint8_t[]){0x3C}, 1);
    lcd_send_cmd_log(io, 0x5E000000, (uint8_t[]){0x1F}, 1);
    lcd_send_cmd_log(io, 0x60000000, (uint8_t[]){0x80}, 1);
    lcd_send_cmd_log(io, 0x61000000, (uint8_t[]){0x3F}, 1);
    lcd_send_cmd_log(io, 0x62000000, (uint8_t[]){0x21}, 1);
    lcd_send_cmd_log(io, 0x63000000, (uint8_t[]){0x07}, 1);
    lcd_send_cmd_log(io, 0x64000000, (uint8_t[]){0xE0}, 1);
    lcd_send_cmd_log(io, 0x65000000, (uint8_t[]){0x01}, 1);
    lcd_send_cmd_log(io, 0xCA000000, (uint8_t[]){0x20}, 1);
    lcd_send_cmd_log(io, 0xCB000000, (uint8_t[]){0x52}, 1);
    lcd_send_cmd_log(io, 0xCC000000, (uint8_t[]){0x10}, 1);
    lcd_send_cmd_log(io, 0xCD000000, (uint8_t[]){0x42}, 1);
    lcd_send_cmd_log(io, 0xD0000000, (uint8_t[]){0x20}, 1);
    lcd_send_cmd_log(io, 0xD1000000, (uint8_t[]){0x52}, 1);
    lcd_send_cmd_log(io, 0xD2000000, (uint8_t[]){0x10}, 1);
    lcd_send_cmd_log(io, 0xD3000000, (uint8_t[]){0x42}, 1);
    lcd_send_cmd_log(io, 0xD4000000, (uint8_t[]){0x0A}, 1);
    lcd_send_cmd_log(io, 0xD5000000, (uint8_t[]){0x32}, 1);

    // Match the vendor Arduino_NV3041A gamma table. Without these values the
    // panel can render recognizable but washed-out images.
    lcd_send_cmd_log(io, 0x80000000, (uint8_t[]){0x04}, 1);
    lcd_send_cmd_log(io, 0xA0000000, (uint8_t[]){0x00}, 1);
    lcd_send_cmd_log(io, 0x81000000, (uint8_t[]){0x07}, 1);
    lcd_send_cmd_log(io, 0xA1000000, (uint8_t[]){0x05}, 1);
    lcd_send_cmd_log(io, 0x82000000, (uint8_t[]){0x06}, 1);
    lcd_send_cmd_log(io, 0xA2000000, (uint8_t[]){0x04}, 1);
    lcd_send_cmd_log(io, 0x86000000, (uint8_t[]){0x2C}, 1);
    lcd_send_cmd_log(io, 0xA6000000, (uint8_t[]){0x2A}, 1);
    lcd_send_cmd_log(io, 0x87000000, (uint8_t[]){0x46}, 1);
    lcd_send_cmd_log(io, 0xA7000000, (uint8_t[]){0x44}, 1);
    lcd_send_cmd_log(io, 0x83000000, (uint8_t[]){0x39}, 1);
    lcd_send_cmd_log(io, 0xA3000000, (uint8_t[]){0x39}, 1);
    lcd_send_cmd_log(io, 0x84000000, (uint8_t[]){0x3A}, 1);
    lcd_send_cmd_log(io, 0xA4000000, (uint8_t[]){0x3A}, 1);
    lcd_send_cmd_log(io, 0x85000000, (uint8_t[]){0x3F}, 1);
    lcd_send_cmd_log(io, 0xA5000000, (uint8_t[]){0x3F}, 1);
    lcd_send_cmd_log(io, 0x88000000, (uint8_t[]){0x08}, 1);
    lcd_send_cmd_log(io, 0xA8000000, (uint8_t[]){0x08}, 1);
    lcd_send_cmd_log(io, 0x89000000, (uint8_t[]){0x0F}, 1);
    lcd_send_cmd_log(io, 0xA9000000, (uint8_t[]){0x0F}, 1);
    lcd_send_cmd_log(io, 0x8A000000, (uint8_t[]){0x17}, 1);
    lcd_send_cmd_log(io, 0xAA000000, (uint8_t[]){0x17}, 1);
    lcd_send_cmd_log(io, 0x8B000000, (uint8_t[]){0x10}, 1);
    lcd_send_cmd_log(io, 0xAB000000, (uint8_t[]){0x10}, 1);
    lcd_send_cmd_log(io, 0x8C000000, (uint8_t[]){0x16}, 1);
    lcd_send_cmd_log(io, 0xAC000000, (uint8_t[]){0x16}, 1);
    lcd_send_cmd_log(io, 0x8D000000, (uint8_t[]){0x14}, 1);
    lcd_send_cmd_log(io, 0xAD000000, (uint8_t[]){0x14}, 1);
    lcd_send_cmd_log(io, 0x8E000000, (uint8_t[]){0x11}, 1);
    lcd_send_cmd_log(io, 0xAE000000, (uint8_t[]){0x11}, 1);
    lcd_send_cmd_log(io, 0x8F000000, (uint8_t[]){0x14}, 1);
    lcd_send_cmd_log(io, 0xAF000000, (uint8_t[]){0x14}, 1);
    lcd_send_cmd_log(io, 0x90000000, (uint8_t[]){0x06}, 1);
    lcd_send_cmd_log(io, 0xB0000000, (uint8_t[]){0x06}, 1);
    lcd_send_cmd_log(io, 0x91000000, (uint8_t[]){0x0F}, 1);
    lcd_send_cmd_log(io, 0xB1000000, (uint8_t[]){0x0F}, 1);
    lcd_send_cmd_log(io, 0x92000000, (uint8_t[]){0x16}, 1);
    lcd_send_cmd_log(io, 0xB2000000, (uint8_t[]){0x16}, 1);

    lcd_send_cmd_log(io, 0xFF000000, (uint8_t[]){0x00}, 1);
    lcd_send_cmd_log(io, 0x11000000, NULL, 0); // Sleep Out
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_send_cmd_log(io, 0x21000000, NULL, 0); // Display Inversion ON (Fixes color inversion on IPS)
    lcd_send_cmd_log(io, 0x29000000, NULL, 0); // Display On
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    uint16_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    lcd_send_cmd_log(global_io_handle, 0x2A000000, (uint8_t[]){(x1 >> 8) & 0xFF, x1 & 0xFF, (x2 >> 8) & 0xFF, x2 & 0xFF}, 4);
    lcd_send_cmd_log(global_io_handle, 0x2B000000, (uint8_t[]){(y1 >> 8) & 0xFF, y1 & 0xFF, (y2 >> 8) & 0xFF, y2 & 0xFF}, 4);
    size_t len = (x2 - x1 + 1) * (y2 - y1 + 1) * 2;
    // 必须用 0x32002C00 (0x32 是硬件 Quad Write，0x2C 是屏幕 RAMWR)
    // 如果用 -1，ESP32 会直接爆射像素数据，导致屏幕花屏
    esp_lcd_panel_io_tx_color(global_io_handle, 0x32002C00, color_map, len);
}

esp_err_t esp_ai_ui_init(void)
{
    ESP_LOGI(TAG, "正在启动 4.3 寸视觉引擎 (处理变态级硬件共享引脚)...");

    // 强行拉高 GPIO 2 给屏幕供电
    // 稍后 AI 引擎启动后，I2S WS 时钟会接管这个脚，变成 PWM 供电
    gpio_set_direction(LCD_POWER_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_POWER_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_QSPI_SCK,
        .data0_io_num = LCD_QSPI_D0,
        .data1_io_num = LCD_QSPI_D1,
        .data2_io_num = LCD_QSPI_D2,
        .data3_io_num = LCD_QSPI_D3,
        .max_transfer_sz = LCD_H_RES * 40 * 2 + 128,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));

    lv_init();
    lv_extra_init();
    static lv_disp_draw_buf_t disp_buf;
    size_t buf_size = LCD_H_RES * 20;
    lv_color_t *buf1 = heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, buf_size);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_QSPI_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI3_HOST, &io_config, &global_io_handle));

    nv3041a_init_sequence(global_io_handle);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "视觉引擎已激活");
    return ESP_OK;
}


esp_err_t esp_ai_ui_show_rgb565_buffer_take(void *buffer, int width, int height, const char *label)
{
    if (!buffer || width != LCD_H_RES || height != LCD_V_RES) {
        ESP_LOGE(TAG, "RGB565缓冲无效: ptr=%p 尺寸=%dx%d 标记=%s", buffer, width, height, label ? label : "<null>");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_image_buf) {
        free(s_image_buf);
        s_image_buf = NULL;
    }
    s_image_buf = (lv_color_t *)buffer;

    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
    s_image_dsc.header.always_zero = 0;
    s_image_dsc.header.w = LCD_H_RES;
    s_image_dsc.header.h = LCD_V_RES;
    s_image_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_image_dsc.data_size = LCD_H_RES * LCD_V_RES * sizeof(lv_color_t);
    s_image_dsc.data = (const uint8_t *)s_image_buf;

    if (s_image_obj && !lv_obj_is_valid(s_image_obj)) {
        ESP_LOGW(TAG, "显示前丢弃失效图片对象: %s", label ? label : "<buffer>");
        s_image_obj = NULL;
    }
    if (!s_image_obj) {
        s_image_obj = lv_img_create(lv_scr_act());
    }
    lv_img_set_src(s_image_obj, &s_image_dsc);
    lv_obj_align(s_image_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(s_image_obj);

    ESP_LOGI(TAG, "RGB565缓冲显示完成: %s %dx%d", label ? label : "<buffer>", width, height);
    return ESP_OK;
}

esp_err_t esp_ai_ui_show_bmp_file(const char *path)
{
    if (!path || path[0] == '\0') {
        ESP_LOGE(TAG, "BMP路径为空");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "打开BMP失败: %s", path);
        return ESP_FAIL;
    }

    uint8_t header[54];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        ESP_LOGE(TAG, "读取BMP头失败: %s", path);
        fclose(f);
        return ESP_FAIL;
    }

    if (header[0] != 'B' || header[1] != 'M') {
        ESP_LOGE(TAG, "不是BMP文件: %s", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t pixel_offset = read_le32(header + 10);
    uint32_t dib_size = read_le32(header + 14);
    int32_t src_w_signed = read_le32_signed(header + 18);
    int32_t src_h_signed = read_le32_signed(header + 22);
    uint16_t planes = read_le16(header + 26);
    uint16_t bpp = read_le16(header + 28);
    uint32_t compression = read_le32(header + 30);

    if (dib_size < 40 || planes != 1 || compression != 0 || src_w_signed == 0 || src_h_signed == 0 ||
        !(bpp == 16 || bpp == 24 || bpp == 32)) {
        ESP_LOGE(TAG, "不支持的BMP: %s dib=%lu planes=%u bpp=%u 压缩=%lu 宽=%ld 高=%ld",
                 path, (unsigned long)dib_size, planes, bpp, (unsigned long)compression,
                 (long)src_w_signed, (long)src_h_signed);
        fclose(f);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int src_w = abs(src_w_signed);
    const int src_h = abs(src_h_signed);
    const bool bottom_up = src_h_signed > 0;
    const int bytes_per_pixel = bpp / 8;
    const int row_size = ((src_w * bpp + 31) / 32) * 4;
    const int draw_w = src_w > LCD_H_RES ? LCD_H_RES : src_w;
    const int draw_h = src_h > LCD_V_RES ? LCD_V_RES : src_h;

    uint8_t *row = heap_caps_malloc(row_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) {
        row = heap_caps_malloc(row_size, MALLOC_CAP_8BIT);
    }
    if (!row) {
        ESP_LOGE(TAG, "BMP行缓冲内存不足: 行=%d 路径=%s", row_size, path);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    const size_t screen_pixels = LCD_H_RES * LCD_V_RES;
    lv_color_t *new_buf = heap_caps_malloc(screen_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_buf) {
        new_buf = heap_caps_malloc(screen_pixels * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }
    if (!new_buf) {
        ESP_LOGE(TAG, "BMP屏幕缓冲内存不足: %u字节 路径=%s",
                 (unsigned)(screen_pixels * sizeof(lv_color_t)), path);
        free(row);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < screen_pixels; ++i) {
        new_buf[i] = lv_color_black();
    }

    for (int y = 0; y < draw_h; ++y) {
        int src_y = bottom_up ? (src_h - 1 - y) : y;
        long pos = (long)pixel_offset + (long)src_y * row_size;
        if (fseek(f, pos, SEEK_SET) != 0 || fread(row, 1, row_size, f) != (size_t)row_size) {
            ESP_LOGE(TAG, "读取BMP行失败: y=%d 位置=%ld 路径=%s", y, pos, path);
            free(new_buf);
            free(row);
            fclose(f);
            return ESP_FAIL;
        }

        lv_color_t *dst = new_buf + (y * LCD_H_RES);
        for (int x = 0; x < draw_w; ++x) {
            const uint8_t *px = row + x * bytes_per_pixel;
            uint8_t r;
            uint8_t g;
            uint8_t b;
            if (bpp == 16) {
                uint16_t rgb = read_le16(px);
                r = (uint8_t)(((rgb >> 11) & 0x1F) << 3);
                g = (uint8_t)(((rgb >> 5) & 0x3F) << 2);
                b = (uint8_t)((rgb & 0x1F) << 3);
            } else {
                b = px[0];
                g = px[1];
                r = px[2];
            }
            dst[x] = lv_color_make(r, g, b);
        }
    }

    free(row);
    fclose(f);

    if (s_image_buf) {
        free(s_image_buf);
        s_image_buf = NULL;
    }
    s_image_buf = new_buf;

    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
    s_image_dsc.header.always_zero = 0;
    s_image_dsc.header.w = LCD_H_RES;
    s_image_dsc.header.h = LCD_V_RES;
    s_image_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_image_dsc.data_size = screen_pixels * sizeof(lv_color_t);
    s_image_dsc.data = (const uint8_t *)s_image_buf;

    if (s_image_obj && !lv_obj_is_valid(s_image_obj)) {
        ESP_LOGW(TAG, "显示BMP前丢弃失效图片对象: %s", path);
        s_image_obj = NULL;
    }
    if (!s_image_obj) {
        s_image_obj = lv_img_create(lv_scr_act());
    }
    lv_img_set_src(s_image_obj, &s_image_dsc);
    lv_obj_align(s_image_obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_foreground(s_image_obj);

    ESP_LOGI(TAG, "BMP显示完成: %s 原图=%dx%d bpp=%u 绘制=%dx%d", path, src_w, src_h, bpp, draw_w, draw_h);
    return ESP_OK;
}
