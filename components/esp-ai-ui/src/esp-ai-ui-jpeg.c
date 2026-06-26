#include "esp-ai-ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeglib.h"

static const char *TAG = "ESP_AI_UI_JPEG";

#define LCD_H_RES 480
#define LCD_V_RES 272

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
} jpeg_error_ctx_t;

static void jpeg_error_exit(j_common_ptr cinfo)
{
    jpeg_error_ctx_t *ctx = (jpeg_error_ctx_t *)cinfo->err;
    (*cinfo->err->format_message)(cinfo, ctx->message);
    longjmp(ctx->jump, 1);
}

esp_err_t esp_ai_ui_show_jpeg_file(const char *path)
{
    if (!path || path[0] == '\0') {
        ESP_LOGE(TAG, "JPEG路径为空");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "打开JPEG失败: %s", path);
        return ESP_FAIL;
    }

    struct jpeg_decompress_struct cinfo;
    jpeg_error_ctx_t jerr;
    memset(&cinfo, 0, sizeof(cinfo));
    memset(&jerr, 0, sizeof(jerr));
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    lv_color_t *screen = NULL;
    JSAMPARRAY row = NULL;
    esp_err_t result = ESP_FAIL;

    if (setjmp(jerr.jump)) {
        ESP_LOGE(TAG, "JPEG解码错误: %s 路径=%s", jerr.message, path);
        goto cleanup;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    const int src_w = (int)cinfo.output_width;
    const int src_h = (int)cinfo.output_height;
    if (src_w <= 0 || src_h <= 0 || cinfo.output_components < 3) {
        ESP_LOGE(TAG, "不支持的JPEG输出: %s %dx%d 颜色分量=%d", path, src_w, src_h, cinfo.output_components);
        result = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    uint32_t scale_w = (((uint32_t)LCD_H_RES * 1024U) + (uint32_t)src_w - 1U) / (uint32_t)src_w;
    uint32_t scale_h = (((uint32_t)LCD_V_RES * 1024U) + (uint32_t)src_h - 1U) / (uint32_t)src_h;
    uint32_t scale = scale_w > scale_h ? scale_w : scale_h;
    if (scale == 0) scale = 1;
    int target_w = ((int32_t)src_w * (int32_t)scale + 1023) / 1024;
    int target_h = ((int32_t)src_h * (int32_t)scale + 1023) / 1024;
    if (target_w < LCD_H_RES) target_w = LCD_H_RES;
    if (target_h < LCD_V_RES) target_h = LCD_V_RES;
    const int draw_x = (LCD_H_RES - target_w) / 2;
    const int draw_y = (LCD_V_RES - target_h) / 2;

    screen = (lv_color_t *)heap_caps_malloc((size_t)LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!screen) {
        screen = (lv_color_t *)heap_caps_malloc((size_t)LCD_H_RES * LCD_V_RES * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }
    if (!screen) {
        ESP_LOGE(TAG, "JPEG屏幕缓冲内存不足");
        result = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    for (size_t i = 0; i < (size_t)LCD_H_RES * LCD_V_RES; ++i) {
        screen[i] = lv_color_black();
    }

    row = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                     (JDIMENSION)(src_w * cinfo.output_components), 1);
    while (cinfo.output_scanline < cinfo.output_height) {
        JDIMENSION y = cinfo.output_scanline;
        jpeg_read_scanlines(&cinfo, row, 1);

        int dest_y0 = draw_y + (int)(((int32_t)y * target_h) / src_h);
        int dest_y1 = draw_y + (int)((((int32_t)y + 1) * target_h) / src_h);
        if (dest_y1 <= dest_y0) dest_y1 = dest_y0 + 1;
        if (dest_y1 <= 0 || dest_y0 >= LCD_V_RES) continue;
        if (dest_y0 < 0) dest_y0 = 0;
        if (dest_y1 > LCD_V_RES) dest_y1 = LCD_V_RES;

        const uint8_t *src = row[0];
        for (int yy = dest_y0; yy < dest_y1; ++yy) {
            lv_color_t *dst = &screen[(size_t)yy * LCD_H_RES];
            for (int x = 0; x < LCD_H_RES; ++x) {
                int target_x = x - draw_x;
                if (target_x < 0 || target_x >= target_w) continue;
                int src_x = (int)(((int32_t)target_x * src_w) / target_w);
                if (src_x < 0) src_x = 0;
                if (src_x >= src_w) src_x = src_w - 1;
                const uint8_t *px = src + (size_t)src_x * cinfo.output_components;
                dst[x] = lv_color_make(px[0], px[1], px[2]);
            }
        }
        if ((y % 16) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    jpeg_finish_decompress(&cinfo);
    result = esp_ai_ui_show_rgb565_buffer_take(screen, LCD_H_RES, LCD_V_RES, path);
    if (result == ESP_OK) {
        screen = NULL;
        ESP_LOGI(TAG, "JPEG显示完成: %s 原图=%dx%d 目标=%dx%d 位置=%d,%d 渐进=%d",
                 path, src_w, src_h, target_w, target_h, draw_x, draw_y,
                 cinfo.progressive_mode ? 1 : 0);
    }

cleanup:
    if (screen) free(screen);
    jpeg_destroy_decompress(&cinfo);
    fclose(fp);
    return result;
}
