idf_component_register(
    SRCS
        "../src/commands.cpp"
        "../src/efuse.cpp"
        "../src/esp_tasks.cpp"
        "../src/esp_usb_host.cpp"
        "../src/main.cpp"
        "../src/ota_receiver.cpp"
        "../src/passthrough.cpp"
        "../src/serialization.cpp"
    INCLUDE_DIRS
        "../include"
        "../../common"
    REQUIRES
        espressif__arduino-esp32
        bblanchon__arduinojson
        app_update
        driver
        efuse
        mbedtls
        nvs_flash
        usb
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    USB_IS_DEBUG=0
    FIRMWARE_VERSION="V1_3"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)

set(_makcm_ota_anchor
    "${CMAKE_CURRENT_LIST_DIR}/../../common/ota_trust_anchor.local.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_makcm_ota_anchor}")
if(EXISTS "${_makcm_ota_anchor}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        MAKCM_OTA_TRUST_ANCHOR_CONFIGURED=1)
endif()
