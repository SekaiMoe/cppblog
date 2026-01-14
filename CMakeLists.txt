cmake_minimum_required(VERSION 3.25)

# Prevent in-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR AND NOT MSVC_IDE)
    message(FATAL_ERROR "In-source builds are not allowed. Please create a separate build directory.")
endif()

project(cppblog LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# Enable colored diagnostics if supported
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_COLOR_DIAGNOSTICS ON)
endif()

# Default build type for single-config generators
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build: None Debug Release RelWithDebInfo MinSizeRel Coverage."
        FORCE)
endif()

# ccache support
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
else()
    message(STATUS "ccache not found; compilation caching disabled.")
endif()

# Fetch submodules if needed
find_package(Git QUIET)
if(GIT_FOUND)
    # Check if key submodule files exist (more robust than just CMakeLists.txt)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/include/crow/include/crow.h" OR
       NOT EXISTS "${PROJECT_SOURCE_DIR}/include/cmark/src/cmark.h")
        message(STATUS "Fetching submodules...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive --depth=1
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            RESULT_VARIABLE GIT_SUBMOD_RESULT
            ERROR_VARIABLE GIT_SUBMOD_ERROR
            OUTPUT_QUIET
        )
        if(NOT GIT_SUBMOD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to fetch submodules:\n${GIT_SUBMOD_ERROR}")
        endif()
    endif()
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# Configure cmark-gfm
set(CMARK_TESTS OFF CACHE BOOL "" FORCE)
set(CMARK_SHARED OFF CACHE BOOL "" FORCE)
set(CMARK_STATIC ON CACHE BOOL "" FORCE)
set(CMARK_SAMPLES OFF CACHE BOOL "" FORCE)
set(CMARK_GFM_EXTENSIONS ON CACHE BOOL "" FORCE)

add_subdirectory(include/cmark build/cmark)

# Create cpptoml interface library
add_library(cpptoml INTERFACE)
target_include_directories(cpptoml INTERFACE ${PROJECT_SOURCE_DIR}/include/cpptoml/include)

# Main executable
add_executable(cppblog src/blog.cpp)

# Set compile options
target_compile_options(cppblog PRIVATE
    -fstack-clash-protection
    -fstack-protector-all
    -D_FILE_OFFSET_BITS=64
    -D_LARGEFILE_SOURCE
    -D_REENTRANT
)

target_include_directories(cppblog PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/cmark/src
    ${PROJECT_SOURCE_DIR}/include/cmark/extensions
)

# Link libraries using modern CMake (no global include_directories!)
target_link_libraries(cppblog PRIVATE
    libcmark-gfm-extensions_static
    libcmark-gfm_static
    cpptoml
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

# Ensure cmark extesions are built before main target (optional but safe)
# Note: add_dependencies is rarely needed if you link properly, but kept for clarity
add_dependencies(cppblog libcmark-gfm-extensions_static)

# Strip binary in Release builds (only on Unix-like systems)
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT WIN32 AND NOT APPLE)
    find_program(STRIP_PROGRAM strip)
    if(STRIP_PROGRAM)
        add_custom_command(TARGET cppblog POST_BUILD
            COMMAND ${STRIP_PROGRAM} $<TARGET_FILE:cppblog>
            COMMENT "Stripping debug symbols from cppblog"
        )
    endif()
endif()
