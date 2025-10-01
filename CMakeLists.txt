cmake_minimum_required(VERSION 3.20)
project(MyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(my_app
  src/main.cpp
)

# Show header in Xcode and set include path (optional but nice)
target_sources(my_app
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES include/av1-toolkit.h
)
target_include_directories(my_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Belt-and-suspenders: if Xcode misdetects, force the language on the file
set_source_files_properties(src/main.cpp PROPERTIES LANGUAGE CXX)

# Warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(my_app PRIVATE -Wall -Wextra -Wpedantic)
endif()