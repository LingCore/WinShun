qt_add_executable(WinShun WIN32
    main.cpp
    App.cpp App.h
    FileIconProvider.cpp FileIconProvider.h
    Migration.cpp Migration.h
    Settings.cpp Settings.h
    platform/Http.cpp platform/Http.h
    platform/KeyListener.cpp platform/KeyListener.h
    platform/MessageWindow.cpp platform/MessageWindow.h
    platform/Shell.cpp platform/Shell.h
    platform/ShortcutCapture.cpp platform/ShortcutCapture.h
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
    qml/TitleBar.qml
    qml/SettingsSection.qml
    qml/SettingRow.qml
    qml/ThemeCard.qml
    qml/UpdateDialog.qml
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
# Serve every QML file from the module root (":/qt/qml/WinShun/Main.qml").
foreach(file IN LISTS qml_files)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()

# Pictures of the 拾穗计划 page, and the sound its avatar plays when pressed,
# at ":/qt/qml/WinShun/gleaning/".
set(gleaning_images
    ${PROJECT_SOURCE_DIR}/resources/gleaning/avatar.png
    ${PROJECT_SOURCE_DIR}/resources/gleaning/winshun.png
    ${PROJECT_SOURCE_DIR}/resources/gleaning/macshun.png
    ${PROJECT_SOURCE_DIR}/resources/gleaning/ikun.mp3
)
foreach(file IN LISTS gleaning_images)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "gleaning/${name}")
endforeach()

# With a shared Qt, QML plugins load at run time, so the import scan is only
# needed for static Qt. Skipping it also avoids a qmlimportscanner bug with
# non-ASCII source paths (such as this project's folder name). Qt 6.12 scans
# at build time as well and decides early: it has to be said here, not set
# on the target afterwards.
set(qml_import_scan "")
if(QT6_IS_SHARED_LIBS_BUILD)
    set(qml_import_scan NO_IMPORT_SCAN)
endif()

qt_add_qml_module(WinShun
    URI WinShun
    VERSION 1.0
    ${qml_import_scan}
    QML_FILES ${qml_files}
    RESOURCES ${gleaning_images}
    SOURCES
        AuthorMark.cpp AuthorMark.h
        GleaningScene.cpp GleaningScene.h
        IconBadge.cpp IconBadge.h
        Launcher.cpp Launcher.h
        Placement.cpp Placement.h
        ResultModel.cpp ResultModel.h
        SettingsEditor.cpp SettingsEditor.h
        SoundClip.cpp SoundClip.h
        SystemTheme.cpp SystemTheme.h
        Updater.cpp Updater.h
        WindowFrame.cpp WindowFrame.h
)

# The UI is written in Chinese; English comes from i18n/winshun_en.ts, built
# into ":/i18n/winshun_en.qm" and switched at run time (App::applyAppearance).
# After changing UI text: cmake --build --preset release --target update_translations,
# then translate the new entries in the .ts file (Qt Linguist or by hand).
qt_add_lupdate(WinShun
    TS_FILES i18n/winshun_en.ts
    OPTIONS -source-language zh_CN -no-obsolete -locations none)
# lrelease reads its arguments in the ANSI code page, which mangles a Chinese
# path (this project's folder) unless Windows uses a Chinese one; so not
# qt_add_lrelease, but relative paths from the source folder.
set(qm_file "${CMAKE_CURRENT_BINARY_DIR}/winshun_en.qm")
file(RELATIVE_PATH qm_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${qm_file}")
add_custom_command(OUTPUT "${qm_file}"
    COMMAND Qt6::lrelease -silent i18n/winshun_en.ts -qm "${qm_relative}"
    DEPENDS i18n/winshun_en.ts
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
qt_add_resources(WinShun "translations"
    PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES "${qm_file}")

target_include_directories(WinShun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinShun PRIVATE
    wscore
    Qt6::Gui Qt6::Qml Qt6::Quick
    comctl32 dwmapi shell32 shlwapi ole32 user32 uxtheme winhttp winmm
    mfplat mfreadwrite mfuuid delayimp)
# The execution level (requireAdministrator) comes from app.manifest alone; the
# linker's own default (asInvoker) would conflict with it. Media Foundation is
# loaded only when used (SoundClip): Windows N editions do not have it.
target_link_options(WinShun PRIVATE /MANIFESTUAC:NO /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)

# UI font files, loaded from fonts\ next to the exe (see main.cpp).
set(font_files
    ${PROJECT_SOURCE_DIR}/resources/fonts/AlibabaPuHuiTi-3-55-Regular.ttf
    ${PROJECT_SOURCE_DIR}/resources/fonts/AlibabaPuHuiTi-3-85-Bold.ttf
)
add_custom_command(TARGET WinShun POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:WinShun>/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${font_files} "$<TARGET_FILE_DIR:WinShun>/fonts"
    VERBATIM)

install(TARGETS WinShun RUNTIME DESTINATION .)
install(FILES ${font_files} DESTINATION fonts)
