cmake_minimum_required(VERSION 3.21)

project(WinShun
    VERSION 0.2.2
    DESCRIPTION "Fast file, folder and text-content search launcher for Windows"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "WinShun targets Windows only (it relies on Win32 APIs).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick LinguistTools)
qt_standard_project_setup(REQUIRES 6.8)

option(WINSHUN_BUILD_TESTS "Build unit tests" ON)
option(WINSHUN_ASAN "Build with AddressSanitizer (MSVC), for the tests" OFF)

# Shared compiler settings for every target in the project.
add_library(winshun_options INTERFACE)
target_compile_definitions(winshun_options INTERFACE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
    QT_NO_CAST_FROM_ASCII QT_NO_URL_CAST_FROM_STRING
    WINSHUN_VERSION="${PROJECT_VERSION}"
    WINSHUN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    WINSHUN_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    WINSHUN_VERSION_PATCH=${PROJECT_VERSION_PATCH})
if(MSVC)
    target_compile_options(winshun_options INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:/utf-8;/W4;/permissive-;/Zc:__cplusplus>")
    if(WINSHUN_ASAN)
        # The parsers read on-disk data (MFT records, journal records,
        # snapshots): run the tests with every out-of-bounds read caught.
        target_compile_options(winshun_options INTERFACE /fsanitize=address /Zi)
        target_link_options(winshun_options INTERFACE /INCREMENTAL:NO /DEBUG)
    endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

# Developer tool: index memory and search timings on the real snapshot.
qt_add_executable(wsbench tools/wsbench.cpp src/app/Settings.cpp)
target_include_directories(wsbench PRIVATE src/app) # Settings: the user's crawl rules for --sync
target_link_libraries(wsbench PRIVATE wscore psapi)
set_target_properties(wsbench PROPERTIES WIN32_EXECUTABLE OFF)

if(WINSHUN_ASAN AND MSVC)
    # Fuzzing the parsers (tools/wsfuzz.cpp) with libFuzzer. They are built
    # into it again, instrumented for coverage, which steers the fuzzing.
    add_executable(wsfuzz tools/wsfuzz.cpp
        src/core/ContentIndex.cpp src/core/ContentScanner.cpp src/core/FileIndex.cpp src/core/Ntfs.cpp
        src/core/Pinyin.cpp src/core/PinyinTable.cpp src/core/TextUtil.cpp src/core/Win32Util.cpp src/core/Wtf8.cpp)
    target_include_directories(wsfuzz PRIVATE src/core)
    target_compile_options(wsfuzz PRIVATE /fsanitize=fuzzer)
    target_link_libraries(wsfuzz PRIVATE Qt6::Core winshun_options)
endif()

if(WINSHUN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
