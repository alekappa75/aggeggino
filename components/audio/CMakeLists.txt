idf_component_register(
    SRCS "audio.c" "audio_samples.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2s esp_driver_i2c bsp_cores3
)
