idf_component_register(SRCS "team_IQ.c"
                       REQUIRES lcd1602_i2c pn532
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_uart esp_driver_ledc esp_wifi esp_event nvs_flash esp_netif esp_http_client mbedtls)

