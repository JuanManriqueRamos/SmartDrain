idf_component_register(
    SRCS "main.c" "lora.c" "sd_storage.c" "ultrasonic_sensor.c"
         "wifi_time_sync.c" "ds3231.c" "hx710b.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console esp_event esp_netif esp_timer esp_driver_gpio
                  esp_driver_i2c esp_driver_spi esp_wifi fatfs lwip nvs_flash sdmmc
                  nopnop2002__sx126x
)
