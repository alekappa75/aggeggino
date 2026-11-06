# assets/ -> images compiled into the firmware. author_selfie.c is the About
#            screen: 320x240 RGB565, 150 KB of flash. It lives HERE and not in
#            ui/assets because this component is what uses it, and ui already
#            depends on settings_ui: putting it there would be a cycle.
idf_component_register(
    SRCS "settings_ui.c" "assets/author_selfie.c"
    INCLUDE_DIRS "include"
    # w5500_net: LAN > Status reads dhcp_lite_state() to say whether the
    #            address came from a server or from auto-ip.
    # sdcard: SD card > Store playlist reads sdcard_state() to warn when the
    #         setting is on but the slot is empty.
    # osc_out: the Custom page edits the message list, whose shape is defined
    #          there. The list itself lives in main.
    REQUIRES lvgl wifi_core wifi_client settings audio leds bsp_cores3 display about w5500_net sdcard osc_out esp_lvgl_port esp_wifi linkbridge esp_app_format esp_timer
)
