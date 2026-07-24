idf_component_register(
    SRCS
        "local_dashboard_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        dht11
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        mq2
        parent_call_alert
)
