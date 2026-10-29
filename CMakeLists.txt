cmake_minimum_required(VERSION 3.21)

project(WinShun
    VERSION 0.15.0
    DESCRIPTION "Fast file, folder and text-content search launcher for Windows"
    LANGUAGES C CXX) # C: miniz

if(NOT WIN32)
    message(FATAL_ERROR "WinShun targets Windows only (it relies on Win32 APIs).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick Sql Svg LinguistTools)
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

add_subdirectory(src/extract)
add_subdirectory(src/core)
add_subdirectory(src/mcp)
add_subdirectory(src/plugin)
add_subdirectory(src/tools-plugin)
add_subdirectory(src/semantic-plugin)
add_subdirectory(src/localsend-plugin)
add_subdirectory(src/capture)
add_subdirectory(src/app)
add_subdirectory(src/stub)
add_subdirectory(src/service)
add_subdirectory(src/updater)

# The plugins that come with Win顺 (plugins/, docs/plugins.md), staged next
# to the build's WinShun.exe the way the installer lays them out: each
# folder as it is in the source tree, the SDK file it uses (src/plugins-sdk)
# copied in, WinShunTools.exe into winshun.tools (WinShunSemantic.exe into winshun.semantic,
# WinShunLocalSend.exe into winshun.localsend). The app reads <exe dir>\
# plugins as the bundled folder; wsbench --plugin and the tests use these
# staged copies; build.ps1 -Deploy takes the folder into dist\.
set(bundled_plugins_dir "${CMAKE_BINARY_DIR}/plugins")
add_custom_target(bundled_plugins ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/plugins" "${bundled_plugins_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/src/plugins-sdk/WinShun.psm1" "${bundled_plugins_dir}/winshun.kill/WinShun.psm1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/src/plugins-sdk/winshun.py" "${bundled_plugins_dir}/winshun.bookmarks/winshun.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/src/plugins-sdk/winshun.py" "${bundled_plugins_dir}/winshun.translate/winshun.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunTools>" "${bundled_plugins_dir}/winshun.tools/WinShunTools.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunSemantic>" "${bundled_plugins_dir}/winshun.semantic/WinShunSemantic.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunLocalSend>" "${bundled_plugins_dir}/winshun.localsend/WinShunLocalSend.exe"
    DEPENDS WinShunTools WinShunSemantic WinShunLocalSend
    COMMENT "Staging the bundled plugins into ${bundled_plugins_dir}"
    VERBATIM)
install(DIRECTORY "${bundled_plugins_dir}/" DESTINATION plugins)

# Developer tool: index memory and search timings on the real snapshot.
qt_add_executable(wsbench tools/wsbench.cpp src/app/Settings.cpp
    src/app/SiteIconImage.cpp src/app/SvgIcon.cpp src/app/platform/Http.cpp src/app/platform/SiteIcons.cpp # --site-icon, --suggest
    src/app/platform/RegistryWatch.cpp src/app/platform/SettingBroadcast.cpp src/app/platform/SwitchAudio.cpp
    src/app/platform/SwitchExplorer.cpp src/app/platform/SwitchKeepAwake.cpp src/app/platform/SwitchRadios.cpp
    src/app/platform/SwitchTheme.cpp src/app/platform/SystemSwitches.cpp # --switches
    src/app/platform/PaddleOcr.cpp) # --ocr
target_include_directories(wsbench PRIVATE src/app src/app/platform) # Settings: the user's crawl rules for --sync
target_link_libraries(wsbench PRIVATE wscore wsextract wsmcp wsplugin wscapture Qt6::Gui Qt6::Svg psapi winhttp advapi32 onnxruntime_headers
    windowsapp iphlpapi) # wsmcp: --mcp-host; wsplugin: --plugin; wscapture: --scroll-replay; advapi32: --open-probe; windowsapp, iphlpapi: --switches (the radios)
add_dependencies(wsbench WinShunPluginHost bundled_plugins) # --plugin starts it from next to wsbench; the bundled plugins next to it too
set_target_properties(wsbench PROPERTIES WIN32_EXECUTABLE OFF)

# Plugin system, stage 0 (tools/plugin-probe.cpp): starts itself every way a
# plugin process could be started and reports what each child gets (token,
# job, mitigations, stdio, timings). Plain Win32 with the C runtime linked
# in, no Qt: its own start is what it times. Runs as the invoker (the
# linker's default manifest): its parent role is run elevated and not.
add_executable(plugin-probe tools/plugin-probe.cpp)
target_link_libraries(plugin-probe PRIVATE winshun_options advapi32 userenv user32 mpr)
set_target_properties(plugin-probe PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Screen capture, stage 0 (docs/design-capture.md, section 6). capture-probe
# times BitBlt and Windows.Graphics.Capture (C++/WinRT from the Windows SDK,
# as in platform/TextRecognizer.cpp), reads HDR state, and tries the Start
# menu and PrtScn cases; no Qt, so the timings are the capture paths' own.
# capture-probe-qt measures the overlay and pin windows with WinShun's own
# Qt Quick setup. Run it with Qt's bin folder on PATH.
add_executable(capture-probe tools/capture-probe.cpp)
target_link_libraries(capture-probe PRIVATE winshun_options windowsapp d3d11 dxgi dwmapi windowscodecs user32 gdi32 advapi32 ole32)
set_target_properties(capture-probe PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

qt_add_executable(recording-probe tools/recording-probe.cpp src/app/platform/KeyListener.cpp)
target_include_directories(recording-probe PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(recording-probe PRIVATE wscapture wscore Qt6::Gui shcore)
set_target_properties(recording-probe PROPERTIES WIN32_EXECUTABLE OFF QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
qt_add_executable(capture-probe-qt tools/capture-probe-qt.cpp)
target_link_libraries(capture-probe-qt PRIVATE winshun_options Qt6::Gui Qt6::Qml Qt6::Quick dwmapi dxgi psapi user32 gdi32)
# Neither probe loads QML imports: no scan (it would walk the whole source tree).
set_target_properties(capture-probe-qt PROPERTIES WIN32_EXECUTABLE OFF QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
# Long screenshot, stage 6 (docs/design-capture-roadmap.md, 2.4): leaving our
# own windows out of the capture, posted wheel messages in real programs, and
# the frame rate while a page scrolls. Run it with Qt's bin folder on PATH.
qt_add_executable(scroll-probe tools/scroll-probe.cpp)
target_link_libraries(scroll-probe PRIVATE winshun_options Qt6::Gui Qt6::Quick windowsapp d3d11 dxgi dwmapi user32 gdi32 advapi32)
set_target_properties(scroll-probe PROPERTIES WIN32_EXECUTABLE OFF QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

if(WINSHUN_ASAN AND MSVC)
    # Fuzzing the parsers (tools/wsfuzz.cpp) with libFuzzer. They are built
    # into it again, instrumented for coverage, which steers the fuzzing.
    # Document readers too, all but PDF (PDFium is fuzzed upstream).
    add_executable(wsfuzz tools/wsfuzz.cpp
        src/core/ContentIndex.cpp src/core/ContentScanner.cpp src/core/FileIndex.cpp src/core/Ntfs.cpp
        src/core/Pinyin.cpp src/core/PinyinTable.cpp src/core/TextUtil.cpp src/core/Win32Util.cpp src/core/Wtf8.cpp
        src/extract/Cfb.cpp src/extract/Doc.cpp src/extract/Extract.cpp src/extract/Markup.cpp src/extract/Ooxml.cpp
        src/extract/Ppt.cpp src/extract/Writer.cpp src/extract/Xls.cpp src/extract/Xml.cpp src/extract/Zip.cpp)
    target_include_directories(wsfuzz PRIVATE src/core src/extract)
    target_compile_options(wsfuzz PRIVATE /fsanitize=fuzzer)
    target_link_libraries(wsfuzz PRIVATE Qt6::Core winshun_options miniz bcrypt) # bcrypt: grams::fingerprint
endif()

if(WINSHUN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
