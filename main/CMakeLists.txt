idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-ai esp-ai-ui esp-ai-sd otakulink_reminder otakulink_system esp_wifi esp_event nvs_flash esp_http_server esp_http_client esp_netif driver bt)
