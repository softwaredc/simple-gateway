# ============================================================================
# simple-gateway C++ 版构建脚本
# ----------------------------------------------------------------------------
# 为什么用 CMake？
#   CMake 是 C++ 生态里事实标准的构建系统，跨平台、不依赖 IDE。
#   本项目要在 WSL Ubuntu-24.04 上 cmake + make 一把过，CMakeLists.txt
#   就是那个"构建说明书"。
#
# 工程结构：
#   include/gateway/*.hpp —— 对外暴露的头文件
#   src/*.cpp             —— 库的实现文件（编译进 libgwlib.a）
#   examples/*.cpp        —— 两个可执行程序：echo-server / gateway
#
# 编译选项：
#   C++17：用 std::variant、std::optional、std::thread 等现代特性
#   -Wall -Wextra -Wpedantic：警告全开，教学代码零警告
#   -pthread：用 std::thread 必须链接 pthread
#
# 为什么静态库叫 gwlib 而不是 gateway？
#   因为我们也有一个叫 gateway 的可执行程序（网关主程序）。
#   CMake 不允许同名 target，所以库用 gwlib，可执行用 gateway。
# ============================================================================

cmake_minimum_required(VERSION 3.16)
project(simple-gateway-cpp LANGUAGES CXX)

# ----------------------------------------------------------------------------
# 语言标准
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # 严格 ISO，不用 -std=gnu++17

# ----------------------------------------------------------------------------
# 全局编译/链接选项
# ----------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options(-pthread)
add_link_options(-pthread)

# ----------------------------------------------------------------------------
# 静态库 gwlib
#   把 src/ 下所有 .cpp 编译合并成 libgwlib.a
# ----------------------------------------------------------------------------
file(GLOB_RECURSE GWLIB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(gwlib STATIC ${GWLIB_SOURCES})

# PUBLIC 头文件路径：依赖 gwlib 的其他 target 自动继承
target_include_directories(gwlib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# ----------------------------------------------------------------------------
# 可执行程序 echo-server（示例后端）
# ----------------------------------------------------------------------------
add_executable(echo-server
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/server_main.cpp"
)
target_link_libraries(echo-server PRIVATE gwlib)

# ----------------------------------------------------------------------------
# 可执行程序 gateway（网关主程序）
# ----------------------------------------------------------------------------
add_executable(gateway
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/gateway_main.cpp"
)
target_link_libraries(gateway PRIVATE gwlib)

# ----------------------------------------------------------------------------
# AddressSanitizer（默认关，调试时 cmake -DENABLE_SANITIZER=ON ..）
# ----------------------------------------------------------------------------
option(ENABLE_SANITIZER "Build with ASan + UBSan" OFF)
if(ENABLE_SANITIZER)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()
