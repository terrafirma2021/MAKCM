cmake_minimum_required(VERSION 3.16)

# Keep board-wide definitions directory-wide so the Arduino core and the
# application agree on Serial/HWCDC and PSRAM.
add_compile_definitions(
    ARDUINO_USB_MODE=1
    ARDUINO_USB_CDC_ON_BOOT=1
    BOARD_HAS_PSRAM
)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_LIST_DIR}/../common/OtaGeneration.h")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../common/OtaGeneration.h"
     _makcm_generation_header)
string(REGEX MATCH "MAKCM_OTA_PAIR_GENERATION[ \t]+([0-9]+)"
       _makcm_generation_match "${_makcm_generation_header}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "Unable to derive PROJECT_VER from OtaGeneration.h")
endif()
set(PROJECT_VER "makcm-g${CMAKE_MATCH_1}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(makcm_right_idf)

# Arduino 3.3.10 publicly requires esp-modbus 2.1.x.  The upstream component
# enables -Werror for itself and its current mDNS helper triggers GCC's
# stringop-truncation diagnostic even though MAKCM does not use Modbus.  Keep
# warnings strict everywhere else and demote only that upstream diagnostic.
idf_component_get_property(_makcm_esp_modbus_lib espressif__esp-modbus COMPONENT_LIB)
target_compile_options(${_makcm_esp_modbus_lib}
                       PRIVATE -Wno-error=stringop-truncation)
