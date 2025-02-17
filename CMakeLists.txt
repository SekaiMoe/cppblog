#if(",${CMAKE_CURRENT_SOURCE_DIR}," STREQUAL ",${CMAKE_CURRENT_BINARY_DIR},")
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR AND NOT MSVC_IDE)
    # try to clean up (does not work)
    #file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "ERROR: In-source builds are not allowed, please use an extra build dir.")
endif()

cmake_minimum_required(VERSION 3.10)

project(cppblog CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE FALSE)

find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
    message(STATUS "Using ccache: ${CCACHE_FOUND}")
else()
    message(STATUS "Ccache not found. Compiling with cache will be disabled.")
endif(CCACHE_FOUND)

add_compile_options("-fstack-clash-protection")
add_compile_options("-fstack-protector-all")

find_package(Git QUIET)
if(GIT_FOUND)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/include/crow/CMakeLists.txt" OR
       NOT EXISTS "${PROJECT_SOURCE_DIR}/include/cmark/CMakeLists.txt")
        message(STATUS "Fecthing submofules")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive --depth=1
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            RESULT_VARIABLE GIT_SUBMOD_RESULT
        )
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update --init --recursive --depth=1 失败，错误代码: ${GIT_SUBMOD_RESULT}")
        endif()
    endif()
endif()

set(CMARK_TESTS OFF CACHE BOOL "Build cmark-gfm tests")
set(CMARK_SHARED OFF CACHE BOOL "Build shared libraries")
set(CMARK_STATIC ON CACHE BOOL "Build static libraries")
set(CMARK_SAMPLES OFF CACHE BOOL "Build sample programs")
add_subdirectory(include/cmark build/cmark)

include_directories(
    ${CMAKE_BINARY_DIR}/build/cmark/src
    ${PROJECT_SOURCE_DIR}/include/cmark/src
    ${PROJECT_SOURCE_DIR}/include/crow/include
    ${PROJECT_SOURCE_DIR}/include/asio/asio/include
    ${PROJECT_SOURCE_DIR}/include/cpptoml/include
    ${PROJECT_SOURCE_DIR}
)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_library(cpptoml INTERFACE)
target_include_directories(cpptoml INTERFACE ${PROJECT_SOURCE_DIR}/include/cpptoml/include)

add_executable(cppblog src/blog.cpp)

target_include_directories(cppblog PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include/cmark/src
    ${PROJECT_SOURCE_DIR}/include/cmark/extensions
)

target_link_libraries(cppblog
    PRIVATE
    libcmark-gfm_static
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ZLIB::ZLIB
)

set(CMAKE_BUILD_TYPE Release)
add_compile_options(-D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_REENTRANT)
add_custom_command(
     TARGET helper
     POST_BUILD
     COMMAND strip helper
     VERBATIM
)
