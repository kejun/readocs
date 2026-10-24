idf_component_register(SRCS "main.cpp" "storage.cpp" "network.cpp" "reader.cpp" "input.cpp"
    PRIV_REQUIRES M5Unified M5GFX esp_wifi esp_event esp_netif nvs_flash
    esp_http_server esp_http_client mbedtls json esp_driver_usb_serial_jtag
    esp_driver_gpio spiffs app_update esp_hw_support
    INCLUDE_DIRS ".")

# The shutdown cover lives at the repository root so it can be replaced without
# touching the firmware sources. Embedding keeps the device independent of a
# filesystem or SD card at power-off time.
target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_LIST_DIR}/../../../cover.jpg" BINARY RENAME_TO cover_jpg)
