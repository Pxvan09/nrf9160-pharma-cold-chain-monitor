cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(pharma_tracker
    VERSION 1.0.0
    DESCRIPTION "Cellular cold-chain monitor for pharmaceutical shipments"
    LANGUAGES C
)

target_sources(app PRIVATE
    src/main.c
    src/sensors.c
    src/gnss_ctrl.c
    src/log_store.c
    src/excursion.c
    src/payload.c
    src/cloud_client.c
)
