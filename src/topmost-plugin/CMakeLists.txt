# WinShunTopmost.exe: the winshun.topmost plugin for the plugin library
# (package/, "窗口置顶"; not in the setup), a resident C++ plugin on the
# single-header SDK (src/plugins-sdk/winshun.h): a shortcut keeps the window
# in front above the others. No Qt, the C runtime linked in. Which windows
# count and the switch itself (Pins) are plain C++ that tests/tst_topmost.cpp
# builds in again, as it does the line round a window on top (Border); Note
# draws the word said on the window.
add_executable(WinShunTopmost
    main.cpp
    Border.cpp Border.h
    Draw.h
    Note.cpp Note.h
    Pins.cpp Pins.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunTopmost.rc
)
target_include_directories(WinShunTopmost PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunTopmost PRIVATE winshun_options user32 gdi32 dwmapi d2d1 dwrite shcore)
set_target_properties(WinShunTopmost PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
