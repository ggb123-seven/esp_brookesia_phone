idf_component_register(
    SRCS
        "dht11_service.c"
        "vendor/esp-idf-lib-dht/dht.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor/esp-idf-lib-dht"
    REQUIRES
        esp_driver_gpio
        esp_timer
)
