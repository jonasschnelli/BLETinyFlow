idf_component_register(SRCS
               "src/ble_server.cpp"
               "src/gatt_service.cpp"
               "src/advertising.cpp"
               "src/image_service.cpp"
        INCLUDE_DIRS "src"
        REQUIRES bt nvs_flash esp_timer)
