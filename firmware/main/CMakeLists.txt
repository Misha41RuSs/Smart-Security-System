option(SECURITY_CLIMATE_NODE "Build the second ESP32 climate-only node" OFF)
if(SECURITY_CLIMATE_NODE)
    set(node_source "climate_main.c")
else()
    set(node_source "main.c")
endif()

idf_component_register(SRCS "${node_source}" "climate.c" "security_ble.c" "cloud.c"
                       PRIV_REQUIRES bt nvs_flash esp_hw_support esp_wifi esp_netif esp_event esp_http_client mbedtls json lwip
                       REQUIRES driver esp_driver_i2c esp_timer esp_adc
                       INCLUDE_DIRS "")

if(SECURITY_CLIMATE_NODE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SECURITY_CLIMATE_NODE=1)
endif()
