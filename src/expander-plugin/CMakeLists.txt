# WinShunExpander.exe: the winshun.expander plugin for the plugin library
# (package/, "缩写展开", GitHub #403; not in the setup), a resident C++
# plugin on the single-header SDK (src/plugins-sdk/winshun.h): an
# abbreviation typed anywhere becomes its text. No Qt, the C runtime linked
# in. The rules (Expander) are plain C++ that tests/tst_expander.cpp builds
# in again; Engine hooks the keyboard and types.
add_executable(WinShunExpander
    main.cpp
    Engine.cpp Engine.h
    Expander.cpp Expander.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunExpander.rc
)
target_include_directories(WinShunExpander PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunExpander PRIVATE winshun_options user32 imm32 advapi32)
set_target_properties(WinShunExpander PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
