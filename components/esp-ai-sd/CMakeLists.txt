idf_component_register(SRCS "src/esp-ai-sd.c"
                    INCLUDE_DIRS "include"
                    REQUIRES fatfs sdmmc driver esp_lcd)
