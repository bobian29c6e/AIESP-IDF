# 发现与结论 (Findings - ESP-IDF 重构版)

## 硬件与驱动相关

### 1. PSRAM 模式
- **现象**: 默认配置下设备反复重启。
- **结论**: 该 ESP32-S3 板载 8MB PSRAM 必须显式开启 Octal 模式。
- **配置**: `CONFIG_SPIRAM_MODE_OCT=y`。

### 2. 音频引脚 (JC4827W543)
- **Audio Power EN**: `GPIO 46` (必须拉高，否则 Mic/Speaker 无供电)。
- **I2S Mic**: `I2S_NUM_1` (RX: 15, BCLK: 6, WS: 7)。
- **I2S Speaker**: `I2S_NUM_0` (TX: 41, BCLK: 42, WS: 2)。
- **Backlight**: `GPIO 1`。

### 3. ASRPro 串口
- **引脚**: RX=44, TX=43, 波特率 115200。
- **触发**: 匹配字符串 `"wakeup"`。

## 存储与持久化 (NVS)
- **命名空间**: `esp_ai_storage`。
- **存储项**: 已成功实现 `api_key` 和 `device_id` 的 Flash 持久化。
- **意义**: 摆脱了硬编码，为 BLE 动态配网提供了基础。

- **逻辑**: 使用 0x01 软件复位指令配合 32 位指令协议，彻底解决了 NV3041A 在 QSPI 模式下的唤醒难题。

## 业务与网络相关

### 1. WebSocket 连通性
- **解析问题**: 复杂的 URI 字符串容易导致解析失败。
- **优化**: 采用结构化配置，并加入 5 秒启动防抖延时。
- **路由**: 腾讯云等云端服务器对 Host 头部敏感，建议使用标准域名而非 IP 直连。

### 2. 音频质量
- **问题**: 录音峰值始终处于 32767。
- **原因**: 32位采样下的直流偏置。
- **解决**: 实现了 `(offset * 127 + sample) >> 7` 的平滑高通滤波。

## 2026-06-23 业务迁移发现

- IDF `esp_websocket_client` 对 URI 更严格，不能把中文人设原文直接拼到 WebSocket URL；否则会出现 `Invalid uri`。当前采用短 `personaId/ext3` 参与 WS 握手，完整 `personality` 只由 HTTP 配置接口保存和返回。
- 当前屏保迁移是基础状态版：已支持设置保存和 active/idle 状态记录，但还未迁移 Arduino 版完整时间/日期屏保渲染。
- 当前图片上传验证了小 PNG；大图/GIF 性能和 GIF 播放尚未作为本轮完成项验证。
- ASRPro 串口日志仍有乱码输入，当前 IDF 日志显示 RX=18/TX=17，与 Arduino 资料中的 RX=44/TX=43 可能不一致，后续迁移硬件唤醒时需要单独处理。

## 2026-06-23 图片显示补充发现

- LVGL GIF 文件源可以直接使用 `S:/gif/gif.gif`，小 GIF 已确认可显示和播放，上传后不影响 ESP-AI WebSocket 在线状态。
- 大 PNG 上传和显示是两段不同流程：HTTP 上传 571KB PNG 成功，但 LVGL PNG 解码阶段尝试申请约 5.2MB 临时内存并失败，日志为 `lv_mem_alloc: couldn't allocate memory (5219712 bytes)` 和 `decoder_open: error 83`。这说明后续需要限制客户端上传为屏幕尺寸图，或改为流式/预缩放解码，不能把这类问题归因成 HTTP 上传失败。
- 当前后台串口守护脚本不稳定，已用“测试脚本内同步打开串口 + HTTP 请求”的方式完成 GIF 显示验证。后续长期测试应单独修监听方式。

## 2026-06-23 大 PNG 显示崩溃根因

- 崩溃地址解析显示问题发生在 `lv_img_set_src()` / `lv_obj_invalidate()`，不是 HTTP 上传，也不是 PNG 文件接收。
- 根因是 GIF 显示和错误页会调用 `lv_obj_clean(lv_scr_act())` 清空屏幕，但 UI 层保存的 `s_image_obj` 静态指针没有置空；之后 PNG/BMP 显示复用已被 LVGL 删除的对象指针，触发 LoadStoreError。
- 修复方式：新增 `esp_ai_ui_reset_image_state()`，清屏前释放旧静态图片缓冲并清空对象指针；显示前额外用 `lv_obj_is_valid()` 防御悬空对象。
- 571KB PNG 已验证显示成功：`PNG shown: /sdcard/upload/photo.png src=828x1576 target=142x270 at=169,1`，设备未重启。
- 剩余风险：上传仍使用整包 multipart 缓冲和内存解析，速度不稳定；这是上传链路优化问题，和本次显示崩溃不是同一个根因。

## 2026-06-23 JPG 与上传链路发现

- LVGL 内置 JPEG 解码不能覆盖所有常规 JPG；已确认部分来自素材目录的 JPG 是 progressive JPEG，旧路径会上传成功但显示失败。
- 当前 JPEG 改为 `libjpeg-turbo` 逐行解码到 480x272 RGB565 显示缓冲，已验证 progressive=1 和普通 JPG 都能显示成功。
- 图片显示层仍由 LVGL 负责最终上屏，但文件格式解码不再完全依赖 LVGL 内置图片解码器。
- 上传慢主要分两段：网络接收和 SD 保存。SD 保存已从大 PNG 测试中的十几秒级降低到约 1-2 秒级；剩余耗时主要来自 HTTP 接收速率和文件大小。

## 2026-06-23 上传路径调优结论

- 全流式 multipart 写 SD 可以降低 PSRAM 峰值，但会显著拖慢常见图片上传；1.4MB JPG 全流式测试约 37 秒，而缓冲路径的 SD 保存只有约 1-2 秒。
- 当前更合理的策略是混合路径：小于 2MB 的普通图片使用 PSRAM 缓冲路径保证速度，大于 2MB 的文件使用流式路径保证不会因为整包缓存造成内存压力。
- 上传耗时日志显示，当前主要瓶颈是 HTTP 接收速率波动；SD 写入已经不是主要瓶颈。后续若要继续提速，需要优化客户端发送方式、HTTP server 接收策略，或做分片上传协议。

## 2026-06-23 屏保接口兼容发现

- Arduino 版把 `/api/screensaver`、`/screensaver`、`/api/screensaver/settings` 都作为屏保设置入口；IDF 版此前只完整支持 `/api/screensaver/settings`，并把 `/api/screensaver` 用作状态接口。
- 为避免破坏已迁移状态查询，IDF 保留 `/api/screensaver` 状态语义，只新增 `/screensaver` 作为设置兼容路径。
- 小程序/旧代码可能发送 `timeColor`、`dateColor`、`timeSize`、`dateSize` 等 camelCase 字段；IDF 现已兼容这些字段并转换/保存到本地配置。

## 2026-06-23 `/api/char_text` SSE 迁移发现

- IDF 版 esp-ai WebSocket 解析与 Arduino 行为存在一个关键差异：服务器下发 `session_status` 时，`status` 字段可能在 JSON 顶层，而不是 `data` 内。若只把 `data` 传给业务回调，HTTP SSE 层拿不到结束状态，会一直等到超时。
- `llm_end` 不能作为整轮对话结束依据；实测它在第一段文本后就会出现，后面仍会继续下发 `instruct` 和 `play_audio`。因此把 `llm_end` 当结束会导致小程序/HTTP 侧提前收尾，丢掉后续文本。
- 当前可靠结束依据是：服务端显式 `tts_real_end/end/session_end`，或本地音频模块确认 `Audio buffer drained` 后 `esp_ai_get_busy()==false` 且短时间没有新文本。
- `/api/char_text` 的 SSE 内容在 Python 测试里显示为乱码，是测试端按 ISO-8859-1 解码导致；ESP 日志中的中文文本正常，固件侧发出的字节是 UTF-8 文本。

## 2026-06-24 ESP-AI 内存优化发现

- 当前 IDF 版 esp-ai 的主要 SRAM 压力并不只来自业务代码本身，WiFi/LwIP/Bluetooth 的网络缓冲和静态段也会吃掉大量 internal SRAM。
- 启用 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` 后，启动日志明确出现 `WiFi/LWIP prefer SPIRAM`，且未自动放大 WiFi RX 数量和 BA 窗口；`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10`、`CONFIG_ESP_WIFI_RX_BA_WIN=6` 保持原值。
- 启用 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` 后，`Audio init before alloc` 的 internal SRAM 从约 130KB 进一步提升到约 146KB，`WS启动前内存` 约 111KB。
- 不能把所有 esp-ai 内存都迁到 PSRAM：曾尝试把 Helix MP3 解码器内存整体放 PSRAM，会导致 `ai_spk_task` watchdog；实时解码和 DMA 相关缓冲必须保留在 internal SRAM。
- ASRPro 任务栈 3072 不安全，实测 `/api/char_text` 场景出现 `A stack overflow in task asrpro_task`。修复方式是恢复 4096 栈，并优先用 PSRAM task stack。
- 一次 `/api/char_text` 验证中遇到 WS 连接超时和 DNS 解析失败，这是外部网络/服务器连通性问题；同一固件在 WS 恢复后直接复测 `/api/char_text` 成功，SRAM 保持约 106KB。

## 2026-06-24 esp-ai SRAM optimization findings

- Safe PSRAM migration target confirmed: audio task stacks. Moving `ai_uplink_task` (4 KB) and `ai_spk_task` (8 KB) to PSRAM increased available internal SRAM from about `106 KB` to about `118 KB` without immediate instability.
- Unsafe/kept-internal targets: I2S DMA buffers, PCM decode/upload buffers, and Helix decoder working memory. These remain internal to avoid DMA incompatibility and playback watchdog issues.
- Current residual risk is not heap pressure but WebSocket send contention from repeated `client_available_audio` flow-control reports. The device stayed alive and SRAM remained stable, but the log showed repeated lock timeout messages around TTS playback.

## 2026-06-24 esp-ai SRAM/WS follow-up findings

- HTTP/SSE send buffers should stay in internal SRAM. A test with PSRAM-backed SSE temporary buffers produced an interrupt WDT in the lwIP receive/queue path; moving only the short-lived send buffers back to internal SRAM removed the WDT while keeping the large accumulated text buffer in PSRAM.
- Runtime configuration text is safe to store in PSRAM because it is ordinary string state, not DMA, ISR, decoder, or socket TX memory. Moving it to PSRAM recovered about 2.4KB internal SRAM.
- Fixed JSON messages should not be passed to the WebSocket client from flash literals. Use RAM-backed small strings for WS sends; direct flash constants correlated with repeated `transport_poll_write` failures during connection notice testing.
- Text dialogue (`/api/char_text`) is stable with the current SRAM layout: no watchdog, no assert, and SRAM remains around 120KB after the request.
- Voice wakeup/IAT still has a separate protocol/concurrency risk: once IAT mode is active, continuous microphone binary upload can contend with server audio downlink on the same `esp_websocket_client`, causing `Could not lock ws-client` and occasional reconnects. Busy re-entry is now blocked, but full-duplex IAT needs a separate flow-control/state-machine pass before it can be considered stable.

## 2026-06-24 esp-ai 音频上行状态机差异

- Arduino esp-ai 的音频上行由 `esp_ai_start_send_audio` 控制：唤醒只请求服务端进入 IAT，真正上传麦克风音频必须等待服务端 `iat_start`；收到 `iat_end`、`play_audio`、`session_stop` 后停止。
- IDF 迁移版原先在 `esp_ai_wakeup()` 中直接设置本地 IAT 活跃，音频任务会基于本地状态尝试上传，容易和 TTS/控制消息抢同一个 WebSocket。
- 已将 IDF 版改为服务端状态驱动：`iat_start` 才上传，`play_audio/iat_end/session_stop/auth_fail/error/disconnect` 停止。
- 当前验证受 `auth_fail` 限制，无法证明服务端正常鉴权后的完整语音链路；但已证明本地不会在唤醒后提前上传麦克风音频，也不会在鉴权失败后保持 busy。

## 2026-06-25 ESP-AI auth_fail 根因

- `auth_fail` 不是 SRAM 或音频状态机问题。新 API Key `3ae1****38e3` 已正确写入 NVS，并出现在 ESP-AI 连接 URI 中，但服务端仍拒绝。
- 历史日志显示当前设备 `98:A3:16:E3:F9:10` 曾用 `3f60****bdb` 成功绑定和连接。切回该 key 后，服务端开始下发 `stc_time/play_audio/session_status`，证明 IDF 连接链路和 URI 格式可用。
- `/api/status` 不能只看底层 WebSocket 是否 connected；物理连接成功后仍可能马上 `auth_fail`。业务可用应以收到正常业务消息为准。
- 结论：用户给的新 key 目前不适用于当前设备或未在服务端完成绑定；固件端已能正确区分 auth_fail 和业务可用。

## 2026-06-25 IDF 版 API Key 类型混用根因

- 小程序保存的 `esp_api_key` 是用户授权 Key，用于请求云端 `equipment/list`。
- 小程序实际同步给 ESP 的是云端设备配置里的智能体 Key：优先 `iat_config.api_key`，其次 `llm_config.api_key`。
- 直接把用户授权 Key 写入 ESP-AI WebSocket 的 `api_key` 会收到服务端 `auth_fail`。
- 旧 Arduino `esp-ai` 库连接时会分别发送 `api_key` 与 `ext1`，IDF 版之前只有一个 Key 字段，导致协议语义被压扁。
- 修复后 `/api/apikey` 可同时兼容用户 Key 和直接智能体 Key：用户 Key 先解析，解析失败才按直接智能体 Key 保存。

## 2026-06-25 IDF 版 Cron/提醒迁移发现

- 服务端的 `cron_task` 可以作为顶层 WebSocket 消息出现，不能只读取 `data` 字段。IDF WebSocket 分发层需要在 `cron_task` 无标准 `data` 对象时把完整 root 消息传给业务层。
- 提醒触发后不能在 `main` loop 栈上直接调用 `esp_ai_tts()`。实测这会造成 `A stack overflow in task main` 并重启；正确方式是主循环只检测到期，实际播报放入独立任务执行。
- 服务端下发 `cron_task` 的时间可能晚于确认回复不少时间；例如 `5秒后提醒我...` 的确认回复先完成，约 20 秒后才收到服务端 `cron_task`。本地触发时间应从收到 `cron_task` 时开始计算，避免对未收到的任务做猜测。
- 当前实现只覆盖相对时间和简单秒/分/小时 cron 兜底，尚未实现完整 Cron 表达式、取消提醒、持久化恢复；这些属于后续增强，不影响本轮最小可用提醒链路。

## 2026-06-25 IDF 版业务 WS 与 ESP-AI 稳定性发现

- 业务 WS 与 ESP-AI WS 同时运行时，会额外占用约 6KB internal SRAM；业务 WS 暂停后 SRAM 会从约 96KB 回到约 102KB。这是连接资源占用，不是持续泄漏。
- ESP-AI 对话期间必须让业务 WS 保持暂停一段时间，不能只在发送瞬间暂停；否则会在 ESP-AI 还未进入 busy 前过早恢复，重新制造资源竞争。
- 10 轮短文本对话验证中，业务 WS 暂停/恢复机制有效：没有触发低内存保护，没有 WiFi 重置，没有业务 WS 错误计数增长。
- 本轮定位到一个真实内存破坏点：MP3 解码输出样本数可能大于原 PCM 缓冲容量，造成堆/任务内存破坏并引发随机重启。修复后重复测试未再出现该崩溃。
- ESP-AI WebSocket 仍可能在个别对话后由底层连接断开并自动重连；当前表现为短暂 `ws_connected=false` 后恢复，有时会触发一次连接提示音。它没有导致内存下降或对话失败，但后续如要进一步优化体验，应单独处理 ESP-AI 重连提示/握手去抖。
