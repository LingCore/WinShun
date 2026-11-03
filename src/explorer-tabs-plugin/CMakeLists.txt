# WinShunExplorerTabs.exe: the winshun.explorer-tabs plugin for the plugin
# library (package/, "资源管理器标签页"; not in the setup), a resident C++
# plugin on the single-header SDK (src/plugins-sdk/winshun.h): new File
# Explorer folder windows become tabs of a window already open. No Qt, the
# C runtime linked in. The rules (Rules) are plain C++ that
# tests/tst_explorertabs.cpp builds in again.
add_executable(WinShunExplorerTabs
    main.cpp
    Engine.cpp Engine.h
    Rules.cpp Rules.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunExplorerTabs.rc
)
target_include_directories(WinShunExplorerTabs PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunExplorerTabs PRIVATE winshun_options ole32 oleaut32 shell32 user32 dwmapi advapi32)
set_target_properties(WinShunExplorerTabs PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
