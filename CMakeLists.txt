cmake_minimum_required(VERSION 3.14)

project(c-coroutine LANGUAGES C)

set(C_STANDARD 89)

include(CMakeDependentOption)
cmake_dependent_option(CO_BUILD_TESTS
  "Build the unit tests when BUILD_TESTING is enabled and we are the root project" ON
  "BUILD_TESTING;CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR" OFF)

message("Generated with config types: ${CMAKE_CONFIGURATION_TYPES}")

set(BUILD_DIR ${CMAKE_SOURCE_DIR}/build)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BUILD_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${BUILD_DIR}/lib)

file(GLOB lib_files
    ${CMAKE_SOURCE_DIR}/coroutine.h
    ${CMAKE_SOURCE_DIR}/coroutine.c
    ${CMAKE_SOURCE_DIR}/channel.c
)

add_library(coroutine STATIC ${lib_files})

if(UNIX)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -D CO_DEBUG ")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -fomit-frame-pointer ")
    find_package(Threads)
    target_link_libraries(coroutine ${CMAKE_THREAD_LIBS_INIT} )
endif()

if(WIN32)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /D CO_DEBUG")
    add_subdirectory(staging/pthreads4w)
    target_link_libraries(coroutine PUBLIC pthreadVC3)
endif()

if(CO_BUILD_TESTS)
    set(TARGET_LIST test_args test_timing example test_resume)
    foreach (TARGET ${TARGET_LIST})
        add_executable(${TARGET} examples/${TARGET}.c)
        target_link_libraries(${TARGET} coroutine)
    endforeach()
endif()