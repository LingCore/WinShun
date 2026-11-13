# WinShunCloudBackup.exe: the winshun.cloudbackup plugin ("云备份", GitHub
# #276) for the plugin library, a resident C++ plugin on the single-header
# SDK (src/plugins-sdk/winshun.h): Win顺's settings backup kept on a WebDAV
# server or in a private GitHub repository (WinHTTP). No Qt, the C runtime
# linked in. Where the backups go (Cloud) is plain C++ that
# tests/tst_cloudbackup.cpp builds in again.
add_executable(WinShunCloudBackup
    main.cpp
    Http.cpp Http.h
    Cloud.cpp Cloud.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunCloudBackup.rc
)
target_include_directories(WinShunCloudBackup PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunCloudBackup PRIVATE winshun_options winhttp)
set_target_properties(WinShunCloudBackup PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
