# A 24 PPQN MIDI clock engine. It depends on no transport: the phase source
# and the sink arrive as callbacks from the application, so the same engine
# will serve Network MIDI 2.0, DIN and USB unchanged.
idf_component_register(
    SRCS "midi_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer
)
