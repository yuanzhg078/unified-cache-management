if(CUDA_ROOT AND NOT CUDAToolkit_ROOT)
    set(CUDAToolkit_ROOT "${CUDA_ROOT}")
endif()
find_package(CUDAToolkit REQUIRED)

add_library(kv_buffer_runtime STATIC
    cuda_buffer.cc
    cuda_device.cc
    cuda_trans.cc
)
target_link_libraries(kv_buffer_runtime PRIVATE CUDA::cudart)
target_include_directories(kv_buffer_runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
)
