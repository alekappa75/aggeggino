idf_component_register(
    SRCS "bsp_cores3.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_spi esp_driver_gpio esp_driver_tsens
    PRIV_REQUIRES esp_timer
)
