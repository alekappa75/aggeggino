# about: the About screen, its typewriter storyboard and the brush.
#
# Depends on display (framebuffer and banded transfer), bsp_cores3 (the touch
# read directly, because while we hold the LVGL lock its callback does not
# run) and esp_lvgl_port (the lock itself).
#
# It does NOT depend on ui or settings_ui: whoever opens the intro calls it,
# not the other way round. That keeps it a self-contained component, which can
# be read - or removed - without touching the rest.
# fonts/ -> fonts generated with lv_font_conv, COMPILED: they go in SRCS.
#           They live HERE and not in ui/fonts because ui depends (through
#           settings_ui) on about: putting them there and using them here
#           would close a cycle between components.
#
#           lv_font_montserrat_bold_18 - the title. Only 0x20-0x5F (capitals,
#           digits, punctuation): the title is uppercase, and the full range
#           cost 81 KB instead of 58.
#           lv_font_montserrat_ipa_18  - transcription and caption. ASCII plus
#           U+0292 (ezh) and U+02C8 (primary stress), which the Italian
#           transcription needs and LVGL's Montserrat do NOT have: those
#           cover only 0x20-0x7E plus the icons.
#
#           Regeneration (same recipe as the badges' bold_12):
#             lv_font_conv --no-compress --no-prefilter --bpp 4 --size 18 \
#               --font Montserrat-Bold.ttf -r 0x20-0x5F --format lvgl \
#               --force-fast-kern-format --lv-include lvgl.h -o <file>.c
#             lv_font_conv --no-compress --no-prefilter --bpp 4 --size 18 \
#               --font Montserrat-Medium.ttf -r 0x20-0x7F -r 0x292 -r 0x2C8 \
#               --format lvgl --force-fast-kern-format --lv-include lvgl.h -o <file>.c
#
#           --no-compress is NOT optional: font decompression is
#           disabled in sdkconfig (CONFIG_LV_USE_FONT_COMPRESSED absent).
idf_component_register(
    SRCS "about.c"
         "fonts/lv_font_montserrat_bold_18.c"
         "fonts/lv_font_montserrat_ipa_18.c"
    INCLUDE_DIRS "include"
    REQUIRES display bsp_cores3 lvgl esp_lvgl_port esp_timer audio esp_app_format
)
