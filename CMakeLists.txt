cmake_minimum_required(VERSION 3.20)
project(MyProject LANGUAGES CXX)

# Require C++23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(my_app
    src/main.cpp
)

target_include_directories(my_app
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Warnings: clang/gcc vs MSVC
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(my_app PRIVATE -Wall -Wextra -Wpedantic)
elseif (MSVC)
  target_compile_options(my_app PRIVATE /W4 /permissive-)
endif()