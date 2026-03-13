cmake_minimum_required(VERSION 3.10)
project(HighPerfServer)

set(CMAKE_CXX_STANDARD 17)


find_package(PostgreSQL REQUIRED)


include_directories(include ${PostgreSQL_INCLUDE_DIRS})

file(GLOB SRC_FILES "src/*.cpp")

add_executable(server ${SRC_FILES})

target_link_libraries(server pthread ${PostgreSQL_LIBRARIES})