find_program(CLANG_CXX_COMPILER NAMES clang++-23 clang++)
if(NOT CLANG_CXX_COMPILER)
    message(FATAL_ERROR "Clang not found. Install clang++ or clang++-23 and add it to PATH.")
endif()
set(CMAKE_CXX_COMPILER "${CLANG_CXX_COMPILER}" CACHE FILEPATH "Clang C++ compiler")

# Apply libc++ to both compilation and linking, including compiler checks.
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++")
