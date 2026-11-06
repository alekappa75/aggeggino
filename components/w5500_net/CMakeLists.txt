## The w5500_net component carries the glue plus the WIZnet ioLibrary sources
## (fetched by fetch_iolibrary.sh into ./ioLibrary). If ioLibrary is missing,
## the build fails with a clear message: run the script.

set(IOL_DIR "${CMAKE_CURRENT_LIST_DIR}/ioLibrary")
set(IOL_SRCS "")
set(IOL_INCS "")
if(EXISTS "${IOL_DIR}/wizchip_conf.c")
    set(IOL_SRCS
        "${IOL_DIR}/wizchip_conf.c"
        "${IOL_DIR}/socket.c"
        "${IOL_DIR}/W5500/w5500.c")
    set(IOL_INCS "${IOL_DIR}" "${IOL_DIR}/W5500")
else()
    message(WARNING
        "WIZnet ioLibrary not found in ${IOL_DIR}. "
        "Run components/w5500_net/fetch_iolibrary.sh before building.")
endif()

idf_component_register(
    SRCS "w5500_net.c" "dhcp_lite.c" ${IOL_SRCS}
    INCLUDE_DIRS "include" ${IOL_INCS}
    # esp_timer: the DHCP timings and the bus-possession measurements. It used to
    #            arrive transitively, which is how display stayed uncovered until
    #            the first reconfigure.
    REQUIRES bsp_cores3 esp_driver_spi esp_driver_gpio esp_timer
)

## Selects the W5500 chip in the ioLibrary
target_compile_definitions(${COMPONENT_LIB} PUBLIC _WIZCHIP_=5500)

## The WIZnet ioLibrary is third-party code: its warnings (indentation,
## uninitialised fields, unused static functions) must NOT be promoted to
## errors by -Werror. They are silenced ONLY for the vendored files,
## leaving our own code under strict warnings.
if(IOL_SRCS)
    set_source_files_properties(${IOL_SRCS} PROPERTIES COMPILE_FLAGS "-w")
endif()