idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES makcm_core nvs_flash esp_timer esp_tinyusb
)

# ``__has_include`` alone is unsafe with a newly-created machine-local trust
# anchor and compiler caches: a previous no-anchor preprocessed result can be
# reused without listing the formerly missing file as a dependency.  Make the
# file's presence part of the compile command so adding/removing the anchor
# always invalidates the object cache.
set(_makcm_ota_anchor
    "${CMAKE_CURRENT_LIST_DIR}/../../common/ota_trust_anchor.local.inc")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_makcm_ota_anchor}")
if(EXISTS "${_makcm_ota_anchor}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        MAKCM_OTA_TRUST_ANCHOR_CONFIGURED=1)
endif()

# Keep compiler stack-usage evidence beside objects for release-gate audits.
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
