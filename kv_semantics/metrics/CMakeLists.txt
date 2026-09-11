add_library(kv_metrics SHARED src/metrics.cc)

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
