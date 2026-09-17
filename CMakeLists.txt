cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(mqtt_bme680_bsec)


target_sources(app PRIVATE
    src/main.c

    src/sensor_bme680.c
    src/bsec_storage.c

    src/wifi_manager.c
    src/mqtt_manager.c

    external/bme68x/bme68x.c
    external/bsec/bsec_iaq.c
)


target_include_directories(app PRIVATE
    src
    external/bme68x
    external/bsec
)


add_library(bsec STATIC IMPORTED GLOBAL)

set_target_properties(bsec PROPERTIES
    IMPORTED_LOCATION
    ${CMAKE_CURRENT_SOURCE_DIR}/external/bsec/libalgobsec.a
)


target_link_libraries(app PUBLIC bsec)