find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(kv_metrics_codegen
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_kv_metrics.py
            --header-only
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating KV metric IDs and standalone default descriptors"
    VERBATIM
)

# UCM uses the generated Python default and UCM YAML as its native registration
# source. Keep those artifacts synchronized without making standalone link UCM.
add_custom_target(kv_metrics_config_check
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_kv_metrics.py
            --check
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking generated KV metric registration defaults"
    VERBATIM
)

# The UCM adapter consumes the native UCM YAML/Python registration defaults as
# well as the generated C++ names. Keep all four artifacts synchronized when
# the UCM integration is built.
add_custom_target(kv_metrics_ucm_codegen
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_kv_metrics.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating KV metric definitions and UCM registration defaults"
    VERBATIM
)
add_dependencies(kv_metrics_ucm_codegen kv_metrics_codegen)

add_library(kv_metrics SHARED src/metrics.cc)
add_dependencies(kv_metrics kv_metrics_codegen)

target_include_directories(kv_metrics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(kv_metrics PRIVATE pthread)
set_target_properties(kv_metrics PROPERTIES
    OUTPUT_NAME kv_metrics
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    INSTALL_RPATH "$ORIGIN"
)

add_library(kv_metrics_standalone STATIC src/standalone_metrics_backend.cc)
add_dependencies(kv_metrics_standalone kv_metrics_codegen)
target_include_directories(kv_metrics_standalone
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(kv_metrics_standalone PUBLIC kv_metrics pthread)

install(TARGETS kv_metrics
        LIBRARY DESTINATION kv_semantics
        COMPONENT ucm)
install(TARGETS kv_metrics
        LIBRARY DESTINATION ucm/store/asu
        COMPONENT ucm)
install(FILES config/kv_metrics.yaml
        DESTINATION kv_semantics/metrics
        COMPONENT ucm)
