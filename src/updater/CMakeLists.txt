# WinShunUpdate.exe: puts a portable copy's new files in place while Win顺
# is not running, and starts Win顺 again after a Setup that failed (see
# main.cpp; Updater runs it). Plain Win32 with the C runtime linked in and
# no Qt, so that a copy of this one file runs from anywhere.
add_executable(WinShunUpdate WIN32
    main.cpp
    Swap.cpp Swap.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunUpdate.rc)
target_link_libraries(WinShunUpdate PRIVATE winshun_options shell32 user32)
set_target_properties(WinShunUpdate PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

install(TARGETS WinShunUpdate RUNTIME DESTINATION .)
