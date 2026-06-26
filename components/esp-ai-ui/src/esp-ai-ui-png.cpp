#include "esp-ai-ui.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef __LINUX__
#define __LINUX__ 1
#endif
#include "pngdec_wide/PNGdec.h"

static const char *TAG = "ESP_AI_UI_PNG";

#define LCD_H_RES 480
#define LCD_V_RES 272
#define PNG_MAX_LINE_PIXELS 2048

static FILE *s_png_file = NULL;
static PNG *s_active_png = NULL;
static lv_color_t *s_screen = NULL;
static lv_color_t *s_line = NULL;
static lv_color_t *s_scaled_line = NULL;
static int16_t s_draw_x = 0;
static int16_t s_draw_y = 0;
static int16_t s_target_w = 0;
static int16_t s_target_h = 0;

static void *png_open_cb(const char *filename, int32_t *size)
{
    s_png_file = fopen(filename, "rb");
    if (!s_png_file) {
        if (size) *size = 0;
        return NULL;
    }
    fseek(s_png_file, 0, SEEK_END);
    long len = ftell(s_png_file);
    fseek(s_png_file, 0, SEEK_SET);
    if (size) *size = len > 0 ? (int32_t)len : 0;
    return s_png_file;
}

static void png_close_cb(void *handle)
{
    (void)handle;
    if (s_png_file) {
        fclose(s_png_file);
        s_png_file = NULL;
    }
}

static int32_t png_read_cb(PNGFILE *handle, uint8_t *buffer, int32_t length)
{
    (void)handle;
    if (!s_png_file || !buffer || length <= 0) return 0;
    int32_t read_len = (int32_t)fread(buffer, 1, (size_t)length, s_png_file);
    // SD 读取和 PNG 解压可能持续数秒，必须让 IDLE 任务有机会喂 watchdog。
    vTaskDelay(pdMS_TO_TICKS(1));
    return read_len;
}

static int32_t png_seek_cb(PNGFILE *handle, int32_t position)
{
    (void)handle;
    if (!s_png_file || position < 0) return 0;
    return fseek(s_png_file, position, SEEK_SET) == 0 ? position : 0;
}

static inline lv_color_t color_from_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return lv_color_make(r, g, b);
}

static inline lv_color_t blend_black(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a == 0) return lv_color_black();
    if (a == 255) return color_from_rgb(r, g, b);
    return color_from_rgb((uint8_t)(((uint16_t)r * a) >> 8),
                          (uint8_t)(((uint16_t)g * a) >> 8),
                          (uint8_t)(((uint16_t)b * a) >> 8));
}

static bool png_line_to_lvcolor(PNGDRAW *pDraw, lv_color_t *dst, size_t dst_pixels)
{
    if (!pDraw || !dst || !pDraw->pPixels || pDraw->iWidth < 0) return false;
    int width = pDraw->iWidth;
    if ((size_t)width > dst_pixels) return false;

    const uint8_t *src = pDraw->pPixels;
    const uint8_t *pal = pDraw->pPalette;

    switch (pDraw->iPixelType) {
        case PNG_PIXEL_TRUECOLOR:
            for (int x = 0; x < width; ++x) {
                dst[x] = color_from_rgb(src[0], src[1], src[2]);
                src += 3;
            }
            return true;
        case PNG_PIXEL_TRUECOLOR_ALPHA:
            for (int x = 0; x < width; ++x) {
                dst[x] = blend_black(src[0], src[1], src[2], src[3]);
                src += 4;
            }
            return true;
        case PNG_PIXEL_GRAYSCALE:
            if (pDraw->iBpp == 8) {
                for (int x = 0; x < width; ++x) {
                    uint8_t c = *src++;
                    dst[x] = color_from_rgb(c, c, c);
                }
                return true;
            }
            if (pDraw->iBpp == 1) {
                for (int x = 0; x < width; ++x) {
                    uint8_t packed = src[x >> 3];
                    uint8_t c = (packed & (0x80 >> (x & 7))) ? 255 : 0;
                    dst[x] = color_from_rgb(c, c, c);
                }
                return true;
            }
            break;
        case PNG_PIXEL_GRAY_ALPHA:
            for (int x = 0; x < width; ++x) {
                uint8_t c = src[0];
                uint8_t a = src[1];
                dst[x] = blend_black(c, c, c, a);
                src += 2;
            }
            return true;
        case PNG_PIXEL_INDEXED:
            if (!pal) return false;
            for (int x = 0; x < width; ++x) {
                uint8_t idx = 0;
                switch (pDraw->iBpp) {
                    case 8:
                        idx = src[x];
                        break;
                    case 4: {
                        uint8_t packed = src[x >> 1];
                        idx = (x & 1) ? (packed & 0x0F) : (packed >> 4);
                        break;
                    }
                    case 2: {
                        uint8_t packed = src[x >> 2];
                        idx = (packed >> (6 - ((x & 3) * 2))) & 0x03;
                        break;
                    }
                    case 1: {
                        uint8_t packed = src[x >> 3];
                        idx = (packed >> (7 - (x & 7))) & 0x01;
                        break;
                    }
                    default:
                        return false;
                }
                const uint8_t *p = &pal[(size_t)idx * 3];
                uint8_t a = pDraw->iHasAlpha ? pal[768 + idx] : 255;
                dst[x] = blend_black(p[0], p[1], p[2], a);
            }
            return true;
    }
    return false;
}

static void png_draw_cb(PNGDRAW *pDraw)
{
    if (!s_active_png || !s_screen || !s_line || !pDraw) return;
    if (!png_line_to_lvcolor(pDraw, s_line, PNG_MAX_LINE_PIXELS)) {
        ESP_LOGW(TAG, "PNG行转换失败 y=%d 宽=%d 类型=%d bpp=%d",
                 pDraw->y, pDraw->iWidth, pDraw->iPixelType, pDraw->iBpp);
        return;
    }

    const int src_h = s_active_png->getHeight();
    int dest_y0 = s_draw_y + (int)(((int32_t)pDraw->y * s_target_h) / src_h);
    int dest_y1 = s_draw_y + (int)((((int32_t)pDraw->y + 1) * s_target_h) / src_h);
    if (dest_y1 <= dest_y0) dest_y1 = dest_y0 + 1;
    if (dest_y1 <= 0 || dest_y0 >= LCD_V_RES) return;
    if (dest_y0 < 0) dest_y0 = 0;
    if (dest_y1 > LCD_V_RES) dest_y1 = LCD_V_RES;

    lv_color_t *src_line = s_line;
    int draw_w = pDraw->iWidth;
    if (s_target_w != pDraw->iWidth) {
        if (!s_scaled_line) return;
        for (int x = 0; x < s_target_w; ++x) {
            int src_x = (int)(((int32_t)x * pDraw->iWidth) / s_target_w);
            if (src_x < 0) src_x = 0;
            if (src_x >= pDraw->iWidth) src_x = pDraw->iWidth - 1;
            s_scaled_line[x] = s_line[src_x];
        }
        src_line = s_scaled_line;
        draw_w = s_target_w;
    }

    int16_t x = s_draw_x;
    int src_offset = 0;
    if (x < 0) {
        src_offset = -x;
        if (src_offset >= draw_w) return;
        draw_w -= src_offset;
        x = 0;
    }
    if (x + draw_w > LCD_H_RES) {
        draw_w = LCD_H_RES - x;
    }
    if (draw_w <= 0) return;

    for (int yy = dest_y0; yy < dest_y1; ++yy) {
        memcpy(&s_screen[(size_t)yy * LCD_H_RES + x], src_line + src_offset, (size_t)draw_w * sizeof(lv_color_t));
    }

    if ((pDraw->y % 16) == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern "C" esp_err_t esp_ai_ui_show_png_file(const char *path)
{
    if (!path || path[0] == '\0') {
        ESP_LOGE(TAG, "PNG路径为空");
        return ESP_ERR_INVALID_ARG;
    }

    void *png_mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!png_mem) png_mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_8BIT);
    if (!png_mem) {
        ESP_LOGE(TAG, "PNG解码器内存不足: 需要=%u", (unsigned)sizeof(PNG));
        return ESP_ERR_NO_MEM;
    }

    PNG *png = new (png_mem) PNG();
    s_active_png = png;
    int rc = png->open(path, png_open_cb, png_close_cb, png_read_cb, png_seek_cb, png_draw_cb);
    if (rc != PNG_SUCCESS) {
        ESP_LOGE(TAG, "打开PNG失败: %s rc=%d", path, rc);
        s_active_png = NULL;
        png->~PNG();
        free(png_mem);
        return ESP_FAIL;
    }

    int png_w = png->getWidth();
    int png_h = png->getHeight();
    if (png_w <= 0 || png_h <= 0 || png_w > PNG_MAX_LINE_PIXELS) {
        ESP_LOGE(TAG, "不支持的PNG尺寸: %s %dx%d 最大行宽=%d", path, png_w, png_h, PNG_MAX_LINE_PIXELS);
        png->close();
        s_active_png = NULL;
        png->~PNG();
        free(png_mem);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int32_t target_w = png_w;
    int32_t target_h = png_h;
    uint32_t scale_w = (((uint32_t)LCD_H_RES * 1024U) + (uint32_t)png_w - 1U) / (uint32_t)png_w;
    uint32_t scale_h = (((uint32_t)LCD_V_RES * 1024U) + (uint32_t)png_h - 1U) / (uint32_t)png_h;
    uint32_t scale = scale_w > scale_h ? scale_w : scale_h;
    if (scale == 0) scale = 1;
    target_w = ((int32_t)png_w * (int32_t)scale + 1023) / 1024;
    target_h = ((int32_t)png_h * (int32_t)scale + 1023) / 1024;
    if (target_w < LCD_H_RES) target_w = LCD_H_RES;
    if (target_h < LCD_V_RES) target_h = LCD_V_RES;

    s_target_w = (int16_t)target_w;
    s_target_h = (int16_t)target_h;
    s_draw_x = (LCD_H_RES - s_target_w) / 2;
    s_draw_y = (LCD_V_RES - s_target_h) / 2;

    s_screen = (lv_color_t *)heap_caps_calloc((size_t)LCD_H_RES * LCD_V_RES, sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_line = (lv_color_t *)heap_caps_malloc((size_t)PNG_MAX_LINE_PIXELS * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_screen || !s_line) {
        ESP_LOGE(TAG, "PNG缓冲内存不足: screen=%p line=%p", s_screen, s_line);
        if (s_screen) free(s_screen);
        if (s_line) free(s_line);
        s_screen = NULL;
        s_line = NULL;
        png->close();
        s_active_png = NULL;
        png->~PNG();
        free(png_mem);
        return ESP_ERR_NO_MEM;
    }
    if (s_target_w != png_w) {
        s_scaled_line = (lv_color_t *)heap_caps_malloc((size_t)s_target_w * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_scaled_line) {
            ESP_LOGE(TAG, "PNG缩放行缓冲内存不足: 目标宽=%d", s_target_w);
            free(s_screen);
            free(s_line);
            s_screen = NULL;
            s_line = NULL;
            png->close();
            s_active_png = NULL;
            png->~PNG();
            free(png_mem);
            return ESP_ERR_NO_MEM;
        }
    }

    rc = png->decode(NULL, 0);
    png->close();
    s_active_png = NULL;
    png->~PNG();
    free(png_mem);

    if (s_scaled_line) {
        free(s_scaled_line);
        s_scaled_line = NULL;
    }
    if (s_line) {
        free(s_line);
        s_line = NULL;
    }

    if (rc != PNG_SUCCESS) {
        ESP_LOGE(TAG, "PNG解码失败: %s rc=%d 原图=%dx%d 目标=%dx%d", path, rc, png_w, png_h, s_target_w, s_target_h);
        if (s_screen) free(s_screen);
        s_screen = NULL;
        return ESP_FAIL;
    }

    esp_err_t err = esp_ai_ui_show_rgb565_buffer_take(s_screen, LCD_H_RES, LCD_V_RES, path);
    s_screen = NULL;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "PNG显示完成: %s 原图=%dx%d 目标=%dx%d 位置=%d,%d", path, png_w, png_h, s_target_w, s_target_h, s_draw_x, s_draw_y);
    }
    return err;
}
