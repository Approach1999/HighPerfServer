cmake_minimum_required(VERSION 3.10)
project(HighPerfServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug) # 或者 Release

# 头文件路径
include_directories(include)

# 自动发现 src 下的所有源文件
file(GLOB SRC_FILES "src/*.cpp")

# 生成可执行文件
add_executable(server ${SRC_FILES})

# 链接线程库 (pthread)
target_link_libraries(server pthread)