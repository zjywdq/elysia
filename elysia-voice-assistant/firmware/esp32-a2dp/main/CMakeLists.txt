idf_component_register(SRCS "bt_app_core.c"
                            "main.c"
                    PRIV_REQUIRES bt nvs_flash driver freertos
                    EMBED_FILES "connected.pcm"
                    INCLUDE_DIRS ".")
