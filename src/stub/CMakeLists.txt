# WinShunSearch.exe: the button the user pins to the taskbar (see main.cpp).
# Plain Win32 with the C runtime linked in: a click starts it, so it must
# load next to nothing. The linker's default manifest runs it as invoker, not
# elevated like WinShun.exe.
add_executable(WinShunSearch WIN32 main.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunSearch.rc)
target_link_libraries(WinShunSearch PRIVATE winshun_options taskschd ole32 oleaut32 shell32 advapi32 user32)
set_target_properties(WinShunSearch PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

install(TARGETS WinShunSearch RUNTIME DESTINATION .)
