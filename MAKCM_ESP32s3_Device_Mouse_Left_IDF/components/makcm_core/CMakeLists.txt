idf_component_register(
    SRCS
        "bridge_transport.cpp"
        "controller_profile_registry.cpp"
        "endpoint_router.cpp"
        "metrics.cpp"
        "ota_update_manager.cpp"
        "profile_store.cpp"
        "raw_proxy_driver.cpp"
        "secondary.cpp"
        "stick_runtime.cpp"
        "tinyusb_adapter.cpp"
    INCLUDE_DIRS "include"
    REQUIRES makcm_common app_update nvs_flash driver esp_timer esp_tinyusb tinyusb mbedtls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MAKCM_NATIVE_IDF=1
    MAKCM_TINYUSB_API_021=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
