# 任务计划 (Task Plan - ESP-IDF 重构版)

## ✅ 已完成

### Stage 1: 硬件抽象层 (HAL)
- [x] **WiFi 稳定性**: 已实现自动联网，IP 固定为 `192.168.1.103`。
- [x] **PSRAM 适配**: 解决了 8MB Octal PSRAM 的崩溃问题。
- [x] **音频输入 (I2S Mic)**: 驱动已加载 (I2S_NUM_1)，实现了 DC Blocker 滤波，可采集真实音频峰值。
- [x] **音频输出 (I2S Speaker)**: 驱动已加载 (I2S_NUM_0)，通过了正弦波测试。
- [x] **显示供电**: 成功控制 GPIO 1 点亮屏幕背光。

### Stage 3: 核心通信与库复现
- [x] **esp-ai 库组件化复现**: 建立了 `components/esp-ai` 架构，实现了硬件、连接、业务的完美封装。
- [x] **WebSocket 基础连接**: 成功建立了与 `node.espai2.fun` 的长连接 (`ws_connected: true`)。
- [x] **双模唤醒功能**: 成功复现了 HTTP 远程触发 (`/api/wakeup`) 和 ASRPro 串口触发 (UART1)。

---

## ⚠️ 待办

### Stage 2: 音频引擎重构 (Audio)
- [x] **上行流上传**: 已实现将采集到的 PCM 音频通过 WebSocket 实时推送到服务器。
- [ ] **下行流处理**: 集成 MP3/AAC 解码器（ESP-ADF 或软解）。
- [ ] **流控机制**: 实现每秒向服务器汇报缓冲区状态。

### Stage 3: 业务协议深化 (JSON)
- [x] **esp-ai 库组件化复现**: 建立了 `components/esp-ai` 架构。
- [x] **业务握手逻辑**: 实现了连接成功后自动发送 `hello` 指令。

### Stage 5: 数据持久化与配网
- [x] **本地 NVS 存储**: 已实现 API Key 和 WiFi 凭据的持久化。
- [ ] **BLE 配网**: 通过蓝牙传输 WiFi 账号密码。

### Stage 4: 图形系统重构 (UI)
- [x] **LCD 驱动**: 已实现基于 SPI3_HOST 的 NV3041A (QSPI) 驱动。
- [x] **LVGL 集成**: 图形引擎已就绪，首屏欢迎界面渲染成功。

## 2026-06-23 补充
- [x] **静态图片上传显示**: `/api/upload` 和 `/upload` 已支持 PNG/JPG/BMP 保存并异步显示；小图验证通过。
- [x] **GIF 上传显示**: `.gif` 保存到 `/sdcard/gif/gif.gif`，LVGL GIF 显示验证通过。
- [x] **大 PNG 显示策略**: 已加入 PNGdec 逐行低内存解码/缩放，571KB 竖图验证显示成功。
- [x] **上传链路优化第一阶段**: multipart 解析和 SD 保存已优化；571KB/1.4MB 图片保存不再出现十几秒 SD 写入瓶颈。后续如需更快，需要继续优化 HTTP 接收或改分片上传。
- [x] **JPG 大图策略**: 已加入 libjpeg-turbo 逐行解码，支持 progressive JPG，并按 480x272 cover/crop 显示。
- [x] **文本对话接口迁移**: `/api/char_text` 已支持发送文本到 ESP-AI、SSE 返回文本片段，并在本地音频播放完成后正常结束。
