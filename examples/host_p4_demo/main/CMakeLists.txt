idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hosted_open esp_hosted nvs_flash esp_event esp_netif esp_wifi
)
