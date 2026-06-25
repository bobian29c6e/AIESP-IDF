#ifndef _ESP_AI_UI_H_
#define _ESP_AI_UI_H_

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LCD 硬件并启动 LVGL 图形引擎
 */
esp_err_t esp_ai_ui_init(void);

/**
 * @brief 从已挂载的文件系统读取 BMP 并显示。
 *
 * 支持常见的 24-bit/32-bit 未压缩 BMP，以及 16-bit RGB565 BMP。
 * path 需要是 POSIX 路径，例如 "/sdcard/upload/photo.bmp"。
 */
esp_err_t esp_ai_ui_show_bmp_file(const char *path);

/**
 * @brief 从 SD 卡读取 PNG，使用逐行低内存解码并显示。
 */
esp_err_t esp_ai_ui_show_png_file(const char *path);

/**
 * @brief 从 SD 卡读取 JPEG/JPG，解码到屏幕缓冲并铺满裁剪显示。
 */
esp_err_t esp_ai_ui_show_jpeg_file(const char *path);

/**
 * @brief 接管一块 480x272 RGB565/LVGL true-color 缓冲并显示。
 *
 * 成功后 UI 层负责释放 buffer；失败时调用方仍负责释放。
 */
esp_err_t esp_ai_ui_show_rgb565_buffer_take(void *buffer, int width, int height, const char *label);

/**
 * @brief 清理 UI 层保存的静态图片对象状态。
 *
 * 调用方在 lv_obj_clean(lv_scr_act()) 前调用，避免后续复用已被 LVGL 删除的对象指针。
 */
void esp_ai_ui_reset_image_state(void);

#ifdef __cplusplus
}
#endif

#endif
