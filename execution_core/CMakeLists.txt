cmake_minimum_required(VERSION 3.20)
project(execution_core LANGUAGES CXX)

add_library(execution_core
    task.cpp
    scheduler.cpp
    execution_core.hpp
    task.hpp
    scheduler.hpp
    awaiters.hpp
 )

target_include_directories(execution_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(execution_core PUBLIC cxx_std_20)

target_compile_options(execution_core PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
