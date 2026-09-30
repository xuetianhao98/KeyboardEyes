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

add_executable(KeyboardEyes src/main.cpp)
target_link_libraries(KeyboardEyes PRIVATE PkgConfig::LIBEVDEV)
target_compile_options(KeyboardEyes PRIVATE -stdlib=libc++ -Wall -Wextra -Wpedantic)
target_link_options(KeyboardEyes PRIVATE -stdlib=libc++)

find_package(SQLite3 3.24 REQUIRED)

add_library(keyboardeyes_database STATIC src/database.cpp)
target_include_directories(keyboardeyes_database PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(keyboardeyes_database PRIVATE SQLite::SQLite3)
target_compile_options(keyboardeyes_database PUBLIC -stdlib=libc++)
target_compile_options(keyboardeyes_database PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(keyboardeyes_database INTERFACE -stdlib=libc++)
target_link_libraries(KeyboardEyes PRIVATE keyboardeyes_database)

add_library(keyboardeyes_reader STATIC
    src/viewer/database_reader.cpp
    src/viewer/table_printer.cpp)
target_include_directories(keyboardeyes_reader PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/viewer")
target_link_libraries(keyboardeyes_reader PRIVATE SQLite::SQLite3)
target_compile_options(keyboardeyes_reader PUBLIC -stdlib=libc++)
target_compile_options(keyboardeyes_reader PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(keyboardeyes_reader INTERFACE -stdlib=libc++)

add_library(keyboardeyes_terminal STATIC
    src/viewer/keyboard_layout.cpp
    src/viewer/heatmap_renderer.cpp
    src/viewer/terminal_session.cpp)
target_link_libraries(keyboardeyes_terminal PUBLIC keyboardeyes_reader)
target_compile_options(keyboardeyes_terminal PRIVATE -Wall -Wextra -Wpedantic)

add_executable(KeyboardEyesViewer src/viewer/main.cpp)
target_link_libraries(KeyboardEyesViewer PRIVATE keyboardeyes_terminal)
target_compile_options(KeyboardEyesViewer PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(database_test tests/database_test.cpp)
    target_link_libraries(database_test PRIVATE keyboardeyes_database SQLite::SQLite3)
    target_compile_options(database_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME database_create_table COMMAND database_test create_table)
    add_test(NAME database_query_update COMMAND database_test query_update)
    add_test(NAME database_load_counts COMMAND database_test load_counts)

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME viewer_integration
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/viewer_test.py"
                --viewer $<TARGET_FILE:KeyboardEyesViewer>)
    set_tests_properties(viewer_integration PROPERTIES TIMEOUT 90)

    add_executable(heatmap_test tests/heatmap_test.cpp)
    target_link_libraries(heatmap_test PRIVATE keyboardeyes_terminal)
    target_compile_options(heatmap_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME heatmap_rendering COMMAND heatmap_test)
    add_test(NAME terminal_integration
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/terminal_test.py"
                --viewer $<TARGET_FILE:KeyboardEyesViewer>)
    set_tests_properties(terminal_integration PROPERTIES TIMEOUT 90)
endif()
