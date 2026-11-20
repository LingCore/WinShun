# WinShunSync.exe: the winshun.sync plugin for the plugin library (package/,
# "剪贴板同步"; not in the setup), a resident C++ plugin on the single-header
# SDK (src/plugins-sdk/winshun.h): the clipboard shared with this user's other
# Windows PCs and Macs (Mac顺) over ShunSync v1 (docs/shunsync-protocol.md),
# end to end encrypted, no account and no server. Windows' own CNG, sockets
# and clipboard; no Qt, the C runtime linked in. LAN transfer's network list,
# firewall facts, safe names and helpers (src/localsend-plugin) are built in
# again.
set(sync_localsend_sources
    ${PROJECT_SOURCE_DIR}/src/localsend-plugin/Firewall.cpp
    ${PROJECT_SOURCE_DIR}/src/localsend-plugin/Network.cpp
    ${PROJECT_SOURCE_DIR}/src/localsend-plugin/Saver.cpp
    ${PROJECT_SOURCE_DIR}/src/localsend-plugin/Util.cpp
)
set(sync_sources
    Crypto.cpp Crypto.h
    X25519.cpp
    Protocol.cpp Protocol.h
    Engine.cpp Engine.h
    ${sync_localsend_sources}
)
set(sync_libraries ws2_32 iphlpapi bcrypt crypt32 ole32 oleaut32 shell32 shlwapi advapi32 user32 windowscodecs)

add_executable(WinShunSync
    main.cpp
    ClipboardWin.cpp ClipboardWin.h
    ${sync_sources}
    ${PROJECT_SOURCE_DIR}/resources/WinShunSync.rc
)
target_include_directories(WinShunSync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/plugins-sdk
    ${PROJECT_SOURCE_DIR}/src/localsend-plugin)
target_link_libraries(WinShunSync PRIVATE winshun_options ${sync_libraries})
set_target_properties(WinShunSync PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The protocol and the engine for tests/tst_shunsync.cpp (with the C runtime
# the tests and Qt use).
add_library(wsshunsync STATIC ${sync_sources})
target_include_directories(wsshunsync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/plugins-sdk
    ${PROJECT_SOURCE_DIR}/src/localsend-plugin)
target_link_libraries(wsshunsync PUBLIC winshun_options ${sync_libraries})
set_target_properties(wsshunsync PROPERTIES AUTOMOC OFF)
