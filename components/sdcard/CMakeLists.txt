# sdcard: the microSD slot and the playlist written to it.
#
# It sits on the SPI bus bsp_cores3 brings up, sharing it with the display and
# the W5500 — see sdcard.h for what that costs and when writing is allowed.
idf_component_register(
    SRCS "sdcard.c"
    INCLUDE_DIRS "include"
    # settings: the playlist only collects when Store playlist is on, and the
    #           option is read where the line is prepared and again where it
    #           would be written.
    # display: a screenshot is a copy of the framebuffer, which lives there.
    REQUIRES bsp_cores3 settings display fatfs esp_driver_sdspi sdmmc esp_timer esp_driver_gpio
)
