idf_component_register(SRCS "tcp.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_netif lwip)