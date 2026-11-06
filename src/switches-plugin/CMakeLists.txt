# Optional native switch provider. The application links only its async UI
# adapter; device watchers and power handles are confined to this process.
set(app_dir "${PROJECT_SOURCE_DIR}/src/app")
set(core_dir "${PROJECT_SOURCE_DIR}/src/core")
set(native_sources)
foreach(name IN ITEMS SwitchAudio SwitchExplorer SwitchKeepAwake SwitchRadios SwitchTheme SystemSwitches)
    list(APPEND native_sources "platform/${name}.cpp" "platform/${name}.h")
endforeach()
qt_add_library(wsswitch_native STATIC ${native_sources}
    ${app_dir}/platform/RegistryWatch.cpp ${app_dir}/platform/SettingBroadcast.cpp
    ${core_dir}/Win32Util.cpp)
target_include_directories(wsswitch_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${app_dir} ${core_dir})
target_link_libraries(wsswitch_native PUBLIC winshun_options Qt6::Core
    PRIVATE ole32 oleaut32 shell32 advapi32 user32 gdi32 powrprof iphlpapi windowsapp)

qt_add_library(wsswitch_service STATIC SwitchProtocol.cpp SwitchProtocol.h SwitchService.cpp SwitchService.h
    ${app_dir}/platform/ComWorker.cpp)
target_include_directories(wsswitch_service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${app_dir} ${core_dir})
target_link_libraries(wsswitch_service PUBLIC winshun_options Qt6::Core PRIVATE ole32 user32)

add_executable(WinShunSwitches WIN32 stub.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunSwitches.rc)
target_link_libraries(WinShunSwitches PRIVATE winshun_options)
set_target_properties(WinShunSwitches PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
qt_add_library(WinShunSwitchesApp SHARED PluginMain.cpp)
target_link_libraries(WinShunSwitchesApp PRIVATE wsswitch_native wsswitch_service)

# Reuse the existing switch translations without running a second lupdate
# over the shared catalog. Relative paths also work on non-Chinese Windows.
set(qm_file "${CMAKE_CURRENT_BINARY_DIR}/winshun_en.qm")
file(RELATIVE_PATH qm_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${qm_file}")
add_custom_command(OUTPUT "${qm_file}"
    COMMAND Qt6::lrelease -silent ../app/i18n/winshun_en.ts -qm "${qm_relative}"
    DEPENDS ${app_dir}/i18n/winshun_en.ts
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    VERBATIM)
qt_add_resources(WinShunSwitchesApp "switch_translations" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${qm_file}")
