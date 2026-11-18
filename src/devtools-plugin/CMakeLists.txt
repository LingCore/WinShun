set(app_dir ${PROJECT_SOURCE_DIR}/src/app)
# What the tests read without a window: the reader, the writer, the rows.
qt_add_library(wsdevtools STATIC Json.cpp Json.h Launcher.cpp Launcher.h)
target_include_directories(wsdevtools PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wsdevtools PUBLIC winshun_options Qt6::Core)

add_executable(WinShunDevTools WIN32 stub/main.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunDevTools.rc)
target_link_libraries(WinShunDevTools PRIVATE winshun_options)
target_link_options(WinShunDevTools PRIVATE /ENTRY:wmainCRTStartup)
set_target_properties(WinShunDevTools PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(devtools_qml)
foreach(name IN ITEMS Theme Hook UiScale WindowEdge HoverTip DropMenu Glyph TitleBar FlatButton Caret
    ListScrollBar ListWheelScroll ScrollPixelAlignment)
    list(APPEND devtools_qml "${app_dir}/qml/${name}.qml")
endforeach()
foreach(name IN ITEMS JsonWindow JsonToolbar JsonTextPane JsonTreePane JsonRow)
    list(APPEND devtools_qml "${CMAKE_CURRENT_SOURCE_DIR}/qml/${name}.qml")
endforeach()
foreach(file IN LISTS devtools_qml)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()
set_source_files_properties("${app_dir}/qml/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
# The 界面风格, as WinShun's own module has them (src/app/CMakeLists.txt).
file(GLOB theme_qml CONFIGURE_DEPENDS "${app_dir}/qml/themes/*/*.qml")
foreach(file IN LISTS theme_qml)
    file(RELATIVE_PATH alias "${app_dir}/qml" "${file}")
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}" QT_QML_SKIP_QMLDIR_ENTRY TRUE)
endforeach()
list(APPEND devtools_qml ${theme_qml})
file(GLOB theme_images CONFIGURE_DEPENDS "${app_dir}/qml/themes/*/*.png")
foreach(file IN LISTS theme_images)
    file(RELATIVE_PATH alias "${app_dir}/qml" "${file}")
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
endforeach()
# JsonTree and JsonTool are QML types: in the module's own target (the tests
# compile them in too).
qt_add_library(WinShunDevToolsApp SHARED DevMain.cpp JsonTree.cpp JsonTree.h JsonTool.cpp JsonTool.h
    ${PROJECT_SOURCE_DIR}/src/capture-plugin/Protocol.cpp
    ${app_dir}/SystemTheme.cpp ${app_dir}/SystemTheme.h
    ${app_dir}/PhosphorGlyph.cpp ${app_dir}/PhosphorGlyph.h
    ${app_dir}/WindowFrame.cpp ${app_dir}/WindowFrame.h
    ${app_dir}/Phosphor.cpp ${app_dir}/platform/WindowEffects.cpp ${app_dir}/platform/ShellDialogs.cpp
    ${app_dir}/platform/Foreground.cpp ${app_dir}/platform/DragOut.cpp
    ${app_dir}/platform/HotPages.cpp ${app_dir}/platform/HotPages.h
    ${app_dir}/platform/IdleTrim.cpp ${app_dir}/platform/IdleTrim.h)
qt_add_qml_module(WinShunDevToolsApp URI WinShun VERSION 1.0 NO_PLUGIN NO_IMPORT_SCAN NO_GENERATE_EXTRA_QMLDIRS
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/WinShun" QML_FILES ${devtools_qml} RESOURCES ${theme_images})
set_target_properties(WinShunDevToolsApp PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
target_include_directories(WinShunDevToolsApp PRIVATE ${app_dir})
target_link_libraries(WinShunDevToolsApp PRIVATE wsdevtools wscore Qt6::Gui Qt6::Qml Qt6::Quick
    advapi32 comctl32 dwmapi gdi32 imm32 ole32 oleaut32 psapi shell32 shcore shlwapi user32 uxtheme)
qt_add_resources(WinShunDevToolsApp "devtools_icons" PREFIX /fonts BASE "${PROJECT_SOURCE_DIR}/third_party/phosphor"
    FILES "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf" "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor-Fill.ttf")

qt_add_lupdate(WinShunDevToolsApp LUPDATE_TARGET devtools_lupdate
    TS_FILES i18n/devtools_en.ts SOURCES Json.cpp Launcher.cpp)
set(qm_file "${CMAKE_CURRENT_BINARY_DIR}/devtools_en.qm")
file(RELATIVE_PATH qm_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${qm_file}")
add_custom_command(OUTPUT "${qm_file}"
    COMMAND Qt6::lrelease i18n/devtools_en.ts -qm "${qm_relative}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/i18n/devtools_en.ts"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
qt_add_resources(WinShunDevToolsApp "devtools_translation" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${qm_file}")
