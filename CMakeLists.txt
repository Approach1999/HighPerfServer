cmake_minimum_required(VERSION 3.10)
project(HighPerfServer)

set(CMAKE_CXX_STANDARD 17)

# 1. 自动查找 PostgreSQL 包
find_package(PostgreSQL REQUIRED)

# 2. 包含头文件路径
# include: 是我们自己的头文件目录
# ${PostgreSQL_INCLUDE_DIRS}: 是 PG 的头文件目录 (/usr/include/postgresql)
include_directories(include ${PostgreSQL_INCLUDE_DIRS})

# 3. 自动发现 src 下的所有源文件
file(GLOB SRC_FILES "src/*.cpp")

# 4. 生成可执行文件
add_executable(server ${SRC_FILES})

# 5. 【修改】链接库文件
# pthread: 线程库
# ${PostgreSQL_LIBRARIES}: PG 数据库库 (libpq)
target_link_libraries(server pthread ${PostgreSQL_LIBRARIES})