cmake_minimum_required(VERSION 3.16)

project(rk3568_hmi_qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)

find_package(Qt5 5.12 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0>=1.20.0
    gstreamer-app-1.0>=1.20.0
    gstreamer-video-1.0>=1.20.0
)

add_executable(rk3568_hmi
    src/main.cpp
    src/monitor_window.cpp
    src/monitor_window.h
)

target_link_libraries(rk3568_hmi PRIVATE Qt5::Widgets PkgConfig::GSTREAMER)

install(TARGETS rk3568_hmi RUNTIME DESTINATION bin)
