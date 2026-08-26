file(GLOB_RECURSE UCM_ASU_STORE_CC_SOURCE_FILES "./cc/*.cc")
add_library(asustore SHARED ${UCM_ASU_STORE_CC_SOURCE_FILES})
target_include_directories(asustore
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/cc
)
target_link_libraries(asustore PUBLIC storeintf asu_client infra_logger PRIVATE trans)
if(TARGET asu_metrics_ucm_adapter)
    target_link_libraries(asustore PRIVATE asu_metrics_ucm_adapter)
    target_compile_definitions(asustore PRIVATE ASU_UCM_METRICS_ADAPTER_ENABLED=1)
endif()
set_target_properties(asustore PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    BUILD_RPATH "$ORIGIN;$ORIGIN/../../transport/kv/asu;$ORIGIN/../../shared/metrics"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../../transport/kv/asu;$ORIGIN/../../shared/metrics"
)

file(RELATIVE_PATH INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
install(TARGETS asustore LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
