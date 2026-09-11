file(GLOB_RECURSE UCM_ASU_STORE_CC_SOURCE_FILES "./cc/*.cc")

# The adapter is UCM-specific and is intentionally built here rather than by
# kv_semantics, so the KV client remains independently buildable without UCM.
add_library(kv_metrics_ucm_adapter STATIC
    ${UCM_ROOT_DIR}/kv_semantics/metrics/src/ucm_metrics_backend.cc
)
target_include_directories(kv_metrics_ucm_adapter PUBLIC
    ${UCM_ROOT_DIR}/kv_semantics/metrics/include
)
target_link_libraries(kv_metrics_ucm_adapter PUBLIC kv_metrics PRIVATE metrics)

add_library(asustore SHARED ${UCM_ASU_STORE_CC_SOURCE_FILES})
target_include_directories(asustore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/cc
)
target_link_libraries(asustore PUBLIC storeintf kv_client infra_logger PRIVATE trans)
target_link_libraries(asustore PRIVATE kv_metrics_ucm_adapter)
set_target_properties(asustore PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    BUILD_RPATH "$ORIGIN;$ORIGIN/../../../kv_semantics;$ORIGIN/../../shared/metrics"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../../../kv_semantics;$ORIGIN/../../shared/metrics"
)

file(RELATIVE_PATH INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS asustore LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
