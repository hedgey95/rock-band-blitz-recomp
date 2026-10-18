cmake_minimum_required(VERSION 3.25)
project(extract_package CXX)
set(CMAKE_CXX_STANDARD 23)
find_package(rexglue REQUIRED CONFIG)
add_executable(extract_package main.cpp)
target_link_libraries(extract_package PRIVATE rex::runtime)
add_custom_command(TARGET extract_package POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:extract_package> $<TARGET_FILE_DIR:extract_package>
    COMMAND_EXPAND_LISTS)
