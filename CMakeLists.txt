cmake_minimum_required(VERSION 3.21)

project(WinShun
    VERSION 0.41.0
    DESCRIPTION "Fast file, folder and text-content search launcher for Windows"
    LANGUAGES C CXX) # C: miniz

if(NOT WIN32)
    message(FATAL_ERROR "WinShun targets Windows only (it relies on Win32 APIs).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick Sql Svg Network LinguistTools)
qt_standard_project_setup(REQUIRES 6.8)

option(WINSHUN_BUILD_TESTS "Build unit tests" ON)
option(WINSHUN_ASAN "Build with AddressSanitizer (MSVC), for the tests" OFF)

include(cmake/Download.cmake)
winshun_configure_mirrors("${CMAKE_BINARY_DIR}/generated/GitHubMirrors.h")

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

# zstd (BSD, third_party/zstd: lib/ of the 1.5.7 release, unchanged).
# Win顺 applies delta updates (src/app/UpdateDelta) with the decompressor
# alone; the compressor goes only into tools/delta and the tests.
file(GLOB zstd_common_sources ${PROJECT_SOURCE_DIR}/third_party/zstd/common/*.c)
file(GLOB zstd_decompress_sources ${PROJECT_SOURCE_DIR}/third_party/zstd/decompress/*.c)
file(GLOB zstd_compress_sources ${PROJECT_SOURCE_DIR}/third_party/zstd/compress/*.c)
add_library(zstd_decompress STATIC ${zstd_common_sources} ${zstd_decompress_sources})
add_library(zstd_full STATIC ${zstd_common_sources} ${zstd_decompress_sources} ${zstd_compress_sources})
foreach(zstd_target zstd_decompress zstd_full)
    target_include_directories(${zstd_target} PUBLIC ${PROJECT_SOURCE_DIR}/third_party/zstd)
    # No assembly (the Huffman decoder's is for GCC/Clang), no threads.
    target_compile_definitions(${zstd_target} PRIVATE ZSTD_DISABLE_ASM)
endforeach()

add_subdirectory(src/extract)
add_subdirectory(src/core)
add_subdirectory(src/mcp)
add_subdirectory(src/plugin)
add_subdirectory(src/cli) # ws.exe; sets WS_CLI_SOURCES for tst_cli
add_subdirectory(src/tools-plugin)
add_subdirectory(src/semantic-plugin)
add_subdirectory(src/localsend-plugin)
add_subdirectory(src/break-plugin)
add_subdirectory(src/explorer-tabs-plugin)
add_subdirectory(src/gestures-plugin)
add_subdirectory(src/expander-plugin)
add_subdirectory(src/switches-plugin)
add_subdirectory(src/bookmarks-plugin)
add_subdirectory(src/translate-plugin)
add_subdirectory(src/cloudbackup-plugin)
add_subdirectory(src/capture)
add_subdirectory(src/capture-plugin)
add_subdirectory(src/ocr-search-plugin)
add_subdirectory(src/devtools-plugin)
add_subdirectory(src/app)
add_subdirectory(src/stub)
add_subdirectory(src/service)
add_subdirectory(src/updater)

# The plugins that come with Win顺 (plugins/, docs/plugins.md), staged next
# to the build's WinShun.exe the way the installer lays them out: each
# folder as it is in the source tree, the SDK file it uses (src/plugins-sdk)
# copied in, WinShunTools.exe into winshun.tools. The app reads <exe dir>\
# plugins as the bundled folder; wsbench --plugin and the tests use these
# staged copies; build.ps1 -Deploy takes the folder into dist\.
set(bundled_plugins_dir "${CMAKE_BINARY_DIR}/plugins")
add_custom_target(bundled_plugins ALL
    # Emptied first: a plugin no longer bundled must not linger in an old build folder (GitHub #138).
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${bundled_plugins_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/plugins" "${bundled_plugins_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/src/plugins-sdk/WinShun.psm1" "${bundled_plugins_dir}/winshun.kill/WinShun.psm1"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunTools>" "${bundled_plugins_dir}/winshun.tools/WinShunTools.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/switches-plugin/package" "${bundled_plugins_dir}/winshun.switches"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunSwitches>" "${bundled_plugins_dir}/winshun.switches/WinShunSwitches.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunSwitchesApp>" "${bundled_plugins_dir}/winshun.switches/WinShunSwitchesApp.dll"
    DEPENDS WinShunTools WinShunSwitches WinShunSwitchesApp
    COMMENT "Staging the bundled plugins into ${bundled_plugins_dir}"
    VERBATIM)
install(DIRECTORY "${bundled_plugins_dir}/" DESTINATION plugins)

# The official plugins that do not come with Win顺 (GitHub #138): installed
# from the plugin library (设置 → 插件 → 插件库) by who wants them. Each is
# staged as its package will hold it -- the files of src/<name>/package and
# its program -- in library-plugins/<id>; release.ps1 zips each folder into
# <id>-<version>.wsplugin for the library, wsbench --semantic reads them there.
set(library_plugins_dir "${CMAKE_BINARY_DIR}/library-plugins")
file(READ "${PROJECT_SOURCE_DIR}/src/web-plugin/package/plugin.json" web_plugin_manifest)
string(JSON web_plugin_manifest SET "${web_plugin_manifest}" minWinShun "\"${PROJECT_VERSION}\"")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/generated/web-plugin.json" CONTENT "${web_plugin_manifest}\n")
# Content providers (commands[].contentSearch) are not in 0.17.0: like
# winshun.web, the OCR plugin needs at least the version it is built with.
set(ocr_plugin_source "${PROJECT_SOURCE_DIR}/src/ocr-search-plugin/package/plugin.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ocr_plugin_source}")
file(READ "${ocr_plugin_source}" ocr_plugin_manifest)
string(JSON ocr_plugin_manifest SET "${ocr_plugin_manifest}" minWinShun "\"${PROJECT_VERSION}\"")
set(OCR_PLUGIN_MANIFEST "${CMAKE_BINARY_DIR}/generated/ocr-search-plugin.json")
file(GENERATE OUTPUT "${OCR_PLUGIN_MANIFEST}" CONTENT "${ocr_plugin_manifest}\n")
# The permission `elevated` is not in 0.18.0 (a host that does not know it
# turns the whole plugin down): Explorer tabs, too, needs at least the version
# it is built with; the release bumps that version before building it.
set(tabs_plugin_source "${PROJECT_SOURCE_DIR}/src/explorer-tabs-plugin/package/plugin.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${tabs_plugin_source}")
file(READ "${tabs_plugin_source}" tabs_plugin_manifest)
string(JSON tabs_plugin_manifest SET "${tabs_plugin_manifest}" minWinShun "\"${PROJECT_VERSION}\"")
set(TABS_PLUGIN_MANIFEST "${CMAKE_BINARY_DIR}/generated/explorer-tabs-plugin.json")
file(GENERATE OUTPUT "${TABS_PLUGIN_MANIFEST}" CONTENT "${tabs_plugin_manifest}\n")
# The developer tools' window is Qt Quick on Win顺's own Qt: built against
# this version's, it needs at least this version of Win顺.
set(devtools_plugin_source "${PROJECT_SOURCE_DIR}/src/devtools-plugin/package/plugin.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${devtools_plugin_source}")
file(READ "${devtools_plugin_source}" devtools_plugin_manifest)
string(JSON devtools_plugin_manifest SET "${devtools_plugin_manifest}" minWinShun "\"${PROJECT_VERSION}\"")
set(DEVTOOLS_PLUGIN_MANIFEST "${CMAKE_BINARY_DIR}/generated/devtools-plugin.json")
file(GENERATE OUTPUT "${DEVTOOLS_PLUGIN_MANIFEST}" CONTENT "${devtools_plugin_manifest}\n")
add_custom_target(library_plugins ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/web-plugin/package" "${library_plugins_dir}/winshun.web"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/generated/web-plugin.json" "${library_plugins_dir}/winshun.web/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/ocr-search-plugin/package" "${library_plugins_dir}/winshun.ocr-search"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OCR_PLUGIN_MANIFEST}" "${library_plugins_dir}/winshun.ocr-search/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunOcrSearch>" "${library_plugins_dir}/winshun.ocr-search/WinShunOcrSearch.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunOcrSearchApp>" "${library_plugins_dir}/winshun.ocr-search/WinShunOcrSearchApp.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/miniz/LICENSE" "${library_plugins_dir}/winshun.ocr-search/licenses/miniz.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/opencc/LICENSE" "${library_plugins_dir}/winshun.ocr-search/licenses/opencc.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/devtools-plugin/package" "${library_plugins_dir}/winshun.devtools"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DEVTOOLS_PLUGIN_MANIFEST}" "${library_plugins_dir}/winshun.devtools/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunDevTools>" "${library_plugins_dir}/winshun.devtools/WinShunDevTools.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunDevToolsApp>" "${library_plugins_dir}/winshun.devtools/WinShunDevToolsApp.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/semantic-plugin/package" "${library_plugins_dir}/winshun.semantic"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunSemantic>" "${library_plugins_dir}/winshun.semantic/WinShunSemantic.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/localsend-plugin/package" "${library_plugins_dir}/winshun.localsend"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunLocalSend>" "${library_plugins_dir}/winshun.localsend/WinShunLocalSend.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/break-plugin/package" "${library_plugins_dir}/winshun.break"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunBreak>" "${library_plugins_dir}/winshun.break/WinShunBreak.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/explorer-tabs-plugin/package" "${library_plugins_dir}/winshun.explorer-tabs"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${TABS_PLUGIN_MANIFEST}" "${library_plugins_dir}/winshun.explorer-tabs/plugin.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunExplorerTabs>" "${library_plugins_dir}/winshun.explorer-tabs/WinShunExplorerTabs.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/gestures-plugin/package" "${library_plugins_dir}/winshun.gestures"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunGestures>" "${library_plugins_dir}/winshun.gestures/WinShunGestures.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunGesturesEditor>" "${library_plugins_dir}/winshun.gestures/WinShunGesturesEditor.dll"
    # The Phosphor icons embedded in WinShunGestures.exe (icons/PhosphorGestures.ttf, a subset).
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/phosphor/LICENSE" "${library_plugins_dir}/winshun.gestures/licenses/phosphor.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/expander-plugin/package" "${library_plugins_dir}/winshun.expander"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunExpander>" "${library_plugins_dir}/winshun.expander/WinShunExpander.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/bookmarks-plugin/package" "${library_plugins_dir}/winshun.bookmarks"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunBookmarks>" "${library_plugins_dir}/winshun.bookmarks/WinShunBookmarks.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/translate-plugin/package" "${library_plugins_dir}/winshun.translate"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunTranslate>" "${library_plugins_dir}/winshun.translate/WinShunTranslate.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/cloudbackup-plugin/package" "${library_plugins_dir}/winshun.cloudbackup"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunCloudBackup>" "${library_plugins_dir}/winshun.cloudbackup/WinShunCloudBackup.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/src/capture-plugin/package" "${library_plugins_dir}/winshun.capture"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunCapture>" "${library_plugins_dir}/winshun.capture/WinShunCapture.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShunCaptureApp>" "${library_plugins_dir}/winshun.capture/WinShunCaptureApp.dll"
    # What is compiled into WinShunCaptureApp.dll; Qt, ONNX Runtime and the OCR models are Win顺's.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/zxing-cpp/LICENSE" "${library_plugins_dir}/winshun.capture/licenses/zxing-cpp.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/third_party/miniz/LICENSE" "${library_plugins_dir}/winshun.capture/licenses/miniz.txt"
    DEPENDS WinShunSemantic WinShunLocalSend WinShunBreak WinShunExplorerTabs WinShunGestures WinShunGesturesEditor WinShunExpander WinShunBookmarks WinShunTranslate WinShunCloudBackup WinShunCapture WinShunCaptureApp WinShunOcrSearch WinShunOcrSearchApp
        WinShunDevTools WinShunDevToolsApp
    COMMENT "Staging the library plugins into ${library_plugins_dir}"
    VERBATIM)

# Developer tool: index memory and search timings on the real snapshot.
qt_add_executable(wsbench tools/wsbench.cpp src/app/Settings.cpp
    src/app/SiteIconImage.cpp src/app/SvgIcon.cpp src/app/platform/Http.cpp src/app/platform/SiteIcons.cpp # --site-icon, --suggest
    src/app/platform/RegistryWatch.cpp src/app/platform/SettingBroadcast.cpp src/switches-plugin/platform/SwitchAudio.cpp
    src/switches-plugin/platform/SwitchExplorer.cpp src/switches-plugin/platform/SwitchKeepAwake.cpp src/switches-plugin/platform/SwitchRadios.cpp
    src/switches-plugin/platform/SwitchTheme.cpp src/switches-plugin/platform/SystemSwitches.cpp # --switches
    src/app/platform/PaddleOcr.cpp src/app/platform/OcrFiles.cpp) # --ocr
target_include_directories(wsbench PRIVATE src/app src/app/platform ${PROJECT_SOURCE_DIR}/src/switches-plugin) # Settings: the user's crawl rules for --sync
target_link_libraries(wsbench PRIVATE wscore wsextract wsmcp wsplugin wscapture Qt6::Gui Qt6::Svg psapi winhttp advapi32 onnxruntime_headers
    windowsapp iphlpapi) # wsmcp: --mcp-host; wsplugin: --plugin; wscapture: --scroll-replay; advapi32: --open-probe; windowsapp, iphlpapi: --switches (the radios)
add_dependencies(wsbench WinShunPluginHost bundled_plugins library_plugins) # --plugin starts it from next to wsbench; the bundled and library plugins next to it too
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

# Memory report a user runs on their own machine (tools/memreport/README.md).
# One exe with the C runtime and miniz built in, no Qt: it runs beside any
# installed Win顺 version and is sent on its own, not installed.
add_executable(WinShunMemReport tools/memreport/memreport.cpp ${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c
    ${PROJECT_SOURCE_DIR}/resources/WinShunMemReport.rc)
target_include_directories(WinShunMemReport PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(WinShunMemReport PRIVATE MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
target_link_libraries(WinShunMemReport PRIVATE winshun_options advapi32 ole32 pdh psapi shell32 user32 version wevtapi)
set_target_properties(WinShunMemReport PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# What reading the clipboard history's pictures does on a user's PC
# (tools/ocrtest/README.md): their pictures and OCR component, each reading
# watched from outside. Qt beside it, deployed by scripts/ocrtest.ps1; its
# own symbols kept, for the dumps it brings back.
qt_add_executable(WinShunOcrTest tools/ocrtest/ocrtest.cpp
    src/app/platform/PaddleOcr.cpp src/app/platform/OcrFiles.cpp
    src/app/platform/CrashRecord.cpp src/app/platform/CodeAddress.cpp)
target_include_directories(WinShunOcrTest PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/app/platform)
# kernel32 and user32 ahead of wscapture's windowsapp umbrella: the console
# calls come from kernel32 itself, not API sets an older Windows 10 may lack.
target_link_libraries(WinShunOcrTest PRIVATE kernel32 user32 winshun_options wscapture miniz Qt6::Gui Qt6::Sql
    onnxruntime_headers dbghelp psapi shell32 ole32 version advapi32)
target_compile_options(WinShunOcrTest PRIVATE /Z7)
target_link_options(WinShunOcrTest PRIVATE /DEBUG /OPT:REF /OPT:ICF)
set_target_properties(WinShunOcrTest PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
qt_add_resources(WinShunOcrTest "ocrtest" PREFIX /ocrtest BASE ${PROJECT_SOURCE_DIR}
    FILES resources/ocr/package.json docs/images/clipboard.jpg)

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

# Delta update packages (src/app/UpdateDelta.h), made and checked by
# scripts/release.ps1: WinShunDelta make / apply / line. Not deployed.
qt_add_executable(WinShunDelta tools/delta/main.cpp src/app/UpdateDelta.cpp src/app/UpdateDeltaMake.cpp
    src/app/UpdatePackage.cpp src/app/platform/Http.cpp)
target_include_directories(WinShunDelta PRIVATE src/app src/core)
target_link_libraries(WinShunDelta PRIVATE wscore miniz zstd_full version winhttp ws2_32)
set_target_properties(WinShunDelta PROPERTIES WIN32_EXECUTABLE OFF QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

if(WINSHUN_ASAN AND MSVC)
    # Fuzzing the parsers (tools/wsfuzz.cpp) with libFuzzer. They are built
    # into it again, instrumented for coverage, which steers the fuzzing.
    # Document readers too, all but PDF (PDFium is fuzzed upstream).
    add_executable(wsfuzz tools/wsfuzz.cpp
        src/core/ContentIndex.cpp src/core/ContentPattern.cpp src/core/ContentScanner.cpp src/core/FileIndex.cpp src/core/Ntfs.cpp
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
