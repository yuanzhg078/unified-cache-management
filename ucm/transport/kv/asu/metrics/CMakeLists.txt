add_library(asu_metrics SHARED
    src/metrics.cpp
    src/standalone_metrics_backend.cpp
)

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

option(BUILD_ASU_UCM_METRICS_ADAPTER
       "Build the optional ASU backend that forwards to UC::Metrics" OFF)
if(BUILD_ASU_UCM_METRICS_ADAPTER)
    add_library(asu_metrics_ucm_adapter STATIC src/ucm_metrics_backend.cpp)
    target_include_directories(asu_metrics_ucm_adapter
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(asu_metrics_ucm_adapter PUBLIC asu_metrics PRIVATE metrics)
endif()

install(TARGETS asu_metrics
        LIBRARY DESTINATION ucm/transport/kv/asu
        COMPONENT ucm)
install(TARGETS asu_metrics
        LIBRARY DESTINATION ucm/store/asu
        COMPONENT ucm)
