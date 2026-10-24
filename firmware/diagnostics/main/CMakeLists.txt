idf_component_register(SRCS "main.cpp"
    PRIV_REQUIRES M5Unified M5GFX esp_wifi esp_event esp_netif nvs_flash
    esp_http_server esp_http_client mbedtls json esp_driver_usb_serial_jtag esp_driver_gpio
    INCLUDE_DIRS ".")
