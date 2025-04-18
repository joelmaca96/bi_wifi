idf_component_register(
    SRCS "bi_wifi.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi wifi_provisioning bt
)