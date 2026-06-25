# AIESP-IDF 迁移参考文件表

本文用于把 Arduino 版 ESP32-S3 固件迁移到 ESP-IDF 版时查原始实现。每个功能都列出需要参考的原项目文件或文件夹，路径全部使用绝对路径。

原则：

- 不只看单个函数，要按文件和模块理解完整业务链路。
- 先保持接口、BLE 指令、SD 路径、日志语义兼容，再考虑 IDF 化重构。
- 不把 Arduino 的阻塞写法原样搬到 IDF；长任务要拆成独立 task 或状态机。
- 不迁移任何硬编码密钥、WiFi 密码、设备私有标识。

## 0. 依赖库对照表

这一节专门标出“迁移时要看哪个库”。后面每个功能章节也会继续列具体业务文件。

| 功能范围 | Arduino 版实际参考库或源码路径 | IDF 版当前或建议对应位置 | 迁移说明 |
| --- | --- | --- | --- |
| ESP-AI 主库、TTS/ASR、服务端 WS、配置存储 | `/Users/bobian/Documents/Arduino/libraries/esp-ai` | `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai` | 这是最关键的库。IDF 版不能只照业务代码，要对齐 Arduino `esp-ai` 库里的事件回调、音频流、WebSocket、localData 行为。 |
| 业务 WebSocket | `/Users/bobian/Documents/Arduino/libraries/arduinoWebSockets` | `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/espressif__esp_websocket_client` | Arduino 版用 `WebSocketsClient`；IDF 版建议封装 `esp_websocket_client`，重新实现心跳、暂停、重连、低内存保护。 |
| LCD/GFX 基础绘制 | Arduino 头文件引用 `Arduino_GFX_Library.h`，业务代码在 `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/display_manager.cpp` | `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui` 和 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/eric-c-e__esp_lcd_nv3041` | Arduino GFX 不直接搬到 IDF。重点参考原显示流程、颜色格式、屏幕参数，IDF 用 esp_lcd/LVGL/NV3041 驱动实现。 |
| LVGL | Arduino 版当前不是主显示框架，主要参考业务显示代码 `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display` | `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/lvgl__lvgl` | IDF 版已有 LVGL，但不能默认替代原有图片/GIF/屏保业务，需要逐项重建。 |
| JPEG 解码 | `/Users/bobian/Documents/Arduino/libraries/TJpg_Decoder` | 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_image` | 参考 JPEG 缩放、居中、RGB565 输出逻辑。IDF 版可以换实现，但输出效果和路径要兼容。 |
| GIF 解码播放 | `/Users/bobian/Documents/Arduino/libraries/AnimatedGIF` | 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_gif` | 参考 GIF 文件读取、帧循环、PSRAM 占用和中断上传处理。 |
| PNG 解码 | `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/pngdec_wide` | 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_image` | 这是项目内置 PNG 解码源码，不是 Arduino libraries 里的库。迁移时重点注意 RGB565 转换和内存安全。 |
| JSON | `/Users/bobian/Documents/Arduino/libraries/Arduino_JSON` | IDF 可用 cJSON；当前 IDF 相关路径是 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components` 和 ESP-IDF 自带 cJSON | 迁移所有 HTTP JSON、服务器命令 JSON、Cron JSON 时要统一字段名。 |
| Cron/提醒 | `/Users/bobian/Documents/Arduino/libraries/CronAlarms` | 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_reminder` | IDF 版不要直接找 CronAlarms 替代品，建议用 esp_timer 或 FreeRTOS task 实现相同业务语义。 |
| BLE | Arduino ESP32 BLE 头文件：`BLEDevice.h`、`BLEServer.h`、`BLEUtils.h`、`BLE2902.h`，业务文件 `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/ble_integration.cpp` | 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_ble` | IDF 可用 NimBLE 或 Bluedroid。必须优先保证 UniApp iOS/Android 搜索和 `REQUEST_IP` 流程兼容。 |
| HTTP Server | Arduino 内置 `WebServer.h`，业务文件 `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp` | 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_http`，基于 ESP-IDF `esp_http_server` | 不要只照路由名，要照上传、SSE、CORS、超时、后台 task、上传 active 状态。 |
| SD/FS/SPI | Arduino 内置 `SD.h`、`FS.h`、`SPI.h`，业务文件 `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/display_manager.cpp` 和 `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp` | `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-sd` | IDF 版要显式管理挂载、SPI 总线、锁、路径和错误恢复。 |
| OTA/Flash 写入 | Arduino `Update.h` 加 ESP-IDF OTA 头文件，业务文件 `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/ota_manager.cpp` | 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_ota` | IDF 版应直接用 ESP-IDF OTA API，保留现有 HTTP 分片协议兼容。 |
| 按钮 | `/Users/bobian/Documents/Arduino/libraries/OneButton` | 后续如果仍需要按钮，建议放入 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_input` | 当前迁移优先级低，除非业务还依赖实体按钮。 |
| NeoPixel | `/Users/bobian/Documents/Arduino/libraries/Adafruit_NeoPixel` | 后续如果仍需要灯效，建议放入 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_led` | 当前迁移优先级低，先确认硬件是否实际使用。 |

## 1. 启动流程和主循环

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/main.c`
- 后续建议拆到 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/app_boot.c`
- 后续建议拆到 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/app_loop.c`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/main.ino`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/main.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/app_state.h`

迁移时重点看：

- `setup()` 里的初始化顺序。
- 首次启动进入 BLE 配网的条件。
- WiFi 连接成功后启动 HTTP、ESP-AI、业务 WebSocket 的顺序。
- 原 `loop()` 里必须保留的职责：ESP-AI 循环、业务 WS 循环、Cron、屏保检查、GIF 播放、系统监控、NTP 同步。
- 哪些旧逻辑不能继续放在一个大循环里阻塞运行。

## 2. 板级配置、引脚和硬件能力

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai/include`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui/include`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-sd/include`
- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_board`

必须参考的 Arduino 原项目文件和资料目录：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/hardware_config.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/hardware_config.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/main.h`
- `/Users/bobian/Desktop/OtakuLink/code/JC4827W543-资料/JC4827W543-C3773`

迁移时重点看：

- LCD 型号、分辨率、旋转方向、背光控制。
- I2S 麦克风和扬声器引脚。
- ASRPro 串口引脚和波特率。
- SD 卡 SPI 引脚和频率。
- PSRAM 类型必须按 OPI PSRAM 处理。
- Arduino 里能跑不代表 IDF 默认配置也能跑，sdkconfig 必须显式对齐硬件。

## 3. 编译配置、分区和固件大小

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/CMakeLists.txt`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/CMakeLists.txt`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/sdkconfig`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/sdkconfig.defaults`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/partitions.csv`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/partitions.csv`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/partitions.csv`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/AGENTS.md`

迁移时重点看：

- 现有固件已经接近 2MB，必须确认 OTA 分区是否放得下。
- Arduino 版依赖 `huge_app`、OPI PSRAM、CDCOnBoot、DIO FlashMode。
- IDF 版不能只用默认分区表。
- 如果保留 OTA，至少需要两个 app 分区；如果固件继续变大，需要重新设计分区。

## 4. 配置存储、NVS 和本地数据键

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_config`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/main.c`

必须参考的 Arduino 原项目文件和库文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/config_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/config_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/auto_config.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/auto_config.h`
- `/Users/bobian/Documents/Arduino/libraries/esp-ai/src`

迁移时重点看：

- WiFi 名称、WiFi 密码、API Key、设备 ID、音量、角色设定、音色、屏保配置分别存在哪些键里。
- 旧版 `esp_ai.getLocalData()` 和 `esp_ai.setLocalData()` 的行为要用 IDF NVS 等价实现。
- 不要把测试用的硬编码 WiFi、API Key、设备 ID 写进最终 IDF 版本。

## 5. BLE 配网和 IP 发现

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_ble`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/main.c`

必须参考的 Arduino 原项目文件和 UniApp 文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/BLEManager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/ble_integration.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/ble_integration.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/ble_integration_glue.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/Uniapp-esp32/pages/index/index.vue`

迁移时重点看：

- BLE 服务 UUID 和特征值 UUID 必须兼容。
- 小程序会发 `REQUEST_IP`、`SCAN_WIFI`、`WIFI:ssid,password`。
- 固件要返回 `IP:<address>`、`WIFI_STATUS:NOT_CONNECTED`、`WIFI_START:<count>`、WiFi 列表包、`WIFI_END`、`WIFI_ERROR:<message>`、`WIFI_OK:<message>`。
- Android 搜不到设备的问题要重点验证广播参数、设备名、service UUID、MTU 和 notify 开启时机。

## 6. 本地 HTTP API

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_http`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/main.c`

必须参考的 Arduino 原项目文件和 UniApp 文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/early_webserver.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/early_webserver_glue.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/Uniapp-esp32/pages/index/index.vue`
- `/Users/bobian/Desktop/OtakuLink/code/Uniapp-esp32/pages/home/home.vue`
- `/Users/bobian/Desktop/OtakuLink/code/Uniapp-esp32/pages/AIcontrol/AIcontrol.vue`

迁移时重点看：

- 必须兼容 `GET /api/status`。
- 必须兼容 `GET|POST /api/wakeup`。
- 必须兼容 `POST /api/char_text`。
- 必须兼容 `GET /api/getVoiceConfig`。
- 必须兼容 `GET /api/volume?value=0..1` 和 `GET /volume?value=0..1`。
- 必须兼容 `POST /api/upload` 和 `POST /upload`。
- 必须兼容 `GET|POST /api/screensaver/settings`。
- 必须兼容 `POST /api/apikey`。
- 必须兼容 `GET|POST /api/display/mode`。
- CORS 和 `OPTIONS` 处理不能漏。

## 7. 文本对话接口和 SSE

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_http`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai`

必须参考的 Arduino 原项目文件和库文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/device_callbacks.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/device_callbacks.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/timer_manager.cpp`
- `/Users/bobian/Documents/Arduino/libraries/esp-ai/src`

迁移时重点看：

- `/api/char_text` 的请求体格式。
- SSE 返回格式和结束事件。
- `session_id`、`session_status`、`tts_chunk_start`、`llm_end`、`tts_real_end` 的更新时机。
- 旧问题是 HTTP 任务里不能直接抢跑 ESP-AI / WebSocket 循环，IDF 版也要避免同类并发冲突。

## 8. ESP-AI 事件回调和命令路由

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai`
- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_ai_bridge`

必须参考的 Arduino 原项目文件和库文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/device_callbacks.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/device_callbacks.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/main.ino`
- `/Users/bobian/Documents/Arduino/libraries/esp-ai/src`

迁移时重点看：

- ESP-AI 的 `on_command`、`on_session_status`、`on_net_status`、`on_ready`、`on_connected_wifi` 等回调语义。
- 服务器下发命令如何路由到音量、屏保、提醒、业务状态。
- `cron_task` 可能是顶层消息，不一定经过 `on_command()`，IDF 版要在 WebSocket 接收路径兜底。

## 9. 业务 WebSocket 和网络恢复

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/espressif__esp_websocket_client`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai/src/esp-ai-ws.c`
- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_business_ws`

必须参考的 Arduino 原项目文件和库文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/websocket_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/websocket_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/timer_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/device_config.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/device_config.h`
- `/Users/bobian/Documents/Arduino/libraries/arduinoWebSockets/src`

迁移时重点看：

- 业务 WS 心跳策略。
- 重连必须是调度式，不能在定时器里阻塞等待。
- 上传、OTA、低内存期间要暂停或降频业务 WS，避免误伤 HTTP 连接。
- WiFi/TCP 栈重置要有明确状态机，不能把普通 WS 重连当成资源释放。

## 10. WiFi 连接和网络状态管理

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_wifi`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/main.c`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/device_config.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/device_config.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/timer_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/system_monitor.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/system_monitor.h`

迁移时重点看：

- WiFi 连接、断开、重连、低内存时重置网络栈的完整流程。
- 日志里必须能区分 WiFi 断开、业务 WS 断开、ESP-AI WS 断开。
- 不能让网络恢复逻辑打断正在进行的上传或 OTA。

## 11. 定时器、Cron 和提醒

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_timer`
- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_reminder`

必须参考的 Arduino 原项目文件和库文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/timer_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/timer_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/reminder_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/reminder_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/main.ino`
- `/Users/bobian/Documents/Arduino/libraries/esp-ai/src`

迁移时重点看：

- Arduino 版依赖 CronAlarms，IDF 版需要替换成 esp_timer 或独立 FreeRTOS task。
- `cron_task` 触发、存储、执行、取消和到点播报的完整链路。
- 旧问题是 cron 消息可能不进 `on_command()`，迁移时要从 WebSocket 原始消息层确认。

## 12. 系统监控和内存诊断

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_monitor`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/system_monitor.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/system_monitor.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/timer_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/logging.h`

迁移时重点看：

- heap、最大连续块、internalMax、PSRAM 的日志格式。
- 系统健康判断。
- 低内存保护阈值。
- task 栈水位日志。
- Watchdog 喂狗位置。

## 13. 显示初始化和基础绘制

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui/src/esp-ai-ui.c`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui/include/esp-ai-ui.h`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/lvgl__lvgl`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/eric-c-e__esp_lcd_nv3041`

必须参考的 Arduino 原项目文件和文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/display_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/display_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/screensaver_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/screensaver_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display`

迁移时重点看：

- NV3041A QSPI 屏幕初始化参数。
- 480x272 分辨率和旋转方向。
- 背光开启和关闭时机。
- Splash 绘制逻辑。
- 显示任务不能长期阻塞网络和 ESP-AI。

## 14. 静态图片显示：BMP、JPEG、PNG

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui/src/esp-ai-ui.c`
- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_image`

必须参考的 Arduino 原项目文件和文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/display_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/display_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/pngdec_wide`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/pngdec_wide_arm.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/pngdec_wide_dma.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/pngdec_wide_s3.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/s3_simd_rgb565.S`

迁移时重点看：

- `/upload/photo.bmp`、`/upload/photo.jpg`、`/upload/photo.png` 的路径兼容。
- RGB565 字节序、颜色转换、行缓冲、PSRAM 和 internal SRAM 的分配方式。
- PNG 显示曾出现 heap corruption，IDF 版要优先用安全路径验证，再考虑 SIMD/DMA 优化。
- 图片铺满、裁剪、缩放策略要和小程序预处理方案保持一致。

## 15. GIF 播放

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_gif`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/gif_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/gif_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/screensaver_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`

迁移时重点看：

- `/gif/gif.gif` 的路径兼容。
- GIF 文件大小判断、PSRAM 分配、帧缓冲释放。
- GIF 播放循环不能卡住 HTTP、ESP-AI 和业务 WS。
- 上传 GIF 中断时不能把半截文件当成功文件。

## 16. SD 卡初始化、文件系统和路径约定

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-sd/src/esp-ai-sd.c`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-sd/include/esp-ai-sd.h`
- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_storage`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/display_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/display_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/screensaver_manager.cpp`

迁移时重点看：

- `initSDCard()` 的重复初始化保护。
- `/upload`、`/gif` 目录创建。
- 上传临时文件、rename、remove、exists 的顺序。
- SD 锁必须覆盖上传写入、显示读取、屏保恢复读取。
- 锁等待要有超时日志，不能无限卡死。

## 17. 图片和 GIF 上传

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_http`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_storage`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_image`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/early_webserver.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/display/display_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/screensaver_manager.cpp`

迁移时重点看：

- `/api/upload` 和 `/upload` 都要保留。
- 支持 `.bmp`、`.jpg`、`.jpeg`、`.png`、`.gif`。
- `.tmp` 临时文件和最终文件替换顺序。
- `isUploadActive()` 类似状态必须保留，用于避免上传期间网络保护误伤。
- 上传诊断日志要保留：总字节、chunk 数、速度、最大间隔、写入耗时、heap、rssi。

## 18. 屏保和显示模式

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_screensaver`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/screensaver_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/screensaver_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/device_callbacks.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/timer_manager.cpp`

迁移时重点看：

- 屏保激活和退出条件。
- 用户活动通知。
- 默认显示内容必须是最后一次接收的静态图片，而不是固定 BMP。
- `/api/screensaver/settings` 和 `/api/display/mode` 的响应兼容。
- 退出屏保后恢复图片不能和上传抢 SD 锁。

## 19. 音频、I2S、ASR 和 TTS

IDF 目标位置：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai/src/esp-ai-audio.c`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai/drivers/esp-ai-i2s.c`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai/src/esp-ai-uart.c`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp_ai/src/esp-ai-audio.c`

必须参考的 Arduino 原项目文件和库文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/hardware_config.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/hardware_config.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/device_callbacks.cpp`
- `/Users/bobian/Documents/Arduino/libraries/esp-ai/src`

迁移时重点看：

- 麦克风、扬声器、ASRPro 串口的硬件配置。
- 唤醒、ASR 开始/结束、TTS 播放、音频流写入的完整链路。
- 之前出现过语音两段之间长空白，迁移时要重点验证音频队列和 WebSocket 接收不会互相阻塞。

## 20. OTA 固件升级

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_ota`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/partitions.csv`

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/ota_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/ota_manager.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main/partitions.csv`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/partitions.csv`

迁移时重点看：

- OTA start、chunk、finish、cancel、status 的接口语义。
- 写入 OTA 分区时必须暂停或降频 ESP-AI WS、业务 WS、GIF、图片显示。
- 固件大小和分区大小必须先算清楚。
- 大文件上传慢的问题不能靠简单加超时解决，要考虑分片确认、断点续传、校验和。

## 21. UniApp 兼容性

IDF 目标位置：

- 所有 BLE、HTTP、上传、屏保、音量、对话相关组件。

必须参考的 UniApp 文件：

- `/Users/bobian/Desktop/OtakuLink/code/Uniapp-esp32/pages/index/index.vue`
- `/Users/bobian/Desktop/OtakuLink/code/Uniapp-esp32/pages/home/home.vue`
- `/Users/bobian/Desktop/OtakuLink/code/Uniapp-esp32/pages/AIcontrol/AIcontrol.vue`

迁移时重点看：

- 小程序如何发现 BLE 设备和拿 IP。
- 小程序调用了哪些 HTTP 路由。
- 图片上传字段名、路径、超时策略。
- 音量响应 JSON。
- `/api/status` 的字段：`success`、`ws_connected`、`asr_ing`、`session_status`、`session_id`、`busy`、`can_wakeup`。
- 不要随意改字段名；如果必须改，要同步改 UniApp。

## 22. 日志和排障能力

IDF 目标位置：

- 后续建议新增 `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/otakulink_log`
- 所有组件统一使用同一套日志格式。

必须参考的 Arduino 原项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/include/logging.h`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/system_monitor.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/timer_manager.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/early_webserver.cpp`
- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/src/websocket_manager.cpp`

迁移时重点看：

- 串口日志要带时间戳。
- 上传、显示、WS、WiFi、ESP-AI、Cron、BLE 都要能从日志单独定位。
- 内存日志至少要包含 heap、最大连续块、internalMax、PSRAM。
- 不能只输出“失败”，要输出失败阶段和耗时。

## 23. Arduino 胶水文件迁移处理

IDF 目标位置：

- IDF 不需要 Arduino 胶水文件，但需要把里面桥接的实现落到对应 IDF component。

必须参考的 Arduino 原项目文件夹：

- `/Users/bobian/Desktop/OtakuLink/code/esp32s3-ai/main`

迁移时重点看：

- `main` 目录里的 `*_glue.cpp` 文件不是无用文件，里面可能把 `src` 模块接入 Arduino 编译。
- IDF 迁移时不能只看 `src`，必须确认每个 glue 文件是否包含了实际编译入口或平台适配。
- IDF 版应该用 CMake component 显式组织，不继续依赖 glue 文件。

## 24. 当前 AIESP-IDF 已有代码需要重点审查的位置

当前 IDF 项目文件：

- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/main/main.c`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-ui`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp-ai-sd`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/components/esp_ai`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/espressif__esp_websocket_client`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/lvgl__lvgl`
- `/Users/bobian/Desktop/OtakuLink/code/AIESP-IDF/managed_components/eric-c-e__esp_lcd_nv3041`

迁移时重点看：

- `main/main.c` 目前更像最小验证样例，不是完整业务迁移。
- `components/esp-ai` 和 `components/esp_ai` 命名相近，后续要统一，避免重复实现和链接混乱。
- LVGL 可以作为显示基础，但不能自动替代原有 BMP/JPG/PNG/GIF 业务路径。
- esp_websocket_client 可以替代 Arduino WebSockets，但心跳、重连、暂停、低内存保护要按本项目业务重新封装。

## 建议迁移顺序

1. 先迁移板级配置、日志、NVS、WiFi。
2. 再迁移 BLE 配网和 IP 返回，保证小程序能找到设备。
3. 再迁移 HTTP 基础接口：status、volume、getVoiceConfig、wakeup。
4. 再迁移 ESP-AI 对话链路和 SSE。
5. 再迁移 SD、图片上传、静态图片显示。
6. 再迁移 GIF、屏保、显示模式。
7. 再迁移业务 WS、Cron、提醒。
8. 最后迁移 OTA 和性能优化。

每一步都要做实机验证，不要等全部迁完再一起调。
