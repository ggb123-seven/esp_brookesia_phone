idf_component_register(
    SRCS
        "onenet_cloud_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        dht11
        esp_http_client
        esp_netif
        esp_timer
        json
        mbedtls
        mqtt
)
