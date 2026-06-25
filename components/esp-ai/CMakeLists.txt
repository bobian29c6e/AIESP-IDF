# 查找并包含所有 Helix 解码器源文件
file(GLOB HELIX_SRCS "src/helix/*.c")

idf_component_register(SRCS
                    "src/esp-ai.c"
                    "src/esp-ai-audio.c"
                    "src/esp-ai-common.c"
                    "src/esp-ai-ws.c"
                    "src/esp-ai-uart.c"
                    "src/esp-ai-nvs.c"
                    "src/esp-ai-helix-memory.c"
                    "drivers/esp-ai-i2s.c"
                    "drivers/esp-ai-power.c"
                    ${HELIX_SRCS}
                    INCLUDE_DIRS "include" "src/helix" "src/helix/utils"
                    REQUIRES esp_websocket_client json driver esp_wifi esp_event nvs_flash esp_netif)

# 关键：通过注入 __unix__ 宏，强制 Helix 使用通用 C 语言解码路径，解决 S3 平台汇编冲突
target_compile_definitions(${COMPONENT_LIB} PRIVATE __unix__)
