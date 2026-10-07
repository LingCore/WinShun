qt_add_executable(QuickFind WIN32
    main.cpp
    App.cpp App.h
    FileIconProvider.cpp FileIconProvider.h
    Settings.cpp Settings.h
    platform/KeyboardHook.cpp platform/KeyboardHook.h
    platform/MessageWindow.cpp platform/MessageWindow.h
    platform/Shell.cpp platform/Shell.h
    platform/VolumeNotifier.cpp platform/VolumeNotifier.h
    platform/WindowEffects.cpp platform/WindowEffects.h
    ${PROJECT_SOURCE_DIR}/resources/app.rc
    ${PROJECT_SOURCE_DIR}/resources/app.manifest
)

set(qml_files
    qml/Main.qml
    qml/Theme.qml
    qml/SearchBar.qml
    qml/ScopeTabs.qml
    qml/ResultRow.qml
    qml/RowAction.qml
    qml/ListScrollBar.qml
    qml/ContextMenu.qml
    qml/HoverTip.qml
    qml/Footer.qml
    qml/KeyHint.qml
    qml/Glyph.qml
    qml/SettingsWindow.qml
    qml/SettingsSection.qml
    qml/SettingRow.qml
    qml/ToggleSwitch.qml
    qml/FlatButton.qml
    qml/InputBox.qml
    qml/Chip.qml
    qml/HotkeyRecorder.qml
    qml/Gleaning.qml
    qml/WarmPalette.qml
    qml/AuthorClock.qml
    qml/AuthorAvatar.qml
    qml/GleaningBackdrop.qml
    qml/GleaningPage.qml
)
set_source_files_properties(qml/Theme.qml qml/Gleaning.qml qml/WarmPalette.qml qml/AuthorClock.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
# Serve every QML file from the module root (":/qt/qml/QuickFind/Main.qml").
foreach(file IN LISTS qml_files)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()

# Pictures of the 拾穗计划 page, at ":/qt/qml/QuickFind/gleaning/".
set(gleaning_images
    ${PROJECT_SOURCE_DIR}/resources/gleaning/avatar.png
    ${PROJECT_SOURCE_DIR}/resources/gleaning/winshun.png
    ${PROJECT_SOURCE_DIR}/resources/gleaning/macshun.png
)
foreach(file IN LISTS gleaning_images)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "gleaning/${name}")
endforeach()

qt_add_qml_module(QuickFind
    URI QuickFind
    VERSION 1.0
    QML_FILES ${qml_files}
    RESOURCES ${gleaning_images}
    SOURCES
        AuthorMark.cpp AuthorMark.h
        GleaningScene.cpp GleaningScene.h
        IconBadge.cpp IconBadge.h
        Launcher.cpp Launcher.h
        ResultModel.cpp ResultModel.h
        SettingsEditor.cpp SettingsEditor.h
        SystemTheme.cpp SystemTheme.h
)

# With a shared Qt, QML plugins load at run time, so the configure-time import
# scan is only needed for static Qt. Skipping it also avoids a qmlimportscanner
# bug with non-ASCII source paths (such as this project's folder name).
if(QT6_IS_SHARED_LIBS_BUILD)
    set_target_properties(QuickFind PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()

target_include_directories(QuickFind PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(QuickFind PRIVATE
    qfcore
    Qt6::Gui Qt6::Qml Qt6::Quick
    dwmapi shell32 ole32 user32 uxtheme)
# The execution level (requireAdministrator) comes from app.manifest alone; the
# linker's own default (asInvoker) would conflict with it.
target_link_options(QuickFind PRIVATE /MANIFESTUAC:NO)

# UI font files, loaded from fonts\ next to the exe (see main.cpp).
set(font_files
    ${PROJECT_SOURCE_DIR}/resources/fonts/AlibabaPuHuiTi-3-55-Regular.ttf
    ${PROJECT_SOURCE_DIR}/resources/fonts/AlibabaPuHuiTi-3-85-Bold.ttf
)
add_custom_command(TARGET QuickFind POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:QuickFind>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${font_files} "$<TARGET_FILE_DIR:QuickFind>/fonts"
    VERBATIM)

install(TARGETS QuickFind RUNTIME DESTINATION .)
install(FILES ${font_files} DESTINATION fonts)
