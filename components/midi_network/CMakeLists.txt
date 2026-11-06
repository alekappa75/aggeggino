idf_component_register(
    SRCS "src/midi2_net.c"
         "src/midi2_net_proto.c"
         "src/midi2_net_mdns.c"
         "src/midi2_ump.c"
         "src/midi2_ump_messages.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_common
    PRIV_REQUIRES lwip esp_timer esp_hw_support mbedtls mdns
)
