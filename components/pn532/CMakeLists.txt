idf_component_register(SRCS "pn532.c"
                        INCLUDE_DIRS "include"
                        REQUIRES esp_driver_gpio esp_driver_uart)
