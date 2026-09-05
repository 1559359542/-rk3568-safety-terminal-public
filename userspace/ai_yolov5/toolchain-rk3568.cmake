# RK3568 Buildroot aarch64 交叉编译工具链。
# 必须由 cmake -DCMAKE_TOOLCHAIN_FILE=... 在 project() 之前加载。
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_BIN /opt/atk-dlrk356x-toolchain/usr/bin)
set(CMAKE_SYSROOT
    /home/lyy/rk3568/SDK/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot)

set(CMAKE_C_COMPILER
    ${TOOLCHAIN_BIN}/aarch64-buildroot-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER
    ${TOOLCHAIN_BIN}/aarch64-buildroot-linux-gnu-g++)

# 库、头文件与 CMake package 必须从同一 Buildroot sysroot 查找。
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)