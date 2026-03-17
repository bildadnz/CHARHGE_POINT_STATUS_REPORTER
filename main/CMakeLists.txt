idf_component_register(SRCS "main.c"
                        PRIV_REQUIRES esp_driver_gpio esp_wifi mqtt esp_partition nvs_flash 
                        esp_netif esp_driver_uart vfs json app_update
                    INCLUDE_DIRS ".")
