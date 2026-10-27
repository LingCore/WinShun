# The words in pictures (platform/PaddleOcr): PP-OCRv6 tiny (PaddleOCR,
# Apache-2.0; ONNX from RapidOCR) run by ONNX Runtime (MIT), both downloaded
# once per build folder. onnxruntime.dll goes next to the programs and the
# models into ocr\; the app loads the DLL itself by its full path (Windows
# has an older onnxruntime.dll in System32), so it only needs the headers.
set(ORT_VERSION 1.30.0)
set(ORT_SHA256 c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949)
set(ORT_DIR "${CMAKE_BINARY_DIR}/_deps/onnxruntime-win-x64-${ORT_VERSION}" CACHE INTERNAL "")
if(NOT EXISTS "${ORT_DIR}/lib/onnxruntime.dll")
    set(archive "${CMAKE_BINARY_DIR}/_deps/onnxruntime-win-x64-${ORT_VERSION}.zip")
    file(DOWNLOAD
        "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-x64-${ORT_VERSION}.zip"
        "${archive}" EXPECTED_HASH SHA256=${ORT_SHA256} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Could not download ONNX Runtime: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_BINARY_DIR}/_deps")
    file(REMOVE "${archive}")
endif()
add_library(onnxruntime_headers INTERFACE IMPORTED GLOBAL)
set_target_properties(onnxruntime_headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${ORT_DIR}/include")
file(COPY "${ORT_DIR}/lib/onnxruntime.dll" DESTINATION "${CMAKE_BINARY_DIR}")

set(ocr_models
    "det/PP-OCRv6_det_tiny.onnx|f42c0fbd294d95eac1a550e131b277dac97462c8025fa4b6c3cec1b7894bd3d5"
    "rec/PP-OCRv6_rec_tiny.onnx|e16e242de5937ad92609223f19bc2aff3727ee40b095f996907c24749bad251b")
set(OCR_MODEL_FILES "" CACHE INTERNAL "")
foreach(model IN LISTS ocr_models)
    string(REPLACE "|" ";" model "${model}")
    list(GET model 0 path)
    list(GET model 1 hash)
    get_filename_component(name "${path}" NAME)
    set(file "${CMAKE_BINARY_DIR}/ocr/${name}")
    if(NOT EXISTS "${file}")
        file(DOWNLOAD "https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6/${path}"
            "${file}" EXPECTED_HASH SHA256=${hash} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE "${file}")
            message(FATAL_ERROR "Could not download ${name}: ${status}")
        endif()
    endif()
    set(OCR_MODEL_FILES "${OCR_MODEL_FILES};${file}" CACHE INTERNAL "")
endforeach()

qt_add_executable(WinShun WIN32
    main.cpp
    AiSettings.cpp AiSettings.h
    App.cpp App.h
    ColorText.cpp ColorText.h
    DialogBarVisibility.cpp DialogBarVisibility.h
    FrameReveal.cpp FrameReveal.h
    Elide.cpp Elide.h
    FileIconProvider.cpp FileIconProvider.h
    McpClients.cpp McpClients.h
    Migration.cpp Migration.h
    PieGeometry.cpp PieGeometry.h
    PieLabels.cpp PieLabels.h
    Preview.cpp Preview.h
    RecentFolders.cpp RecentFolders.h
    Settings.cpp Settings.h
    SiteIconImage.cpp SiteIconImage.h
    SvgIcon.cpp SvgIcon.h
    Switches.cpp Switches.h SwitchBackend.h
    TextField.cpp TextField.h
    ThumbnailProvider.cpp ThumbnailProvider.h
    WebSuggester.cpp WebSuggester.h
    WindowLogo.cpp WindowLogo.h
    platform/ClipboardWatcher.cpp platform/ClipboardWatcher.h
    platform/ComWorker.cpp platform/ComWorker.h
    platform/Credentials.cpp platform/Credentials.h
    platform/DialogJump.cpp platform/DialogJump.h
    platform/DragOut.cpp platform/DragOut.h
    platform/ExternalPreview.cpp platform/ExternalPreview.h
    platform/FileDialog.cpp platform/FileDialog.h
    platform/FileManagers.cpp platform/FileManagers.h
    platform/FolderViewHit.cpp platform/FolderViewHit.h
    platform/Foreground.cpp platform/Foreground.h
    platform/Http.cpp platform/Http.h
    platform/IdleTrim.cpp platform/IdleTrim.h
    platform/Integrity.cpp platform/Integrity.h
    platform/KeyListener.cpp platform/KeyListener.h
    platform/PieDrop.cpp platform/PieDrop.h
    platform/ServiceKeyListener.cpp platform/ServiceKeyListener.h
    platform/KeyRouter.cpp platform/KeyRouter.h
    platform/MessageWindow.cpp platform/MessageWindow.h
    platform/MonoLogo.cpp platform/MonoLogo.h
    platform/NetworkNotifier.cpp platform/NetworkNotifier.h
    platform/NumberKeys.cpp platform/NumberKeys.h
    platform/Paster.cpp platform/Paster.h
    platform/PeekingLogo.cpp platform/PeekingLogo.h
    platform/PreviewHost.cpp platform/PreviewHost.h
    platform/Shell.cpp platform/Shell.h
    platform/ShellContextMenu.cpp platform/ShellContextMenu.h
    platform/ShellMenu.cpp platform/ShellMenu.h
    platform/ShortcutCapture.cpp platform/ShortcutCapture.h
    platform/SiteIcons.cpp platform/SiteIcons.h
    platform/RegistryWatch.cpp platform/RegistryWatch.h
    platform/SettingBroadcast.cpp platform/SettingBroadcast.h
    platform/SwitchAudio.cpp platform/SwitchAudio.h
    platform/SwitchExplorer.cpp platform/SwitchExplorer.h
    platform/SwitchKeepAwake.cpp platform/SwitchKeepAwake.h
    platform/SwitchRadios.cpp platform/SwitchRadios.h
    platform/SwitchTheme.cpp platform/SwitchTheme.h
    platform/SystemSwitches.cpp platform/SystemSwitches.h
    platform/TypeToSearch.cpp platform/TypeToSearch.h
    platform/TaskbarSearch.cpp platform/TaskbarSearch.h
    platform/TaskbarSearchBox.cpp platform/TaskbarSearchBox.h
    platform/TextCaret.cpp platform/TextCaret.h
    platform/PaddleOcr.cpp platform/PaddleOcr.h
    platform/TextRecognizer.cpp platform/TextRecognizer.h
    platform/VolumeNotifier.cpp platform/VolumeNotifier.h
    platform/WinV.cpp platform/WinV.h
    platform/WindowEffects.cpp platform/WindowEffects.h
    ${PROJECT_SOURCE_DIR}/resources/app.rc
    ${PROJECT_SOURCE_DIR}/resources/app.manifest
)

set(qml_files
    qml/Main.qml
    qml/Theme.qml
    qml/WindowEdge.qml
    qml/SearchBar.qml
    qml/ScopeTabs.qml
    qml/TypeStrip.qml
    qml/ResultRow.qml
    qml/Panel.qml
    qml/PanelRow.qml
    qml/FileThumbnail.qml
    qml/ClipboardWindow.qml
    qml/ClipboardPage.qml
    qml/ClipTabs.qml
    qml/ClipRow.qml
    qml/ClipPreview.qml
    qml/RowAction.qml
    qml/ListScrollBar.qml
    qml/ScrollPixelAlignment.qml
    qml/ListWheelScroll.qml
    qml/ContextMenu.qml
    qml/DialogBarWindow.qml
    qml/DialogBarRow.qml
    qml/FolderMenuWindow.qml
    qml/DropPieWindow.qml
    qml/SoftShadow.qml
    qml/PreviewWindow.qml
    qml/HoverTip.qml
    qml/ShortcutTip.qml
    qml/NameTip.qml
    qml/DropMenu.qml
    qml/Footer.qml
    qml/KeyHint.qml
    qml/Glyph.qml
    qml/SettingsWindow.qml
    qml/SettingsSearchBox.qml
    qml/TitleBar.qml
    qml/SettingsSection.qml
    qml/FileTypesSection.qml
    qml/SettingRow.qml
    qml/PageTabs.qml
    qml/ThemeCard.qml
    qml/AppearancePreview.qml
    qml/UpdateDialog.qml
    qml/ToggleSwitch.qml
    qml/SwitchLevel.qml
    qml/FlatButton.qml
    qml/InputBox.qml
    qml/WebShortcutForm.qml
    qml/FavoritesSection.qml
    qml/FavoriteForm.qml
    qml/ShareCard.qml
    qml/CenteredNumber.qml
    qml/IconNumber.qml
    qml/NumberHints.qml
    qml/ShortcutsButton.qml
    qml/KeyCap.qml
    qml/Chip.qml
    qml/HotkeyRecorder.qml
    qml/Gleaning.qml
    qml/WarmPalette.qml
    qml/AuthorClock.qml
    qml/AuthorAvatar.qml
    qml/GleaningBackdrop.qml
    qml/GleaningPage.qml
    qml/AiAccessSection.qml
    qml/AiClientsSection.qml
    qml/AiPrivacySection.qml
    qml/PluginsSection.qml
    qml/PluginCard.qml
    qml/Tag.qml
    qml/PluginDevSection.qml
    qml/PluginDialog.qml
    qml/PluginGallerySection.qml
    qml/PluginGalleryEntry.qml
    qml/PluginsPreview.qml
    qml/CaptureOverlay.qml
    qml/AnnotateToolbar.qml
    qml/AnnotateStylePanel.qml
    qml/AnnotateSmartHint.qml
    qml/AnnotateLayer.qml
    qml/AnnotateObjects.qml
    qml/EditWindow.qml
    qml/CaptureCard.qml
    qml/BuiltinCard.qml
    qml/PasteFileCard.qml
    qml/OcrPanel.qml
    qml/LongCapturePanel.qml
    qml/RecordingPanel.qml
    qml/ScreenshotConfirm.qml
    qml/SettingsConfirm.qml
    qml/SettingsNotice.qml
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
    ${PROJECT_SOURCE_DIR}/resources/gleaning/ghxi.png
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
        BoxSide.h
        ClipImage.cpp ClipImage.h
        ClipFiles.cpp ClipFiles.h
        ClipImport.cpp ClipImport.h
        ClipImportJob.cpp ClipImportJob.h
        ClipModel.cpp ClipModel.h
        Clipboard.cpp Clipboard.h
        ColorSwatch.cpp ColorSwatch.h
        DialogBar.cpp DialogBar.h
        FavoritesSettings.cpp FavoritesSettings.h
        FileDropTarget.cpp FileDropTarget.h
        FileFacts.cpp FileFacts.h
        FilePreview.cpp FilePreview.h
        FolderMenu.cpp FolderMenu.h
        DropPie.cpp DropPie.h # the favorite folders round the pointer while dragging files (DropPieWindow.qml)
        DropPieText.cpp DropPieText.h
        GleaningScene.cpp GleaningScene.h
        HotkeyRule.h
        IconBadge.cpp IconBadge.h
        Launcher.cpp Launcher.h LauncherNavigation.h
        Placement.cpp Placement.h
        PluginGallery.cpp PluginGallery.h
        PluginSettings.cpp PluginSettings.h
        ResultModel.cpp ResultModel.h
        PanelColumns.cpp PanelColumns.h
        SettingsEditor.cpp SettingsEditor.h
        Diagnostics.cpp Diagnostics.h
        DisplayDiagnostics.cpp DisplayDiagnostics.h # how Win顺 draws, for display problems (GitHub #70)
        TextCheck.cpp TextCheck.h
        SettingsSearch.cpp SettingsSearch.h
        SoundClip.cpp SoundClip.h
        SystemTheme.cpp SystemTheme.h
        UiSize.h
        Updater.cpp Updater.h UpdatePackage.cpp UpdatePackage.h
        Backup.cpp Backup.h BackupFiles.cpp BackupFiles.h
        WindowFrame.cpp WindowFrame.h
        # The screen capture's QML-facing part (src/capture/CMakeLists.txt has the rest).
        ../capture/Capture.cpp ../capture/Capture.h
        ../capture/CaptureImage.cpp ../capture/CaptureImage.h
        ../capture/AnnotationModel.cpp ../capture/AnnotationModel.h
        ../capture/AnnotationCanvas.cpp ../capture/AnnotationCanvas.h
        ../capture/CaptureSession.cpp ../capture/CaptureSession.h
        ../capture/CaptureEditor.cpp ../capture/CaptureEditor.h
        ../capture/CaptureOcr.cpp ../capture/CaptureOcr.h
        ../capture/LongCapture.cpp ../capture/LongCapture.h
        ../capture/Recorder.cpp ../capture/Recorder.h
        ../capture/PinManager.cpp ../capture/PinManager.h
        ../capture/PinWindow.cpp ../capture/PinWindow.h
)

# The UI is written in Chinese; English comes from i18n/winshun_en.ts, built
# into ":/i18n/winshun_en.qm" and switched at run time (App::applyAppearance).
# After changing UI text: cmake --build --preset release --target update_translations,
# then translate the new entries in the .ts file (Qt Linguist or by hand).
# The annotation's words that live in the wscapture library (tool names, undo labels, errors).
qt_add_lupdate(WinShun
    TS_FILES i18n/winshun_en.ts
    SOURCES ${PROJECT_SOURCE_DIR}/src/capture/AnnotationCommands.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/AnnotationFormat.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/AnnotationStyles.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/CountdownWindow.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/Recording.cpp
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
# The plugin SDK (src/plugins-sdk), copied into a new plugin's folder by
# "新建插件…" (PluginSettings::createFromTemplate): one source of truth.
qt_add_resources(WinShun "plugins_sdk"
    PREFIX /sdk
    BASE "${PROJECT_SOURCE_DIR}/src/plugins-sdk"
    FILES "${PROJECT_SOURCE_DIR}/src/plugins-sdk/winshun.py"
          "${PROJECT_SOURCE_DIR}/src/plugins-sdk/WinShun.psm1"
          "${PROJECT_SOURCE_DIR}/src/plugins-sdk/DEVELOPMENT.md"
          "${PROJECT_SOURCE_DIR}/src/plugins-sdk/winshun.h")

target_include_directories(WinShun PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(WinShun PRIVATE
    wscore wsmcp wsplugin
    wscapture # the screen capture (src/capture)
    Qt6::Gui Qt6::Qml Qt6::Quick
    Qt6::Svg # SvgIcon: web shortcuts' SVG icons, drawn once into the PNG kept
    comctl32 d2d1 dwmapi dwrite imm32 oleacc oleaut32 shcore shell32 shlwapi ole32 user32 uxtheme version winhttp winmm
    credui iphlpapi mpr psapi windowscodecs
    mfplat mfreadwrite mfuuid delayimp
    windowsapp # TextRecognizer: Windows.Media.Ocr, through C++/WinRT from the Windows SDK
    onnxruntime_headers) # PaddleOcr
target_link_libraries(WinShun PRIVATE advapi32 miniz) # miniz: settings backups (BackupFiles)
# The execution level (asInvoker, since 0.8.0) comes from app.manifest alone,
# with the rest of the manifest; the linker's own UAC fragment would be a
# second one. Media Foundation is loaded only when used (SoundClip): Windows
# N editions do not have it.
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
install(FILES "${ORT_DIR}/lib/onnxruntime.dll" DESTINATION .)
install(FILES ${OCR_MODEL_FILES} DESTINATION ocr)
install(FILES ${font_files} DESTINATION fonts)
