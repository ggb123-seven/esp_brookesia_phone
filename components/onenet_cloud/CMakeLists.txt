idf_component_register(
    SRCS
        "onenet_cloud_service.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES
        "onenet_mqtt_root.pem"
    REQUIRES
        dht11
        esp_http_client
        esp_netif
        esp-tls
        esp_timer
        json
        lwip
        mbedtls
        mqtt
)
