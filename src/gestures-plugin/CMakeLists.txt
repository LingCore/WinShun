# WinShunGestures.exe: the winshun.gestures plugin for the plugin library
# (package/, "鼠标手势"; not in the setup), a resident C++ plugin on the
# single-header SDK (src/plugins-sdk/winshun.h): hold the right button and
# draw, push into a corner, rub an edge (docs/design-gestures.md). No Qt,
# the C runtime linked in. The rules (Gesture) are plain C++ that
# tests/tst_gestures.cpp builds in again; Engine hooks the mouse and draws
# the trail with GDI+, Cards the notes, the card that asks first and the
# translation; Selection reads the words selected with UI Automation.
# EditorHost starts the settings window (below) as `WinShunGestures.exe
# --editor`, and EditorStub.cpp is that start (still no Qt in the exe).
add_executable(WinShunGestures
    main.cpp
    Cards.cpp Cards.h
    EditorHost.cpp EditorHost.h
    EditorStub.cpp EditorStub.h
    Engine.cpp Engine.h
    Gesture.cpp Gesture.h
    Look.h
    Selection.cpp Selection.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunGestures.rc
)
target_include_directories(WinShunGestures PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunGestures PRIVATE winshun_options user32 gdi32 gdiplus shcore shell32 advapi32 ole32 oleaut32 dwmapi)
set_target_properties(WinShunGestures PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The .rc embeds the icon subset (icons/PhosphorGestures.ttf) as RCDATA: rebuilt when it changes.
set_source_files_properties(${PROJECT_SOURCE_DIR}/resources/WinShunGestures.rc PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/icons/PhosphorGestures.ttf)

# The settings window (设置 on the plugin's card): Qt Quick on Win顺's own
# Qt, as the capture's and the developer tools' windows are. The exe above
# stays without Qt: `WinShunGestures.exe --editor` finds Win顺's folder
# (WS_APP_DIR), puts it on the DLL path and loads WinShunGesturesEditor.dll
# from its own folder; the plugin and the window talk in JSON lines over
# the window's stdin and stdout (editor/EditorLink.h). The rules (Gesture.cpp)
# are built in again, so it names and reads them as the plugin does.
set(app_dir ${PROJECT_SOURCE_DIR}/src/app)
set(editor_dir ${CMAKE_CURRENT_SOURCE_DIR}/editor)
# The common controls of Win顺's own module (src/app/qml), as the capture's
# window has them (src/capture-plugin/CMakeLists.txt), and the window's own.
set(editor_shared_qml
    Theme.qml Hook.qml UiScale.qml WindowEdge.qml HoverTip.qml DropMenu.qml Glyph.qml TitleBar.qml ToggleSwitch.qml FlatButton.qml
    InputBox.qml Caret.qml CenteredNumber.qml KeyCap.qml HotkeyRecorder.qml ListScrollBar.qml ListWheelScroll.qml
    ScrollPixelAlignment.qml SettingsSection.qml SettingRow.qml PageTabs.qml ScopeTabs.qml Chip.qml SettingTextArea.qml)
set(editor_qml)
foreach(name IN LISTS editor_shared_qml)
    list(APPEND editor_qml "${app_dir}/qml/${name}")
endforeach()
list(APPEND editor_qml "${PROJECT_SOURCE_DIR}/src/capture-plugin/qml/SettingsSearch.qml")
foreach(name IN ITEMS GesturesWindow GestureList GesturePad RuleRow SlotRow ActionPicker ActionButton ZoneButton PickerPanel PickRow
                      CornersPage ProgramsPage GeneralPage ProgramPicker)
    list(APPEND editor_qml "${editor_dir}/qml/${name}.qml")
endforeach()
set_source_files_properties("${app_dir}/qml/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(file IN LISTS editor_qml)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()
# The 界面风格, as WinShun's own module has them (src/app/CMakeLists.txt).
file(GLOB editor_theme_qml CONFIGURE_DEPENDS "${app_dir}/qml/themes/*/*.qml")
foreach(file IN LISTS editor_theme_qml)
    file(RELATIVE_PATH alias "${app_dir}/qml" "${file}")
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}" QT_QML_SKIP_QMLDIR_ENTRY TRUE)
endforeach()
list(APPEND editor_qml ${editor_theme_qml})
file(GLOB editor_theme_images CONFIGURE_DEPENDS "${app_dir}/qml/themes/*/*.png")
foreach(file IN LISTS editor_theme_images)
    file(RELATIVE_PATH alias "${app_dir}/qml" "${file}")
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
endforeach()

# GestureEditor, the models and the pad's reader are QML types: in the
# module's own target.
qt_add_library(WinShunGesturesEditor SHARED
    editor/EditorMain.cpp
    editor/EditorLink.cpp editor/EditorLink.h
    editor/GestureEditor.cpp editor/GestureEditor.h
    editor/PadReader.cpp editor/PadReader.h
    editor/RuleModels.cpp editor/RuleModels.h
    Gesture.cpp Gesture.h
    ${app_dir}/SystemTheme.cpp ${app_dir}/SystemTheme.h
    ${app_dir}/PhosphorGlyph.cpp ${app_dir}/PhosphorGlyph.h
    ${app_dir}/WindowFrame.cpp ${app_dir}/WindowFrame.h
    ${app_dir}/HotkeyRule.h
    ${app_dir}/Phosphor.cpp ${app_dir}/platform/WindowEffects.cpp ${app_dir}/platform/ShellDialogs.cpp
    ${app_dir}/platform/Foreground.cpp ${app_dir}/platform/DragOut.cpp
    ${app_dir}/platform/HotPages.cpp ${app_dir}/platform/HotPages.h
    ${app_dir}/platform/IdleTrim.cpp ${app_dir}/platform/IdleTrim.h)
qt_add_qml_module(WinShunGesturesEditor URI WinShun VERSION 1.0 NO_PLUGIN NO_IMPORT_SCAN NO_GENERATE_EXTRA_QMLDIRS
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/WinShun" QML_FILES ${editor_qml} RESOURCES ${editor_theme_images})
# Bytecode only, as the capture's: the plugin has to run on whichever Qt 6
# the Win顺 it is installed into comes with.
set_target_properties(WinShunGesturesEditor PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
target_include_directories(WinShunGesturesEditor PRIVATE ${app_dir} ${CMAKE_CURRENT_SOURCE_DIR} ${editor_dir} ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunGesturesEditor PRIVATE winshun_options wscore Qt6::Gui Qt6::Qml Qt6::Quick
    advapi32 comctl32 dwmapi gdi32 imm32 ole32 oleaut32 psapi shell32 shcore shlwapi user32 uxtheme)
qt_add_resources(WinShunGesturesEditor "gestures_icons" PREFIX /fonts BASE "${PROJECT_SOURCE_DIR}/third_party/phosphor"
    FILES "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf" "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor-Fill.ttf")

# English, as the capture's (src/capture-plugin/CMakeLists.txt): the words are
# Chinese in the QML and C++, editor/i18n/gestures_en.ts has the English, built
# into ":/i18n/gestures_en.qm". After changing the window's words:
# cmake --build --preset release --target gestures_lupdate
qt_add_lupdate(WinShunGesturesEditor LUPDATE_TARGET gestures_lupdate
    TS_FILES editor/i18n/gestures_en.ts
    OPTIONS -source-language zh_CN -no-obsolete -locations none)
set(editor_qm "${CMAKE_CURRENT_BINARY_DIR}/gestures_en.qm")
file(RELATIVE_PATH editor_qm_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${editor_qm}")
add_custom_command(OUTPUT "${editor_qm}"
    COMMAND Qt6::lrelease -silent editor/i18n/gestures_en.ts -qm "${editor_qm_relative}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/editor/i18n/gestures_en.ts"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
qt_add_resources(WinShunGesturesEditor "gestures_translation" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${editor_qm}")
