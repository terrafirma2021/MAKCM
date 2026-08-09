cmake_minimum_required(VERSION 3.16)

# This is the sole left-board firmware project.
# PROJECT_VER is resolved at configure time, so CMake must be told that the
# generation header is a configure input.  Without this a generation bump
# recompiles the sources but leaves the app descriptor on the previous
# `makcm-g<N>`, and the signed package is then rejected for an intrinsic
# identity mismatch that looks like a packaging bug.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/../common/OtaGeneration.h")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../common/OtaGeneration.h"
     _makcm_generation_header)
string(REGEX MATCH "MAKCM_OTA_PAIR_GENERATION[ \\t]+([0-9]+)"
       _makcm_generation_match "${_makcm_generation_header}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Unable to derive PROJECT_VER from OtaGeneration.h")
endif()
set(PROJECT_VER "makcm-g${CMAKE_MATCH_1}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(makcm_left_idf)

# esp_tinyusb's convenience layer normally owns the descriptor callbacks.
# This target supplies a runtime descriptor proxy instead, so compile those
# helper callbacks under private names and leave TinyUSB's public callback
# symbols available to app_main.cpp.
idf_component_get_property(_esp_tinyusb_lib espressif__esp_tinyusb COMPONENT_LIB)
if(_esp_tinyusb_lib)
    target_compile_definitions(${_esp_tinyusb_lib} PRIVATE
        tud_descriptor_device_cb=makcm_esp_tinyusb_descriptor_device_cb
        tud_descriptor_configuration_cb=makcm_esp_tinyusb_descriptor_configuration_cb
        tud_descriptor_string_cb=makcm_esp_tinyusb_descriptor_string_cb
        tud_descriptor_device_qualifier_cb=makcm_esp_tinyusb_descriptor_device_qualifier_cb
        tud_descriptor_other_speed_configuration_cb=makcm_esp_tinyusb_descriptor_other_speed_configuration_cb
    )
endif()

# The RAW proxy schedules endpoint drains through TinyUSB's task event queue.
# The default queue depth (16) is enough for a single class driver but can
# overflow when four interrupt IN completions and deferred drains coincide;
# TinyUSB asserts on that overflow.  Keep the queue bounded, but leave room
# for the six endpoint events plus EP0/control traffic without changing the
# DCD or adding another USB class.
idf_component_get_property(_tinyusb_lib espressif__tinyusb COMPONENT_LIB)
if(_tinyusb_lib)
    target_compile_definitions(${_tinyusb_lib} PRIVATE
        CFG_TUD_TASK_QUEUE_SZ=64
        CFG_TUD_TASK_EVENTS_PER_RUN=32
    )
endif()
