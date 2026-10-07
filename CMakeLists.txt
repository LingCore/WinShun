cmake_minimum_required(VERSION 3.21)

project(QuickFind
    VERSION 0.1.0
    DESCRIPTION "Fast file, folder and text-content search launcher for Windows"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "QuickFind targets Windows only (it relies on Win32 APIs).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick)
qt_standard_project_setup(REQUIRES 6.8)

option(QUICKFIND_BUILD_TESTS "Build unit tests" ON)

# Shared compiler settings for every target in the project.
add_library(quickfind_options INTERFACE)
target_compile_definitions(quickfind_options INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    QT_NO_CAST_FROM_ASCII QT_NO_URL_CAST_FROM_STRING
    QUICKFIND_VERSION="${PROJECT_VERSION}"
    QUICKFIND_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    QUICKFIND_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    QUICKFIND_VERSION_PATCH=${PROJECT_VERSION_PATCH})
if(MSVC)
    target_compile_options(quickfind_options INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive-;/Zc:__cplusplus>")
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

# Developer tool: index memory and search timings on the real snapshot.
qt_add_executable(qfbench tools/qfbench.cpp src/app/Settings.cpp)
target_include_directories(qfbench PRIVATE src/app) # Settings: the user's crawl rules for --sync
target_link_libraries(qfbench PRIVATE qfcore psapi)
set_target_properties(qfbench PROPERTIES WIN32_EXECUTABLE OFF)

if(QUICKFIND_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
