# WinShunTools.exe: the winshun.tools plugin that comes with Win顺 (plugins/
# winshun.tools), a resident C++ plugin on the single-header SDK
# (src/plugins-sdk/winshun.h): calculator, unit conversion, colours,
# timestamps, UUIDs, Base64 and hashes. No Qt, the C runtime linked in, so
# it starts in milliseconds and runs in the strict sandbox (low integrity).
# The pieces that compute (Calc, Units, Color, Codec) are plain C++ that
# tests/tst_tools.cpp builds in again.
#
# wstools: the plugin itself (Tools.cpp, tools::serve), which the plugin
# host (src/plugin/runner) runs on a thread of its own; WinShunTools.exe
# runs it alone when plugins are kept apart (WINSHUN_PLUGINS_APART=1).
add_library(wstools STATIC
    Tools.cpp Tools.h
    Calc.cpp Calc.h
    Codec.cpp Codec.h
    Color.cpp Color.h
    Units.cpp Units.h
)
target_include_directories(wstools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(wstools PUBLIC winshun_options bcrypt)
set_target_properties(wstools PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(WinShunTools main.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunTools.rc)
target_link_libraries(WinShunTools PRIVATE wstools)
set_target_properties(WinShunTools PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
