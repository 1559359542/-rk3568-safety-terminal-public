cmake_minimum_required(VERSION 3.10)

project(rk3568_yolov5_single_image LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(Threads REQUIRED)

find_library(RKNNRT_LIB rknnrt REQUIRED)
find_library(RGA_LIB rga REQUIRED)
find_library(MPP_LIB rockchip_mpp REQUIRED)

add_executable(rk3568_yolov5_demo
    src/main.cc
    src/postprocess.cc
)
target_include_directories(rk3568_yolov5_demo PRIVATE
    ${CMAKE_SYSROOT}/usr/include
    ${CMAKE_SYSROOT}/usr/include/rknn
    ${CMAKE_SYSROOT}/usr/include/rga
    ${CMAKE_SYSROOT}/usr/include/rockchip
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(rk3568_yolov5_demo PRIVATE
    ${OpenCV_LIBS}
    ${RKNNRT_LIB}
    ${RGA_LIB}
    ${MPP_LIB}
    Threads::Threads
)

install(TARGETS rk3568_yolov5_demo RUNTIME DESTINATION bin)
