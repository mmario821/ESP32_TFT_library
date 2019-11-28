set(SOURCES tft_demo.c)
idf_component_register(SRCS tft_demo.c PRIV_REQUIRES tft spiffs)

spiffs_create_partition_image(storage my_folder FLASH_IN_PROJECT)