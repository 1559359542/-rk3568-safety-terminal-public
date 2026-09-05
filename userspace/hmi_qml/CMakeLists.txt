cmake_minimum_required(VERSION 3.16)

project(rk3568_hmi_qml LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 5.12 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql)

add_executable(rk3568_hmi_qml
    src/main.cpp
    src/runtime_model.cpp
    src/runtime_model.h
    src/h264_video_bridge.cpp
    src/h264_video_bridge.h
    src/history_model.cpp
    src/history_model.h
    qml.qrc
)

target_link_libraries(rk3568_hmi_qml PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Qml
    Qt5::Quick
    Qt5::QuickControls2
    Qt5::Sql
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
target_link_libraries(rk3568_hmi_qml PRIVATE PkgConfig::GSTREAMER)

install(TARGETS rk3568_hmi_qml RUNTIME DESTINATION bin)
