cmake_minimum_required(VERSION 3.20)
project(execution_adapters LANGUAGES CXX)

add_library(execution_adapters
    execution_adapters.cpp
    execution_adapters.hpp
)

target_include_directories(execution_adapters
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(execution_adapters
    PUBLIC
        execution_core
)

target_compile_features(execution_adapters PUBLIC cxx_std_20)

target_compile_options(execution_adapters PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)