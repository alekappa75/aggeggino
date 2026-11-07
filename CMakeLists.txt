cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ============================================================================
# DJB_DEVELOPER — one switch for everything the development build needs
# ============================================================================
#
# At 1 the build carries the development tools; at 0 they leave the BINARY, not
# merely the view.
#
# WHAT IT GOVERNS:
#   - the Developer page in the settings, and screenshots to the card
#   - the CPU, SPI bus, beat jitter and task stack metrics
#   - the FreeRTOS instrumentation that feeds those metrics
#   - the level of log that gets compiled
#
# WHY ONE AND NOT SEVERAL. The metrics could otherwise be turned off while the
# instrumentation that feeds them stayed on — paying the cost at every context
# switch to collect numbers nobody reads. One switch moves them together.
# Published at 0: whoever clones this gets the production build. Set it to 1 to
# work on the firmware — but remember that the metrics build pays FreeRTOS
# instrumentation on every context switch, so timing measured with it is not
# the timing of the firmware people run.
set(DJB_DEVELOPER 0)

# The logs: the MAXIMUM COMPILED level follows the default one, so lowering it
# to WARN does not silence them at runtime but REMOVES them from the binary.
# That is 132 ESP_LOGI calls no longer even generated, and every line on the
# UART at 115200 blocks its caller for about 9 ms.
#
# MIND THIS: sdkconfig is generated ONCE and then kept. After changing this
# value the sdkconfig file must be DELETED for the defaults to be read again;
# otherwise the build uses the old configuration and the switch appears to do
# nothing.
#
# 'idf.py fullclean' is NOT enough, whatever one might assume: it empties the
# build directory and the documentation states plainly that the project
# configuration is not deleted. The file has to go.
#
#     rm sdkconfig && idf.py reconfigure
if(DJB_DEVELOPER)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.dev")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.prod")
endif()

idf_build_set_property(COMPILE_OPTIONS "-DDJB_DEVELOPER=${DJB_DEVELOPER}" APPEND)

# The version ends up in the application header (esp_app_desc_t) and the About
# screen reads it from there, together with the build date. There is no constant
# to keep aligned by hand: changing this number is enough.
#
# Without VERSION, ESP-IDF falls back to 'git describe' and, with no repository
# either, to the string "1" — which on screen looks like a defect.
#
# The project is still named aggeggino while the device is called AGGEGGINO:
# that is the internal name, present in components, log tags and directories,
# and changing it belongs to a pass of its own — some of those occurrences break
# the build or the OTA update if changed on their own.
project(aggeggino VERSION 1.0.0)
