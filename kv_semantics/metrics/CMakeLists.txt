add_library(asu_metrics SHARED src/metrics.cc)

target_include_directories(asu_metrics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(asu_metrics PRIVATE pthread)
set_target_properties(asu_metrics PROPERTIES
    OUTPUT_NAME asu_metrics
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    INSTALL_RPATH "$ORIGIN"
)

add_library(asu_metrics_standalone STATIC src/standalone_metrics_backend.cc)
target_include_directories(asu_metrics_standalone
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(asu_metrics_standalone PUBLIC asu_metrics pthread)

install(TARGETS asu_metrics
        LIBRARY DESTINATION kv_semantics
        COMPONENT ucm)
install(TARGETS asu_metrics
        LIBRARY DESTINATION ucm/store/asu
        COMPONENT ucm)
