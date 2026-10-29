# WinShunBreak.exe: the winshun.break plugin that comes with Win顺 (plugins/
# winshun.break, "休息提醒"), a resident C++ plugin on the single-header SDK
# (src/plugins-sdk/winshun.h): a short break every so often, a long one every
# few, the screen dimmed with a countdown. Direct2D and DirectWrite draw the
# windows; no Qt, the C runtime linked in. The clock (Schedule) is plain
# C++ that tests/tst_break.cpp builds in again.
add_executable(WinShunBreak
    main.cpp
    Core.h
    Schedule.cpp Schedule.h
    Sense.cpp Sense.h
    Ui.cpp Ui.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunBreak.rc
)
target_include_directories(WinShunBreak PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunBreak PRIVATE winshun_options d2d1 dwrite shcore wtsapi32 powrprof shell32 user32 gdi32)
set_target_properties(WinShunBreak PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
