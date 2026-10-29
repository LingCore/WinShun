# winshun.capture (GitHub #138): the screenshot and screen recording, a
# plugin of the plugin library rather than a part of WinShun.exe.
# WinShunCapture.exe is a starter without Qt (stub/main.cpp): it finds Win顺's
# Qt where Win顺 is installed (WS_APP_DIR) and loads WinShunCaptureApp.dll,
# the plugin itself, built on that Qt. The build stages both, with the
# package's files, into library-plugins/winshun.capture (top CMakeLists.txt).

add_executable(WinShunCapture stub/main.cpp)
target_link_libraries(WinShunCapture PRIVATE winshun_options)
set_target_properties(WinShunCapture PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

qt_add_library(WinShunCaptureApp SHARED Probe.cpp)
target_link_libraries(WinShunCaptureApp PRIVATE wscapture Qt6::Gui Qt6::Qml Qt6::Quick)
