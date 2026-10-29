# WinShunService.exe: the index service (see main.cpp), the one part of Win顺
# with system rights: it launches a user's UIAccess input listener and reads
# the NTFS volumes' master file tables and
# change journals for a WinShun.exe that runs with the user's normal rights,
# over a pipe (src/core/VolumeProtocol.h; the client is ServiceVolumeAccess).
# Plain Win32 with the C runtime linked in and no Qt, so that an upgrade
# never finds a Qt DLL held open by a service; the NTFS reader and the
# protocol are built in from src/core (wscore would bring Qt along).
add_executable(WinShunService
    main.cpp
    Log.cpp Log.h
    Server.cpp Server.h
    InputListener.cpp InputListener.h
    ${PROJECT_SOURCE_DIR}/src/app/platform/KeyListener.cpp
    Volumes.cpp
    ${PROJECT_SOURCE_DIR}/src/core/DialogNav.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Ntfs.cpp
    ${PROJECT_SOURCE_DIR}/src/core/NtfsBulk.cpp
    ${PROJECT_SOURCE_DIR}/src/core/ServiceLocation.cpp
    ${PROJECT_SOURCE_DIR}/src/core/VolumeProtocol.cpp
    ${PROJECT_SOURCE_DIR}/resources/WinShunService.rc)
target_include_directories(WinShunService PRIVATE ${PROJECT_SOURCE_DIR}/src/core ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(WinShunService PRIVATE winshun_options advapi32 ole32 oleaut32 shcore shell32 user32 userenv wtsapi32)
set_target_properties(WinShunService PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

install(TARGETS WinShunService RUNTIME DESTINATION .)
