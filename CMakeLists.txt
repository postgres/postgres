cmake_minimum_required(VERSION 3.6)
project(postgres)


set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

add_custom_target(postgres COMMAND make -C ${postgres_SOURCE_DIR})
add_definitions("-Wall -g")
add_executable(hook)
