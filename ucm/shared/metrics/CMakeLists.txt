file(GLOB_RECURSE UCMMETRICS_CC_SOURCE_FILES "./cc/*.cc")
add_library(metrics SHARED ${UCMMETRICS_CC_SOURCE_FILES})
target_include_directories(metrics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/cc/api
    ${CMAKE_CURRENT_SOURCE_DIR}/cc/domain
)
set_target_properties(metrics PROPERTIES
    OUTPUT_NAME ucm_metrics
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    INSTALL_RPATH "$ORIGIN"
)

file(GLOB_RECURSE UCMMETRICS_CPY_SOURCE_FILES CONFIGURE_DEPENDS "./cpy/*.cc")
pybind11_add_module(ucmmetrics ${UCMMETRICS_CPY_SOURCE_FILES})
target_link_libraries(ucmmetrics PRIVATE metrics)
set_target_properties(ucmmetrics PROPERTIES INSTALL_RPATH "$ORIGIN")

file(RELATIVE_PATH INSTALL_REL_PATH
     ${UCM_ROOT_DIR}
     ${CMAKE_CURRENT_SOURCE_DIR}
)
install(TARGETS metrics ucmmetrics
        LIBRARY DESTINATION ${INSTALL_REL_PATH}
        COMPONENT ucm)
