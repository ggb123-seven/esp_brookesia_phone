idf_component_register(
    SRCS
        "parent_call_alert_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_driver_uart
        esp_http_client
        esp_netif
        esp_timer
        json
)
