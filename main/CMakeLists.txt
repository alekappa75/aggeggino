idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bsp_cores3 display w5500_net djlink osc_out midi_clock midi_network midi_usb wifi_core wifi_ap wifi_client linkbridge leds ui settings_ui settings dbserver atomic_stub audio esp_timer esp_driver_gpio esp_driver_uart
)
