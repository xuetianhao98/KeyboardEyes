cmake_minimum_required(VERSION 3.16)

# Select Clang before CMake detects the compiler on the first configure.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/clang.cmake"
        CACHE FILEPATH "CMake toolchain file")
endif()

project(KeyboardEyes VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "This project requires the Clang compiler.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-stdlib=libc++")
check_cxx_source_compiles("
    #include <iostream>
    #ifndef _LIBCPP_VERSION
    #error libc++ is required
    #endif
    static_assert(__cplusplus >= 202002L, \"C++20 is required\");
    int main() { std::cout << _LIBCPP_VERSION; }
" HAS_CXX20_LIBCXX)
unset(CMAKE_REQUIRED_FLAGS)
if(NOT HAS_CXX20_LIBCXX)
    message(FATAL_ERROR "C++20 with libc++ is required. Install libc++ and libc++abi for your Clang version.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)

add_executable(hello_world src/main.cpp)
target_link_libraries(hello_world PRIVATE PkgConfig::LIBEVDEV)
target_compile_options(hello_world PRIVATE -stdlib=libc++ -Wall -Wextra -Wpedantic)
target_link_options(hello_world PRIVATE -stdlib=libc++)
