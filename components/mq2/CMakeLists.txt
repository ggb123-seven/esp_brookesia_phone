idf_component_register(
    SRCS
        "mq2_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_gpio
        esp_timer
)
