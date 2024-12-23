set(srcs
    "src/AceButton.cpp")

idf_component_register(SRCS "${srcs}"
                    REQUIRES "esp_driver_gpio esp_timer"
                    INCLUDE_DIRS "src/include")
idf_build_set_property(COMPILE_OPTIONS "-DESP32" APPEND)
set_target_properties(${TARGET} PROPERTIES LINKER_LANGUAGE CXX)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--undefined=uxTopUsedPriority")