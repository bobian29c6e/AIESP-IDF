# 进度记录 (Progress - ESP-IDF 重构版)

## 2026-06-20

### 视觉引擎 (Stage 4) 达成
- **QSPI 驱动对齐**: 实现了基于 SPI3_HOST 和 32-bit 指令协议的 NV3041A 高性能驱动。
- **LVGL 交付**: 成功运行 LVGL 8.3，并实现了屏幕画面的原生显示。

### 音频上行与数据持久化 (Stage 2 & 5) 突破
- **实时音频泵**: 实现了 `audio_uplink_task`，可将麦克风 PCM 数据实时打包发送。
- **业务逻辑复现**: 补全了 `esp_ai_wakeup` 接口，打通了从触发唤醒到云端 IAT 对话的全流程。
- **NVS 存储引擎**: 实现了 `esp-ai-nvs` 模块，API Key 等配置现已落地 Flash，摆脱了硬编码。

### 核心库复现与双模唤醒达成
- **组件化架构**: 建立了 `components/esp-ai` 库，实现了 1:1 的功能复现。
- **唤醒系统**: 成功打通了 HTTP 接口唤醒和 ASRPro 串口 (UART1, RX:44, TX:43) 硬件唤醒。
- **业务联调**: 设备已能自动完成“联网 -> 初始化 -> 鉴权 -> 待命”的全流程。

### HAL 层基础交付
- 完成了针对 JC4827W543 板子的全引脚校验。
- 解决了 OPI PSRAM 导致的 Boot Loop 故障。
- 优化了 32位 I2S 采样的直流滤波算法。

## 2026-06-23

### 业务迁移推进
- 修复 ESP-AI WebSocket 连接参数：完整人设文本不再直接拼入 URL，`ext3` 改为短人设 ID，URL 参数统一编码，避免中文导致 IDF websocket client 判定 URI 无效。
- `/api/getVoiceConfig` 支持查询和保存 `personaId`、`voiceId`、`personality`，并落地 NVS。
- `/api/volume` 保存音量到 NVS，并同步运行时音量。
- `/api/screensaver/settings` 支持保存和查询屏保设置。
- 新增 `/api/screensaver` 状态接口，提供 `active`、`idle_ms`、`timeout_ms`、`theme`。
- HTTP 路由上限调到 32，避免新增接口注册失败。

### 验证
- IDF 编译通过并烧录到 `/dev/cu.usbmodem11101`。
- 启动日志确认 IP `192.168.1.116`，HTTP server started，ESP-AI WebSocket connected。
- HTTP 验证通过：`/api/status`、`/api/getVoiceConfig`、`/api/volume`、`/api/screensaver`、`/api/screensaver/settings`、`/api/display/mode`。
- 图片上传 `/api/upload` 使用小 PNG 验证通过，保存 `/sdcard/upload/photo.png` 并显示成功。
- `/api/char_text` 验证通过，AI 文本和 TTS 正常，结束后状态回到 idle。

### GIF 上传与显示迁移
- 启用 LVGL GIF 支持：`CONFIG_LV_USE_GIF=y`。
- `/api/upload` 和 `/upload` 对 `.gif` 保存到 `/sdcard/gif/gif.gif` 后会触发异步显示。
- 编译通过并烧录到 `/dev/cu.usbmodem11101`，设备 IP `192.168.1.116`。
- 使用 17KB 示例 GIF 验证通过：HTTP 200，串口确认 `[UPLOAD] saved kind=gif file=/sdcard/gif/gif.gif`、`LVGL GIF shown: S:/gif/gif.gif`，状态接口仍返回 `ws_connected=true`。
- 使用 571KB PNG 验证上传成功：HTTP 200，保存 `/sdcard/upload/photo.png`，但显示阶段因 LVGL PNG 解码申请大块内存失败，未作为显示成功项。

### 大 PNG 低内存显示修复
- 移植 Arduino 版 PNGdec 逐行解码到 `components/esp-ai-ui`，避免 LVGL 直接为大 PNG 一次性申请整图解码内存。
- 修复 GIF/错误页清屏后，UI 层静态图片对象指针未同步失效的问题；该问题会导致后续 PNG/BMP 显示复用已删除对象并重启。
- 编译通过并烧录到 `/dev/cu.usbmodem11101`。
- 使用 571KB、828x1576 PNG 验证：HTTP 200，保存 `/sdcard/upload/photo.png`，逐行缩放到 142x270 居中显示成功，显示后 `/api/status` 返回 `ws_connected=true`，SRAM 约 60KB。
- 仍需后续优化：该 PNG 上传总耗时约 66 秒，其中接收约 50 秒、保存/解析约 16 秒，上传链路仍偏慢。

### JPG 渐进式解码与上传写入优化
- 引入 `libjpeg-turbo` 作为 JPEG 解码路径，`/sdcard/upload/photo.jpg` 不再依赖 LVGL 内置 JPEG 文件解码，解决渐进式 JPEG 显示失败问题。
- JPEG/PNG 显示策略改为 cover/crop：先按屏幕 480x272 铺满，再居中裁剪，避免图片显示成窄条或边缘大量黑边。
- 优化上传保存：multipart 尾部边界改为反向查找，SD 写入改为 16KB 内部 DMA 缓冲分块写入，避免直接从 PSRAM 大块写 SD。
- 验证 210KB 渐进式 JPG：HTTP 200，用时约 2.1 秒；日志 `JPEG shown ... progressive=1`，显示结束 `ESP_OK`，状态接口仍 `ws_connected=true`。
- 验证 1.4MB 普通 JPG：接收约 9.8 秒，SD 保存约 2.2 秒，HTTP 返回约 12.1 秒；日志 `JPEG shown ... progressive=0`，显示结束 `ESP_OK`，显示后 SRAM 约 60KB。

### 上传链路混合策略验证
- `/api/upload`/`/upload` 现在按大小选择路径：2MB 以下 multipart 图片继续使用 PSRAM 缓冲快速路径；超过 2MB 的文件走流式写 SD 兜底，避免大文件占用整包 PSRAM。
- 初版全流式路径实测稳定但速度变慢，因为边收边写 SD 会拖慢 HTTP 接收；因此改为混合策略，保留常见图片的快速体验。
- 编译和烧录通过，固件大小 `0x17a850`，app 分区剩余约 51%。
- 验证 1.4MB JPG：走 `buffered path`，HTTP 200，SD 保存约 1.9 秒，显示 `JPEG shown ... ESP_OK`，状态接口仍 `ws_connected=true`。
- 验证 571KB PNG：走 `buffered path`，HTTP 200，SD 保存约 1.0 秒，显示 `PNG shown ... ESP_OK`，状态接口仍 `ws_connected=true`。

### 屏保 HTTP 兼容接口迁移
- 保留现有 `/api/screensaver` 状态接口，不改变其 active/idle/status 语义。
- 新增 Arduino 兼容路径 `/screensaver` GET/POST，复用 `/api/screensaver/settings` 设置处理逻辑。
- `/api/screensaver/settings` 和 `/screensaver` 现在同时支持 snake_case 与 Arduino/小程序常见 camelCase 字段：`timeout_ms`/`timeout`、`time_color`/`timeColor`、`date_color`/`dateColor`、`time_size`/`timeSize`、`date_size`/`dateSize`。
- 编译并烧录到 `/dev/cu.usbmodem11101` 通过。
- HTTP 验证通过：`GET /screensaver`、`GET /screensaver?timeout=1&theme=1`、`POST /api/screensaver/settings`、`GET /api/screensaver/settings`、`GET /api/screensaver` 均返回 200，设置值能保存并回读。

### `/api/char_text` SSE 对话接口迁移修复
- 新增独立 `/api/char_text` 处理逻辑：HTTP 收到文本后调用 `esp_ai_tts()`，通过 SSE 返回 AI 文本片段，并在对话结束时返回 `event: round_end` 与 `event: end`。
- 修复首版栈溢出风险：SSE 缓冲从 HTTP 任务栈改为 heap 分配，避免接口调用后设备重启。
- 修复 `session_status` 回调数据缺失：服务器的 `status` 在顶层字段，IDF esp-ai 组件此前只传 `data` 给业务回调，导致 SSE 无法结束。
- 修复过早结束：`llm_end` 只代表当前文本生成结束，不代表 TTS/整轮回复结束；当前等待本地音频缓冲播放完成后再结束 SSE。
- 编译并烧录到 `/dev/cu.usbmodem11101` 通过。
- 实测 `/api/char_text`：HTTP 200，收到 3 段 AI 文本，音频播放完成后 19.1 秒正常返回 `event:end`；最终 `/api/status` 为 `ws_connected=true`、`busy=false`、`session_status=idle`，未重启。

## 2026-06-24

### ESP-AI SRAM/PSRAM 优化
- 启用网络相关 PSRAM 策略：`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`，让 WiFi/LwIP 缓冲优先使用 PSRAM。
- 将普通 `malloc()` 优先 SRAM 阈值从 16KB 降到 8KB：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192`，减少中等大小对象占用 SRAM。
- 启用 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`，把 lwIP/net80211/pp/bt 等静态 BSS 段放到 PSRAM。
- 音频缓冲策略保持安全边界：音频大 ringbuffer 放 PSRAM；I2S DMA、MP3 PCM 等实时/DMA 缓冲仍放 internal SRAM。
- ASRPro 串口任务的接收缓冲放 PSRAM；任务栈改为优先 PSRAM 4096 字节，失败时回退 internal 4096，避免 3072 栈导致溢出重启。

### 验证
- 编译通过并烧录到 `/dev/cu.usbmodem11101`。
- 90 秒启动/待机验证：无 watchdog、无崩溃，`/api/status` 返回 200。
- 优化前后对比：`WS启动前内存` 从此前约 22KB 提升到约 111KB；`/api/status free_sram` 从约 14KB/87KB 阶段提升到约 106KB。
- `/api/char_text` 复测通过：HTTP 200，用时约 10.6 秒，返回两段 SSE 文本并正常 `event:end`，结束后状态 `ws_connected=true`、`busy=false`、`session_status=idle`、`free_sram≈106KB`。

## 2026-06-24 esp-ai SRAM optimization follow-up

- Moved `ai_uplink_task` and `ai_spk_task` task stacks to PSRAM using `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` with internal-SRAM fallback.
- Kept real-time PCM buffers and I2S DMA buffers in internal SRAM; did not move Helix decoder working memory to PSRAM because prior testing showed playback watchdog risk.
- Built successfully with ESP-IDF/Ninja and flashed to `/dev/cu.usbmodem11101`.
- Runtime status after flash: `free_sram` improved to about `118 KB`, `free_psram` about `7.71 MB`, `ws_connected=true` after reconnect.
- `/api/char_text` test passed after the stack migration: HTTP 200, SSE returned text, no reboot and no stack overflow.
- Remaining observation: `client_available_audio` flow-control messages can still trigger `websocket_client: Could not lock ws-client within 100 timeout` during/after TTS playback. This is a WS contention issue, not a SRAM regression; it should be handled as a separate esp-ai flow-control throttling fix.

## 2026-06-24 esp-ai SRAM/WS stability follow-up

- Kept the large `/api/char_text` accumulation buffer in PSRAM, but moved the short-lived SSE send buffers back to internal SRAM after an interrupt WDT pointed at lwIP/HTTP send locking. This preserves the main 8KB SRAM saving while avoiding PSRAM-backed send buffers on the HTTP path.
- Moved runtime config strings (`api_key`, `volume`, `personaId`, `voiceId`, WiFi credentials, and the 2KB personality prompt buffer) to PSRAM BSS. Defaults now live as flash constants and are copied into PSRAM before NVS overrides are loaded.
- Removed two cJSON allocations from fixed esp-ai business messages: connection notice and wakeup `iat` start now use small RAM strings instead of cJSON object/print allocation.
- Added wakeup re-entry protection: `/api/status` now reports `can_wakeup=false` while busy, and `/api/wakeup` returns `409 AI_BUSY` instead of sending a second wakeup into an active session.
- Added initial IAT uplink protection: mic upload is skipped while busy/uploading and for 3 seconds after busy clears, to reduce WebSocket send/receive lock contention.

### Verification
- Build passed and firmware was flashed to `/dev/cu.usbmodem11101`.
- Idle status after optimization: `free_sram` improved from about `118KB` to about `120.5KB`, `ws_connected=true`.
- `/api/char_text` validated after the changes: HTTP 200, SSE ended normally, final status `ws_connected=true`, `busy=false`, `free_sram≈120KB`.
- `/api/wakeup` re-entry guard validated: first wakeup returns 200, immediate second wakeup returns 409 while busy, and `can_wakeup=false` during the active session.

## 2026-06-24

### esp-ai 唤醒与麦克风上传状态机对齐 Arduino

- 将 IDF 版 esp-ai 的麦克风上传开关从本地 `iat_active/busy` 推导，改为服务端状态驱动。
- `esp_ai_wakeup()` 现在只发送 `{"type":"iat","data":{"status":"start"}}`，不会直接允许麦克风上传。
- 只有服务端下发 `session_status=iat_start` 后才允许音频上行；`iat_end`、`play_audio`、`session_stop`、`auth_fail/error`、WS 断开都会关闭音频上行并清理状态。
- ASRPro 串口仍监听 `start`/`wakeup` 并调用同一个 `esp_ai_wakeup()`，因此 HTTP 唤醒和 ASRPro 唤醒后续走同一条业务路径。
- 编译通过并已通过 `/dev/cu.usbmodem11101` 烧录。
- 实测当前设备会收到服务器 `auth_fail`，因此无法完整验证 `iat_start` 后的真实语音上传；已验证 HTTP 唤醒不会提前开启麦克风上传，`auth_fail` 后 `/api/status` 恢复为 `idle`、`busy=false`，SRAM 约 120KB。

## 2026-06-25

### ESP-AI 鉴权与业务 ready 判定修复

- `/api/apikey` 现在兼容 `apikey`、`apiKey`、`api_key` 三种字段名，避免小程序/测试脚本字段名不一致导致 API Key 没有正确写入。
- ESP-AI WebSocket 连接 URI 对齐 Arduino esp-ai 格式，补齐 `/?v=1.1.0`、`ext5/ext6/ext7`，并保留 `ext1=api_key` 兼容服务端。
- 将 `esp_ai_is_connected()` 从“底层 WebSocket 物理连接”改为“业务消息已通过鉴权并可用”。收到 `auth_fail/error`、断开连接时会清除业务 ready。
- 服务端没有固定下发 `welcome`，实测会先下发 `stc_time/play_audio/session_status`，因此现在收到任意非 `auth_fail/error` 的正常业务消息后标记业务 ready。
- 用用户提供的新 API Key `3ae1****38e3` 写入成功，但服务端仍返回 `auth_fail`；用历史已绑定当前设备的 `3f60****bdb` 对照后鉴权通过。
- 当前设备保留可用的 `3f60****bdb` 配置，`/api/status` 返回 `ws_connected=true`、`can_wakeup=true`、`free_sram≈120KB`。
- `/api/char_text` 已验证：发送“简单说一句你好”后返回 SSE 文本“你好，我在。”，结束后状态 `idle/busy=false`。

## 2026-06-25 ESP-AI API Key 配置链路对齐

- 对照 Arduino `esp-ai` 库确认 WebSocket 参数中 `api_key` 与 `ext1` 是两个独立字段。
- IDF 版新增用户 Key 解析流程：`/api/apikey` 收到小程序用户 Key 后，请求 `http://api.espai2.fun/equipment/list`，从返回设备配置中提取 `iat_config.api_key`，失败时兼容直接智能体 Key。
- IDF 版 `esp_ai_server_config_t` 增加 `ext1` 字段，WebSocket URL 不再强行把 `api_key` 当作所有扩展字段。
- 编译通过并烧录到 `/dev/cu.usbmodem11101`。
- 用用户 Key `3ae1****38e3` 实测：设备端解析成功，保存智能体 Key `3f60****4bdb`，重启后 ESP-AI 连接成功，无 `auth_fail`。
- `/api/char_text` 回归测试通过：输入“简单说一句测试成功”，SSE 返回 `测试成功。`、`round_end`、`end`，最终 `/api/status` 为 `ws_connected=true`、`busy=false`、`can_wakeup=true`。

## 2026-06-25 IDF 版 Cron/提醒链路迁移

- 新增 `components/otakulink_reminder` 轻量提醒模块，使用 IDF 定时轮询替代 Arduino `CronAlarms` 的核心业务语义。
- ESP-AI 回调新增 `cron_task` 路由：兼容服务端顶层 `cron_task` 以及 `data` 内嵌任务两种格式。
- 支持 `clock_type=2` 的相对时间提醒：例如 `5秒后提醒我...`、`10秒后提醒我...`，也兼容简单 `*/N * * * * *` cron 秒级表达式作为兜底。
- 新增 `on_iat_cb` 文本兜底解析，后续 ASRPro/语音识别文本如果直接包含“几秒/分钟后提醒我”，也能创建本地提醒。
- 修复首次实现中的主任务栈溢出：提醒到点后不再在 `main` loop 里直接调用 `esp_ai_tts()`，而是调度独立 `reminder_tts` 任务播报。
- 编译通过并烧录到 `/dev/cu.usbmodem11101`。
- 实测 `/api/char_text` 输入 `5秒后提醒我测试cron功能`：服务端下发 `cron_task`，设备创建提醒，5 秒后触发并向 ESP-AI 发送提醒播报，最终音频播放完成，设备未重启，`/api/status` 恢复 `ws_connected=true`、`busy=false`、`free_sram≈117KB`。

## 2026-06-25 IDF 版业务 WS/网络恢复/健康监控迁移

- 新增 `components/otakulink_system`，实现独立业务 WebSocket，和 ESP-AI WebSocket 分离。
- 业务 WebSocket 使用保守心跳策略，并在 ESP-AI 对话、图片上传、低内存风险时自动暂停，避免和 ESP-AI 音频/文本通道争抢资源。
- 新增网络恢复状态机：监控 internal SRAM、最大连续块、WiFi RSSI、ESP-AI WS、业务 WS、上传状态；低内存持续风险时暂停业务 WS，严重时请求 WiFi 栈重置，上传中不重置 WiFi。
- `/api/status` 扩展诊断字段：`free_sram`、`free_psram`、`max_sram_block`、`min_sram`、`rssi`、`business_ws_connected`、`business_ws_paused`、`upload_active`、`network_recovery_active`、异常计数等。
- ESP-AI 文本发送增加短重试，缓解 `websocket_client` 偶发锁竞争导致的单次发送失败。
- 修复 ESP-AI MP3 播放稳定性问题：Helix 解码可能输出最多 2304 个 PCM sample，原缓冲只按 576 sample 分配，会越界破坏内存；现按最大输出样本分配并校验 `outputSamps`。
- 编译通过并烧录到 `/dev/cu.usbmodem11101`。
- 连续 10 轮 `/api/char_text` 稳定性测试全部通过：HTTP 返回 200，SSE 正常 `round_end/end`，设备未重启，`wifi_stack_resets=0`、`business_ws_errors=0`、`low_mem_events=0`。
- 测试后状态：ESP-AI connected，业务 WS 自动恢复，`free_sram≈96KB`，业务 WS 暂停时约 `102KB`，`min_sram≈95KB`，未观察到 SRAM 持续下降。

## 2026-06-29 设备绑定栈溢出修复

- 定位到启动反复重启原因：后台设备绑定任务 `device_bind` 栈溢出，日志为 `A stack overflow in task device_bind`。
- 修复方式：`devices/add` 响应缓冲从任务栈迁移到 heap/PSRAM，绑定任务栈从 6144 提升到 10240，并避免失败日志打印超长响应。
- 编译通过并烧录到 `/dev/cu.usbmodem11101`。
- 启动验证通过：未再出现 `device_bind` 栈溢出，设备加载 `binding_status=bound` 后跳过重复绑定。
- HTTP 验证通过：`/api/status` 返回 `ws_connected=true`、`bound=true`、`business_ws_connected=true`，SRAM 约 92KB。
