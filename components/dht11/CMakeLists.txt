idf_component_register(
    SRCS
        "dht11_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_driver_rmt
        esp_timer
)
