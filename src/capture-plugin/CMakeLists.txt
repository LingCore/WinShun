# winshun.capture (GitHub #138): the screenshot and screen recording, a
# plugin of the plugin library rather than a part of WinShun.exe.
# WinShunCapture.exe is a starter without Qt (stub/main.cpp): it finds Win顺's
# Qt where Win顺 is installed (WS_APP_DIR) and loads WinShunCaptureApp.dll,
# the plugin itself, built on that Qt. The build stages both, with the
# package's files, into library-plugins/winshun.capture (top CMakeLists.txt).

add_executable(WinShunCapture stub/main.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunCapture.rc)
target_link_libraries(WinShunCapture PRIVATE winshun_options)
set_target_properties(WinShunCapture PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# The capture's C++ (src/capture, wscapture) and its QML (qml/), with the
# common controls they use from WinShun's own module (src/app/qml): built
# into this one, whose URI is WinShun too (another process, so no clash).
set(app_dir ${PROJECT_SOURCE_DIR}/src/app)
set(shared_qml
    Theme.qml UiScale.qml WindowEdge.qml HoverTip.qml DropMenu.qml Glyph.qml TitleBar.qml ToggleSwitch.qml FlatButton.qml
    InputBox.qml CenteredNumber.qml KeyCap.qml HotkeyRecorder.qml ListScrollBar.qml ListWheelScroll.qml
    ScrollPixelAlignment.qml SettingsSection.qml SettingRow.qml PageTabs.qml ScopeTabs.qml)
set(capture_qml)
foreach(name IN LISTS shared_qml)
    list(APPEND capture_qml "${app_dir}/qml/${name}")
endforeach()
foreach(name IN ITEMS CaptureOverlay AnnotateToolbar AnnotateStylePanel AnnotateSmartHint AnnotateLayer AnnotateObjects
                      EditWindow OcrPanel LongCapturePanel RecordingPanel RecordingButton RecordingOptions CaptureOptions CaptureSettingsWindow CaptureSaveButton
                      SettingsSearch CaptureKeyHints PresenterOverlay PresenterToolbar)
    list(APPEND capture_qml "${CMAKE_CURRENT_SOURCE_DIR}/qml/${name}.qml")
endforeach()
set_source_files_properties("${app_dir}/qml/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(file IN LISTS capture_qml)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()

qt_add_library(WinShunCaptureApp SHARED
    PluginMain.cpp
    CaptureApp.cpp CaptureApp.h
    PinImage.cpp PinImage.h
    CapturePreview.cpp CapturePreview.h LongCapturePreview.cpp CaptureSavePreview.cpp
    RawClicks.cpp RawClicks.h
    Protocol.cpp Protocol.h
    PluginShell.cpp PluginShell.h
    PresenterHost.cpp PresenterHost.h
    ${app_dir}/Phosphor.cpp ${app_dir}/Phosphor.h
    ${app_dir}/TrayMenu.cpp ${app_dir}/TrayMenu.h
    ${app_dir}/PictureFile.cpp ${app_dir}/PictureFile.h
    ${app_dir}/ClipImage.cpp ${app_dir}/ClipImage.h
    ${app_dir}/platform/DragOut.cpp ${app_dir}/platform/DragOut.h
    ${app_dir}/platform/Foreground.cpp ${app_dir}/platform/Foreground.h
    ${app_dir}/platform/HotkeyText.h
    ${app_dir}/platform/IdleTrim.cpp ${app_dir}/platform/IdleTrim.h
    ${app_dir}/platform/Integrity.cpp ${app_dir}/platform/Integrity.h
    ${app_dir}/platform/PaddleOcr.cpp ${app_dir}/platform/PaddleOcr.h
    ${app_dir}/platform/OcrFiles.cpp ${app_dir}/platform/OcrFiles.h
    ${app_dir}/platform/ShellDialogs.cpp
    ${app_dir}/platform/WindowEffects.cpp ${app_dir}/platform/WindowEffects.h
    ${PROJECT_SOURCE_DIR}/src/core/DiagnosticLog.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Win32Util.cpp
)

set(qml_import_scan "")
if(QT6_IS_SHARED_LIBS_BUILD)
    set(qml_import_scan NO_IMPORT_SCAN) # as WinShun's (src/app/CMakeLists.txt)
endif()
qt_add_qml_module(WinShunCaptureApp
    URI WinShun
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/WinShun"
    VERSION 1.0
    NO_PLUGIN
    ${qml_import_scan}
    QML_FILES ${capture_qml}
    SOURCES
        CaptureSettings.cpp CaptureSettings.h
        ${app_dir}/SystemTheme.cpp ${app_dir}/SystemTheme.h
        ${app_dir}/PhosphorGlyph.cpp ${app_dir}/PhosphorGlyph.h
        ${app_dir}/WindowFrame.cpp ${app_dir}/WindowFrame.h
        ${app_dir}/HotkeyRule.h
        ../capture/Capture.cpp ../capture/Capture.h
        ../capture/CaptureImage.cpp ../capture/CaptureImage.h
        ../capture/CapturePointer.cpp ../capture/CapturePointer.h
        ../capture/AnnotationModel.cpp ../capture/AnnotationModel.h
        ../capture/AnnotationCanvas.cpp ../capture/AnnotationCanvas.h
        ../capture/CaptureSession.cpp ../capture/CaptureSession.h
        ../capture/CaptureEditor.cpp ../capture/CaptureEditor.h
        ../capture/CaptureOcr.cpp ../capture/CaptureOcr.h
        ../capture/LongCapture.cpp ../capture/LongCapture.h
        ../capture/Recorder.cpp ../capture/Recorder.h
        ../capture/PinManager.cpp ../capture/PinManager.h
        ../capture/PinWindow.cpp ../capture/PinWindow.h
        ../capture/Presenter.cpp ../capture/Presenter.h
        ../capture/LaserTrailItem.cpp ../capture/LaserTrailItem.h
)
# Bytecode only, no QML compiled to C++: that C++ calls QML's private
# interfaces, which change between Qt versions, and the plugin has to run on
# whichever Qt 6 the Win顺 it is installed into comes with.
set_target_properties(WinShunCaptureApp PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")

# English, as WinShun's (src/app/CMakeLists.txt): i18n/capture_en.ts, built
# into ":/i18n/capture_en.qm". After changing the capture's words:
# cmake --build --preset release --target update_translations.
qt_add_lupdate(WinShunCaptureApp LUPDATE_TARGET capture_lupdate
    TS_FILES i18n/capture_en.ts
    SOURCES ${PROJECT_SOURCE_DIR}/src/capture/AnnotationCommands.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/AnnotationFormat.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/AnnotationStyles.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/CaptureKeys.h
            ${PROJECT_SOURCE_DIR}/src/capture/CountdownWindow.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/Recording.cpp
            ${PROJECT_SOURCE_DIR}/src/capture/RecordingWorker.cpp
    OPTIONS -source-language zh_CN -no-obsolete -locations none)
set(qm_file "${CMAKE_CURRENT_BINARY_DIR}/capture_en.qm")
file(RELATIVE_PATH qm_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${qm_file}")
add_custom_command(OUTPUT "${qm_file}"
    COMMAND Qt6::lrelease -silent i18n/capture_en.ts -qm "${qm_relative}"
    DEPENDS i18n/capture_en.ts
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    VERBATIM)
qt_add_resources(WinShunCaptureApp "capture_translations"
    PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES "${qm_file}")

# The icons (Phosphor, see Phosphor.h): in WinShun.exe's resources there, so here too.
qt_add_resources(WinShunCaptureApp "capture_phosphor"
    PREFIX /fonts
    BASE "${PROJECT_SOURCE_DIR}/third_party/phosphor"
    FILES "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf"
          "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor-Fill.ttf")

target_include_directories(WinShunCaptureApp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${app_dir} ${app_dir}/platform ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(WinShunCaptureApp PRIVATE
    wscapture Qt6::Gui Qt6::Qml Qt6::Quick onnxruntime_headers
    advapi32 comctl32 d2d1 dwmapi dxgi gdi32 imm32 ole32 oleaut32 psapi shcore shell32 shlwapi user32 uxtheme windowscodecs
    mfplat mfreadwrite mfuuid delayimp)
target_link_options(WinShunCaptureApp PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
