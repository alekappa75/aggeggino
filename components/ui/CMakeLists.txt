# fonts/  -> fonts generated with lv_font_conv, COMPILED: they go in SRCS.
#            lv_font_montserrat_bold_12.c is Montserrat wght=700 at 12 px, used
#            by the status strip badges. LVGL does not ship bold variants
#            (its lv_font_montserrat_* are Medium weight).
#            To regenerate it or produce other sizes, see the comment on
#            STRIP_FONT in ui.c.
# assets/ -> images NOT compiled (the bitmap padlocks, replaced by the vector
#            glyph): kept for reference, not in SRCS.
#            NOTE: the About screen's image is NOT here but under
#            settings_ui/assets, because settings_ui is what uses it - and ui
#            already depends on settings_ui, so the reverse would be a cycle.
idf_component_register(
    SRCS "ui.c" "fonts/lv_font_montserrat_bold_12.c"
    INCLUDE_DIRS "include"
    REQUIRES display bsp_cores3 esp_lvgl_port settings esp_timer audio settings_ui osc_out
)
