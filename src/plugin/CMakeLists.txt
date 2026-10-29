# The plugin system (docs/architecture.md "插件"): manifests, the catalog of
# installed plugins, zero-code commands, and plugin processes spoken to in
# JSON-RPC lines -- started not by Win顺 (elevated) but by a runner with the
# user's normal rights (WinShunPluginHost.exe), which Win顺 starts once.

# wsplugin_lite: what the runner and the test plugin (tests/plugin_fake)
# share, with no Qt and the C runtime linked in: JSON, the sandbox pieces
# (restricted token, job limits, environment, the facts a process reports
# about itself).
add_library(wsplugin_lite STATIC
    runner/Json.cpp runner/Json.h
    runner/Lite.h
    runner/Sandbox.cpp runner/Sandbox.h
)
target_include_directories(wsplugin_lite PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runner)
target_link_libraries(wsplugin_lite PUBLIC winshun_options advapi32 userenv user32)
set_target_properties(wsplugin_lite PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# WinShunPluginHost.exe: the runner (runner/main.cpp). The linker's default
# manifest runs it as the invoker; Win顺 gives it Explorer's token.
add_executable(WinShunPluginHost WIN32 runner/main.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunPluginHost.rc)
target_link_libraries(WinShunPluginHost PRIVATE wsplugin_lite shell32)
set_target_properties(WinShunPluginHost PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

install(TARGETS WinShunPluginHost RUNTIME DESTINATION .)

# wsplugin: Win顺's side. runner/Sandbox.cpp is built in again (with Qt's C
# runtime this time) for the token and job of the runner itself.
qt_add_library(wsplugin STATIC
    AiBridge.cpp AiBridge.h
    Catalog.cpp Catalog.h
    Gallery.cpp Gallery.h
    Host.cpp Host.h
    Install.cpp Install.h
    Items.cpp Items.h
    Manifest.cpp Manifest.h
    Merge.cpp Merge.h
    OpenPolicy.cpp OpenPolicy.h
    Programs.cpp Programs.h
    Runner.cpp Runner.h
    Secrets.cpp Secrets.h
    Session.cpp Session.h
    ToastPolicy.cpp ToastPolicy.h
    Tools.cpp Tools.h
    ZeroCode.cpp ZeroCode.h
    runner/Sandbox.cpp
)
target_include_directories(wsplugin PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/runner)
# miniz: archives of plugins (Install.cpp); the Credential Manager keeps
# their passwords (Secrets.cpp).
# shlwapi: AssocIsDangerous, for the open policy (OpenPolicy.cpp).
target_link_libraries(wsplugin PUBLIC wscore wsmcp wsmcp_proto PRIVATE miniz advapi32 userenv user32 shlwapi)
