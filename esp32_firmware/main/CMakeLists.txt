idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi nvs_flash esp_event mqtt esp_netif json bmp280 i2cdev esp_adc)
