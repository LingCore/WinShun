# WinShunLocalSend.exe: the winshun.localsend plugin that comes with Win顺
# (plugins/winshun.localsend, "局域网传输", GitHub #113), a resident C++
# plugin on the single-header SDK (src/plugins-sdk/winshun.h): files and
# text to and from devices nearby running LocalSend (protocol v2.2), with
# Windows' own TLS (Schannel) and sockets. No Qt, the C runtime linked in.
set(localsend_sources
    Background.h
    Client.cpp Client.h
    Discovery.cpp Discovery.h
    Firewall.cpp Firewall.h
    HttpLite.cpp HttpLite.h
    Identity.cpp Identity.h
    Network.cpp Network.h
    Protocol.cpp Protocol.h
    RateLimits.cpp RateLimits.h
    Rows.cpp Rows.h
    Saver.cpp Saver.h
    Sender.cpp Sender.h
    Server.cpp Server.h
    Tls.cpp Tls.h
    Util.cpp Util.h
)
set(localsend_libraries ws2_32 secur32 crypt32 ncrypt bcrypt iphlpapi ole32 oleaut32 shell32 advapi32)

add_executable(WinShunLocalSend
    main.cpp
    ${localsend_sources}
    ${PROJECT_SOURCE_DIR}/resources/WinShunLocalSend.rc
)
target_include_directories(WinShunLocalSend PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunLocalSend PRIVATE winshun_options ${localsend_libraries})
set_target_properties(WinShunLocalSend PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The same code for tests/tst_localsend.cpp (with the C runtime the tests
# and Qt use): the parsers, the names made safe, a loopback receiver and
# sender over TLS.
add_library(wslocalsend STATIC ${localsend_sources})
target_include_directories(wslocalsend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(wslocalsend PUBLIC winshun_options ${localsend_libraries})
set_target_properties(wslocalsend PROPERTIES AUTOMOC OFF)
