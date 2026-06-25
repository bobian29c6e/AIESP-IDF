idf_component_register(
    SRCS
        "src/esp-ai-ui.c"
        "src/esp-ai-ui-png.cpp"
        "src/esp-ai-ui-jpeg.c"
        "src/pngdec_wide/PNGdec.cpp"
        "src/pngdec_wide/adler32.c"
        "src/pngdec_wide/crc32.c"
        "src/pngdec_wide/infback.c"
        "src/pngdec_wide/inffast.c"
        "src/pngdec_wide/inflate.c"
        "src/pngdec_wide/inftrees.c"
        "src/pngdec_wide/zutil.c"
    INCLUDE_DIRS "include" "src" "src/pngdec_wide"
    REQUIRES esp_lcd lvgl driver espressif__libjpeg-turbo
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE __LINUX__=1)
