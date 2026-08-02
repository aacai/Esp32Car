idf_component_register(SRCS "main.c" "motor.c" "web_car.c" "ble_car.c" "car_status.c" "car_log.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "web/index.html"
                    REQUIRES esp_wifi esp_netif esp_event nvs_flash driver bt esp_http_server lwip app_update)
