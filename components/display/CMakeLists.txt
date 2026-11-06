# lvgl is needed for the FONTS: display_font_text draws Montserrat straight into
# the framebuffer, reading glyphs through the font API. It does not use the
# object system - only the data in flash - and the caller must hold LVGL's lock.
idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "include"
    # esp_timer: the flush statistics measure how long the display holds the
    #            SPI bus. It used to arrive transitively from another component,
    #            until a reconfigure recomputed the graph.
    REQUIRES bsp_cores3 esp_lcd lvgl esp_timer
)
