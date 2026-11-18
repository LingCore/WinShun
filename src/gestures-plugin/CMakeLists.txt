# WinShunGestures.exe: the winshun.gestures plugin for the plugin library
# (package/, "鼠标手势"; not in the setup), a resident C++ plugin on the
# single-header SDK (src/plugins-sdk/winshun.h): hold the right button and
# draw. No Qt, the C runtime linked in. The rules (Gesture) are plain C++
# that tests/tst_gestures.cpp builds in again; Engine hooks the mouse and
# draws the trail with GDI+.
add_executable(WinShunGestures
    main.cpp
    Engine.cpp Engine.h
    Gesture.cpp Gesture.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunGestures.rc
)
target_include_directories(WinShunGestures PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunGestures PRIVATE winshun_options user32 gdi32 gdiplus shcore advapi32)
set_target_properties(WinShunGestures PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
