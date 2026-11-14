idf_component_register(SRCS "main.c" "security_ble.c"
                       PRIV_REQUIRES bt nvs_flash esp_hw_support
                       REQUIRES driver esp_driver_i2c esp_timer esp_adc
                       INCLUDE_DIRS "")
