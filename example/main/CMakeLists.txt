idf_component_register(SRCS "main.c" "board.c" "xiaozhi_app.c"
                       INCLUDE_DIRS "./"
                       )


spiffs_create_partition_image(spiffs_data ../spiffs FLASH_IN_PROJECT)