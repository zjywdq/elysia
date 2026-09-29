idf_component_register(
    SRCS "TCP/tcp.c" "I2S_mirco/i2s_mirco.c" "I2S_voice/i2s_voice.c"
         "UART/uart.c" "BT_STATE/bt_state.c"
    INCLUDE_DIRS "include" "TCP" "I2S_mirco" "I2S_voice" "UART" "BT_STATE"
    REQUIRES driver esp_wifi esp_event esp_netif nvs_flash lwip esp_http_client
)
target_compile_options(${COMPONENT_LIB} PRIVATE -ffast-math -O3 -Wno-error=format)
