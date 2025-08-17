idf_component_register(SRCS "src/xz_chat.c" 
                            "src/xz_board_info.c" 
                            "src/xz_http_client_request.c"
                            "src/xz_util.c"
                            "src/xz_ws_protocol.c"
                            "src/xz_mqtt_protocol.c"
                       
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include" 
                       
                       REQUIRES http_client_util esp_common  mqtt mjson esp_websocket_client task_util
                        PRIV_REQUIRES nvs_flash app_update esp_app_format esp_partition esp_wifi spi_flash
                       )

add_compile_definitions(APP_NAME="${CMAKE_PROJECT_NAME}")