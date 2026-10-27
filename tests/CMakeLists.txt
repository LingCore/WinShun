find_package(Qt6 6.8 REQUIRED COMPONENTS Test QuickTest)

qt_add_executable(tst_panelcolumns tst_panelcolumns.cpp ${PROJECT_SOURCE_DIR}/src/app/PanelColumns.cpp)
target_include_directories(tst_panelcolumns PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_panelcolumns PRIVATE wscore Qt6::Qml Qt6::Test)
set_target_properties(tst_panelcolumns PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_panelcolumns COMMAND tst_panelcolumns)
set_tests_properties(tst_panelcolumns PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_opusfocus tst_opusfocus.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp)
target_include_directories(tst_opusfocus PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_opusfocus PRIVATE winshun_options Qt6::Test oleacc oleaut32 ole32 user32)
set_target_properties(tst_opusfocus PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_opusfocus COMMAND tst_opusfocus)
set_tests_properties(tst_opusfocus PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_launchernavigation tst_launchernavigation.cpp)
target_include_directories(tst_launchernavigation PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_launchernavigation PRIVATE wscore Qt6::Test)
set_target_properties(tst_launchernavigation PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_launchernavigation COMMAND tst_launchernavigation)
set_tests_properties(tst_launchernavigation PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_wsl tst_wsl.cpp)
target_link_libraries(tst_wsl PRIVATE wscore Qt6::Test)
set_target_properties(tst_wsl PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_wsl COMMAND tst_wsl)
set_tests_properties(tst_wsl PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_displaygeometry tst_displaygeometry.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Placement.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_displaygeometry PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_displaygeometry PRIVATE winshun_options Qt6::Gui Qt6::Qml Qt6::Test
    comctl32 dwmapi ole32 shell32 user32 advapi32)
set_target_properties(tst_displaygeometry PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_displaygeometry COMMAND tst_displaygeometry -o -,txt)
set_tests_properties(tst_displaygeometry PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_filedialog tst_filedialog.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileDialog.cpp)
target_include_directories(tst_filedialog PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_filedialog PRIVATE wscore Qt6::Test ole32 oleaut32 uiautomationcore user32)
add_test(NAME tst_filedialog COMMAND tst_filedialog)
set_tests_properties(tst_filedialog PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Launch only a short-lived copy of this test, writing into QTemporaryDir.
qt_add_executable(tst_shelllaunch WIN32 tst_shelllaunch.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Shell.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileManagers.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Integrity.cpp)
target_include_directories(tst_shelllaunch PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_shelllaunch PRIVATE wscore Qt6::Gui Qt6::Test
    ole32 oleaut32 oleacc shell32 shlwapi user32 advapi32 taskschd uiautomationcore)
add_test(NAME tst_shelllaunch COMMAND tst_shelllaunch -o -,txt)
set_tests_properties(tst_shelllaunch PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_windowvisibility tst_windowvisibility.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_windowvisibility PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_windowvisibility PRIVATE winshun_options Qt6::Gui Qt6::Test comctl32 gdi32 dwmapi ole32 shell32 user32)
set_target_properties(tst_windowvisibility PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_windowvisibility COMMAND tst_windowvisibility)
set_tests_properties(tst_windowvisibility PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The real passive window lifecycle: render/GUI ordering, stale frames,
# native visibility, and hiding the separate logo when Qt closes the bar.
qt_add_executable(tst_dialogbarvisibility tst_dialogbarvisibility.cpp
    ${PROJECT_SOURCE_DIR}/src/app/DialogBarVisibility.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_dialogbarvisibility PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_dialogbarvisibility PRIVATE winshun_options Qt6::Quick Qt6::Test comctl32 gdi32 dwmapi ole32 shell32 user32)
set_target_properties(tst_dialogbarvisibility PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
foreach(renderer IN ITEMS gpu software)
    add_test(NAME tst_dialogbarvisibility_${renderer} COMMAND tst_dialogbarvisibility -o -,txt)
    set_tests_properties(tst_dialogbarvisibility_${renderer} PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()
set_tests_properties(tst_dialogbarvisibility_gpu PROPERTIES ENVIRONMENT "QSG_RHI_BACKEND=d3d11;QT_QUICK_BACKEND=")
set_tests_properties(tst_dialogbarvisibility_software PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")

# Shared by the launcher and clipboard: only a current opening's fresh
# frame may reveal it, and a stale frame cannot cancel its fallback.
qt_add_executable(tst_framereveal tst_framereveal.cpp ${PROJECT_SOURCE_DIR}/src/app/FrameReveal.cpp)
target_include_directories(tst_framereveal PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_framereveal PRIVATE winshun_options Qt6::Quick Qt6::Test)
set_target_properties(tst_framereveal PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
foreach(renderer IN ITEMS gpu software)
    add_test(NAME tst_framereveal_${renderer} COMMAND tst_framereveal -o -,txt)
    set_tests_properties(tst_framereveal_${renderer} PROPERTIES
        RUN_SERIAL TRUE
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()
set_tests_properties(tst_framereveal_gpu PROPERTIES ENVIRONMENT "QSG_RHI_BACKEND=d3d11;QT_QUICK_BACKEND=")
set_tests_properties(tst_framereveal_software PROPERTIES ENVIRONMENT "QT_QUICK_BACKEND=software")

qt_add_executable(tst_scrollalignment tst_scrollalignment.cpp)
target_compile_definitions(tst_scrollalignment PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
target_link_libraries(tst_scrollalignment PRIVATE Qt6::Quick Qt6::Test Qt6::QuickTest)
set_target_properties(tst_scrollalignment PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE WIN32_EXECUTABLE FALSE)
foreach(scale IN ITEMS 1 1.25 1.5 1.75 2)
    add_test(NAME tst_scrollalignment_${scale} COMMAND tst_scrollalignment -o -,txt)
    set_tests_properties(tst_scrollalignment_${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${scale}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endforeach()

qt_add_executable(tst_numberhints tst_numberhints.cpp ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp)
target_include_directories(tst_numberhints PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_numberhints PRIVATE wscore Qt6::Quick Qt6::Test)
target_compile_definitions(tst_numberhints PRIVATE WINSHUN_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(tst_numberhints PROPERTIES WIN32_EXECUTABLE FALSE)
if(QT6_IS_SHARED_LIBS_BUILD)
    set_target_properties(tst_numberhints PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
add_test(NAME tst_numberhints COMMAND tst_numberhints)
set_tests_properties(tst_numberhints PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The settings AI may read and change (src/app/AiSettings): the catalog's
# validation, what is never writable, and every writable entry through a
# save and a load.
qt_add_executable(tst_aisettings tst_aisettings.cpp
    ${PROJECT_SOURCE_DIR}/src/app/AiSettings.cpp ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp)
target_include_directories(tst_aisettings PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_aisettings PRIVATE wscore Qt6::Gui Qt6::Test)
set_target_properties(tst_aisettings PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_aisettings COMMAND tst_aisettings)
set_tests_properties(tst_aisettings PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_recording tst_recording.cpp)
target_link_libraries(tst_recording PRIVATE wscapture Qt6::Test)
set_target_properties(tst_recording PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_recording COMMAND tst_recording)
set_tests_properties(tst_recording PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_settings tst_settings.cpp ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp)
target_include_directories(tst_settings PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_settings PRIVATE wscore Qt6::Gui Qt6::Test)
set_target_properties(tst_settings PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_settings COMMAND tst_settings)
set_tests_properties(tst_settings PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The screen capture's geometry, HDR mapping and state machine (src/capture),
# without windows.
qt_add_executable(tst_capture tst_capture.cpp ${PROJECT_SOURCE_DIR}/src/capture/CaptureSession.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/AnnotationModel.cpp)
target_link_libraries(tst_capture PRIVATE wscapture Qt6::Qml Qt6::Test)
# Qt6::Qml only for QML_ELEMENT's macros: no QML imports to look for.
set_target_properties(tst_capture PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_capture COMMAND tst_capture)
set_tests_properties(tst_capture PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Annotating a screenshot (src/capture/Annotation*): geometry, crisp strokes,
# tiles equal to the whole picture, the command stack, numbers, redactions
# that cannot be undone, the project format with its v1 sample, the overlay
# turned into a project, the remembered styles and the tool table.
qt_add_executable(tst_annotate tst_annotate.cpp)
target_link_libraries(tst_annotate PRIVATE wscapture Qt6::Test)
target_compile_definitions(tst_annotate PRIVATE WINSHUN_ANNOTATE_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data/annotate")
set_target_properties(tst_annotate PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_annotate COMMAND tst_annotate)
set_tests_properties(tst_annotate PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Smart redaction's rules (src/capture/SmartRedact): numbers with their check
# digits, keys, placeholders, names, boxes from the characters' places.
qt_add_executable(tst_smartredact tst_smartredact.cpp)
target_link_libraries(tst_smartredact PRIVATE wscapture Qt6::Test)
set_target_properties(tst_smartredact PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_smartredact COMMAND tst_smartredact)
set_tests_properties(tst_smartredact PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Stage 7 (src/capture: PinTools, CaptureTools, QrCode): a pin's turns,
# collapsed forms, corner drags and picked text, the colour formats, the
# delayed capture's countdown, Shift+R's monitor, the cursor drawn into a
# picture, the rules for UI Automation, and QR codes made by zxing-cpp's writer.
qt_add_executable(tst_stage7 tst_stage7.cpp)
target_link_libraries(tst_stage7 PRIVATE wscapture ZXing::ZXing Qt6::Test)
set_target_properties(tst_stage7 PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_stage7 COMMAND tst_stage7)
set_tests_properties(tst_stage7 PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The long screenshot's stitching (src/capture/ScrollStitcher) replayed on a
# made-up long page cut into frames, and its diagnostics' text.
qt_add_executable(tst_stitch tst_stitch.cpp)
target_link_libraries(tst_stitch PRIVATE wscapture Qt6::Test)
set_target_properties(tst_stitch PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_stitch COMMAND tst_stitch)
set_tests_properties(tst_stitch PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The editing window's controller (src/capture/CaptureEditor): the zoom and
# where the picture lies, the mouse as image pixels, the ways out, Esc, the
# crop, the window emptying on close; and a pin's new picture (PinWindow).
qt_add_executable(tst_editor tst_editor.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/CaptureEditor.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/AnnotationModel.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/AnnotationCanvas.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/PinWindow.cpp)
target_link_libraries(tst_editor PRIVATE wscapture Qt6::Qml Qt6::Quick Qt6::Test imm32)
set_target_properties(tst_editor PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_editor COMMAND tst_editor)

set_tests_properties(tst_editor PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The overlay's annotations (AnnotationModel, AnnotationCanvas): every tool
# drawn with the mouse, undo and redo, the keys, numbers, text, the style
# panel and the wheel; the canvas's tiles grabbed from an offscreen window on
# the software scene graph, equal to the compositor's pixels.
qt_add_executable(tst_annotmodel tst_annotmodel.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/AnnotationModel.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/AnnotationCanvas.cpp)
target_link_libraries(tst_annotmodel PRIVATE wscapture Qt6::Qml Qt6::Quick Qt6::Test)
# Qt6::Qml only for QML_ELEMENT's macros: no QML imports to look for.
set_target_properties(tst_annotmodel PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_annotmodel COMMAND tst_annotmodel)
set_tests_properties(tst_annotmodel PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_appearance tst_appearance.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_appearance PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(tst_appearance PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
target_link_libraries(tst_appearance PRIVATE wscore Qt6::Test Qt6::Quick comctl32 gdi32 dwmapi ole32 shell32 user32)
get_target_property(appearance_qt_type Qt6::Core TYPE)
if(appearance_qt_type STREQUAL "SHARED_LIBRARY")
    set_target_properties(tst_appearance PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
foreach(scale IN ITEMS 1 1.25 1.5 2)
    add_test(NAME tst_appearance_${scale} COMMAND tst_appearance)
    set_tests_properties(tst_appearance_${scale} PROPERTIES
        ENVIRONMENT "QT_QUICK_BACKEND=software;QT_ENABLE_HIGHDPI_SCALING=0;QT_SCALE_FACTOR=${scale}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()

qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE wscore wsextract miniz Qt6::Test)

qt_add_executable(tst_diagnostics tst_diagnostics.cpp ${PROJECT_SOURCE_DIR}/src/app/Diagnostics.cpp
    ${PROJECT_SOURCE_DIR}/src/service/Log.cpp)
target_include_directories(tst_diagnostics PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/service)
target_link_libraries(tst_diagnostics PRIVATE wscore miniz Qt6::Test)
add_test(NAME tst_diagnostics COMMAND tst_diagnostics)
set_tests_properties(tst_diagnostics PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_clipboard tst_clipboard.cpp)
target_link_libraries(tst_clipboard PRIVATE wscore Qt6::Test)

# Image codecs, persistence, real Win32 clipboard round trips on a private
# window station, and the production QML preview (offscreen).
qt_add_executable(tst_clipimage tst_clipimage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipFiles.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ColorSwatch.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ClipboardWatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_clipimage PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(tst_clipimage PRIVATE
    WINSHUN_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data"
    WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
target_link_libraries(tst_clipimage PRIVATE wscore Qt6::Quick Qt6::Test user32 gdi32 comctl32 shell32 ole32 version dwmapi)
set_target_properties(tst_clipimage PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_clipimage COMMAND tst_clipimage -o -,txt)
set_tests_properties(tst_clipimage PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Native worker lifecycle only: never writes the system clipboard or loads OCR models.
qt_add_executable(tst_cliplifecycle tst_cliplifecycle.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ClipboardWatcher.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/TextRecognizer.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/PaddleOcr.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipImage.cpp)
target_include_directories(tst_cliplifecycle PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_cliplifecycle PRIVATE winshun_options wscore wscapture Qt6::Gui Qt6::Test
    onnxruntime_headers windowsapp user32 gdi32 shell32 ole32 version)
set_target_properties(tst_cliplifecycle PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_cliplifecycle COMMAND tst_cliplifecycle -o -,txt)
set_tests_properties(tst_cliplifecycle PROPERTIES RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The index service (src/service) through its pipe client (ServiceVolumeAccess),
# started as `WinShunService.exe --console --pipe <random>` on a real volume
# (as administrator): the MFT the same as read directly, the journal
# followed, volumes let go of, the service gone and back, the handshake.
qt_add_executable(tst_service tst_service.cpp)
target_link_libraries(tst_service PRIVATE wscore Qt6::Test)
target_compile_definitions(tst_service PRIVATE WINSHUN_SERVICE_EXE="$<TARGET_FILE:WinShunService>")
add_dependencies(tst_service WinShunService)

qt_add_executable(tst_keylistener tst_keylistener.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/KeyListener.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ServiceKeyListener.cpp)
target_include_directories(tst_keylistener PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_keylistener PRIVATE wscore Qt6::Test user32 advapi32 shcore)
target_compile_definitions(tst_keylistener PRIVATE WINSHUN_SERVICE_EXE="$<TARGET_FILE:WinShunService>")
add_dependencies(tst_keylistener WinShunService)

qt_add_executable(tst_keyrouter tst_keyrouter.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/KeyRouter.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Diagnostics.cpp)
target_include_directories(tst_keyrouter PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_keyrouter PRIVATE wscore miniz Qt6::Test user32 imm32)
set_target_properties(tst_keyrouter PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_keyrouter COMMAND tst_keyrouter -o -,txt)
set_tests_properties(tst_keyrouter PROPERTIES RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The drop pie (DropPie): its chips round the middle, the direction that
# chooses one, and the folders' names shortened to fit.
qt_add_executable(tst_piegeometry tst_piegeometry.cpp ${PROJECT_SOURCE_DIR}/src/app/PieGeometry.cpp)
target_include_directories(tst_piegeometry PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_piegeometry PRIVATE winshun_options Qt6::Core Qt6::Test)
qt_add_executable(tst_pielabels tst_pielabels.cpp ${PROJECT_SOURCE_DIR}/src/app/PieLabels.cpp)
target_include_directories(tst_pielabels PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_pielabels PRIVATE winshun_options Qt6::Gui Qt6::Test)
# 粘贴为文件 (core/PasteFile): which clipboards Ctrl+V takes, the kind, CSV, names.
qt_add_executable(tst_pastefile tst_pastefile.cpp ${PROJECT_SOURCE_DIR}/src/core/PasteFile.cpp)
target_include_directories(tst_pastefile PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_pastefile PRIVATE winshun_options Qt6::Core Qt6::Test)
foreach(test IN ITEMS tst_piegeometry tst_pielabels tst_pastefile)
    set_target_properties(${test} PROPERTIES WIN32_EXECUTABLE FALSE)
    add_test(NAME ${test} COMMAND ${test})
    set_tests_properties(${test} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()

# The favorites: their file, the store, and the search's list with nothing typed.
qt_add_executable(tst_favorites tst_favorites.cpp)
target_link_libraries(tst_favorites PRIVATE wscore Qt6::Test)

# The actual shell/image-provider path: resolve commands, decode custom
# pictures and fall back immediately for offline programs. Never executes a command.
qt_add_executable(tst_commandicons tst_commandicons.cpp
    ${PROJECT_SOURCE_DIR}/src/app/FileIconProvider.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SiteIconImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SvgIcon.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/SiteIcons.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Shell.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileManagers.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Integrity.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_commandicons PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_commandicons PRIVATE wscore wsplugin Qt6::Quick Qt6::Svg Qt6::Test
    comctl32 gdi32 dwmapi ole32 oleaut32 oleacc shell32 shlwapi user32 advapi32 taskschd uiautomationcore winhttp)
set_target_properties(tst_commandicons PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_commandicons COMMAND tst_commandicons)
set_tests_properties(tst_commandicons PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Colour values as text: app code that needs nothing but QColor.
qt_add_executable(tst_color tst_color.cpp ${PROJECT_SOURCE_DIR}/src/app/ColorText.cpp)
target_include_directories(tst_color PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_color PRIVATE Qt6::Gui Qt6::Test)

# Long names and paths shortened for a row: extension, end and matches kept.
qt_add_executable(tst_elide tst_elide.cpp ${PROJECT_SOURCE_DIR}/src/app/Elide.cpp)
target_include_directories(tst_elide PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_elide PRIVATE Qt6::Gui Qt6::Test)

# The diagnostics' glyph self-check (GitHub #70): clean in the fonts drawn
# with, a font's glyph numbers drawn with another reported; the display part
# of an export's summary.
qt_add_executable(tst_textcheck tst_textcheck.cpp ${PROJECT_SOURCE_DIR}/src/app/TextCheck.cpp
    ${PROJECT_SOURCE_DIR}/src/app/DisplayDiagnostics.cpp)
target_include_directories(tst_textcheck PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(tst_textcheck PRIVATE WINSHUN_FONTS_DIR="${PROJECT_SOURCE_DIR}/resources/fonts")
target_link_libraries(tst_textcheck PRIVATE wscore Qt6::Quick Qt6::Test)
set_target_properties(tst_textcheck PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_textcheck COMMAND tst_textcheck)
# And in the font engines the app runs with (main.cpp): 显卡加速 FreeType, 省内存 GDI.
add_test(NAME tst_textcheck_freetype COMMAND tst_textcheck)
add_test(NAME tst_textcheck_gdi COMMAND tst_textcheck)
set_tests_properties(tst_textcheck tst_textcheck_freetype tst_textcheck_gdi PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
set_tests_properties(tst_textcheck_freetype PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows:fontengine=freetype")
set_tests_properties(tst_textcheck_gdi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows:fontengine=gdi")

# Which side of a box (the text caret, a search box) the clipboard window
# opens on, as Placement decides it (BoxSide.h, GitHub #38).
qt_add_executable(tst_placement tst_placement.cpp)
target_include_directories(tst_placement PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_placement PRIVATE winshun_options Qt6::Core Qt6::Test)

# Backups of the settings: what goes in, unchanged ones skipped, old ones
# removed, and restoring (a path leaving the folder refused).
qt_add_executable(tst_backup tst_backup.cpp ${PROJECT_SOURCE_DIR}/src/app/BackupFiles.cpp)
target_include_directories(tst_backup PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_backup PRIVATE winshun_options miniz Qt6::Core Qt6::Test)

# Sites' icons as read from what a site sends (PNG, ICO), damaged ones too;
# SVG icons (SvgIcon) drawn, and those naming anything outside turned away.
qt_add_executable(tst_siteicon tst_siteicon.cpp ${PROJECT_SOURCE_DIR}/src/app/SiteIconImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SvgIcon.cpp)
target_include_directories(tst_siteicon PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_siteicon PRIVATE Qt6::Gui Qt6::Svg Qt6::Test)

# The words in pictures: PP-OCRv6 through ONNX Runtime, the real models.
qt_add_executable(tst_ocr tst_ocr.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/PaddleOcr.cpp)
target_include_directories(tst_ocr PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_ocr PRIVATE winshun_options wscapture Qt6::Gui Qt6::Test onnxruntime_headers)

# What a drop target gets when results are dragged out of Win顺.
qt_add_executable(tst_dragout tst_dragout.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp)
target_include_directories(tst_dragout PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_dragout PRIVATE winshun_options Qt6::Core Qt6::Test)

# The system switches' cache (Switches): toggles turned into On and Off, writes
# coalesced, failures, changes made elsewhere, through a fake backend on the
# real switch thread. Nothing of the system is read or changed.
qt_add_executable(tst_switches tst_switches.cpp ${PROJECT_SOURCE_DIR}/src/app/Switches.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Switches.h ${PROJECT_SOURCE_DIR}/src/app/platform/ComWorker.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/SwitchKeepAwake.cpp) # keep awake is built in, over a fake clock
target_include_directories(tst_switches PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_switches PRIVATE wscore Qt6::Test ole32 powrprof)
set_target_properties(tst_switches PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_switches COMMAND tst_switches)
set_tests_properties(tst_switches PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The MCP server (src/mcp): the protocol, the tools on a hand-made index, the
# pipe, and the relay WinShunMcp.exe, whose path it is told; and the
# plugins' commands as tools (plugin::AiBridge), with plugin_fake.exe through
# the runner as in tst_plugin.
qt_add_executable(tst_mcp tst_mcp.cpp)
target_link_libraries(tst_mcp PRIVATE wsmcp wsplugin Qt6::Test)
target_compile_definitions(tst_mcp PRIVATE
    WINSHUN_MCP_RELAY="$<TARGET_FILE:WinShunMcp>"
    WINSHUN_PLUGIN_HOST="$<TARGET_FILE:WinShunPluginHost>"
    WINSHUN_PLUGIN_FAKE="$<TARGET_FILE:plugin_fake>")
add_dependencies(tst_mcp WinShunMcp WinShunPluginHost plugin_fake)

# The pieces of the tools plugin that compute (src/tools-plugin), built in
# again: the calculator and the conversions have edges (division by zero,
# overflow, bad input) worth pinning down.
qt_add_executable(tst_tools tst_tools.cpp
    ${PROJECT_SOURCE_DIR}/src/tools-plugin/Calc.cpp ${PROJECT_SOURCE_DIR}/src/tools-plugin/Units.cpp
    ${PROJECT_SOURCE_DIR}/src/tools-plugin/Color.cpp ${PROJECT_SOURCE_DIR}/src/tools-plugin/Codec.cpp)
target_include_directories(tst_tools PRIVATE ${PROJECT_SOURCE_DIR}/src/tools-plugin)
target_link_libraries(tst_tools PRIVATE winshun_options Qt6::Test bcrypt)

# winshun.semantic's pieces (src/semantic-plugin), built in again: passages,
# the vector store, the model services' settings.
qt_add_executable(tst_semantic tst_semantic.cpp
    ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Chunker.cpp ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Store.cpp
    ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Embedder.cpp)
target_include_directories(tst_semantic PRIVATE ${PROJECT_SOURCE_DIR}/src/semantic-plugin ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(tst_semantic PRIVATE winshun_options Qt6::Test winhttp)

# The plugin system (src/plugin): manifests, items, zero-code, the catalog,
# and plugin processes started through the runner WinShunPluginHost.exe,
# played by plugin_fake.exe (no Qt, as a plugin would be); both paths are
# told to the test.
add_executable(plugin_fake plugin_fake.cpp)
target_link_libraries(plugin_fake PRIVATE wsplugin_lite)
set_target_properties(plugin_fake PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
qt_add_executable(tst_plugin tst_plugin.cpp)
target_link_libraries(tst_plugin PRIVATE wsplugin miniz Qt6::Test) # miniz: the test writes zips to install
target_compile_definitions(tst_plugin PRIVATE
    WINSHUN_PLUGIN_HOST="$<TARGET_FILE:WinShunPluginHost>"
    WINSHUN_PLUGIN_FAKE="$<TARGET_FILE:plugin_fake>"
    WINSHUN_TEST_PLUGINS="${CMAKE_CURRENT_SOURCE_DIR}/plugins")
add_dependencies(tst_plugin WinShunPluginHost plugin_fake)

add_test(NAME tst_core COMMAND tst_core)
add_test(NAME tst_service COMMAND tst_service)
add_test(NAME tst_keylistener COMMAND tst_keylistener)
add_test(NAME tst_clipboard COMMAND tst_clipboard)
add_test(NAME tst_favorites COMMAND tst_favorites)
add_test(NAME tst_color COMMAND tst_color)
add_test(NAME tst_elide COMMAND tst_elide)
add_test(NAME tst_placement COMMAND tst_placement)
add_test(NAME tst_dragout COMMAND tst_dragout)
add_test(NAME tst_siteicon COMMAND tst_siteicon)
add_test(NAME tst_backup COMMAND tst_backup)
add_test(NAME tst_ocr COMMAND tst_ocr)
add_test(NAME tst_tools COMMAND tst_tools)
add_test(NAME tst_semantic COMMAND tst_semantic)
add_test(NAME tst_mcp COMMAND tst_mcp)
add_test(NAME tst_plugin COMMAND tst_plugin)
# Importing clipboard history (Ditto, text, CSV): the reader with a real store.
qt_add_executable(tst_clipimport tst_clipimport.cpp ${PROJECT_SOURCE_DIR}/src/app/ClipImport.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipImportJob.cpp)
target_include_directories(tst_clipimport PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_clipimport PRIVATE wscore Qt6::Gui Qt6::Test)
target_compile_definitions(tst_clipimport PRIVATE WINSHUN_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME tst_clipimport COMMAND tst_clipimport)
set_tests_properties(tst_clipimport PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# A native file picker, enabled explicitly with WINSHUN_TEST_NATIVE_DIALOG=1.
qt_add_executable(tst_filepaste tst_filepaste.cpp ${PROJECT_SOURCE_DIR}/src/app/ClipFiles.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileDialog.cpp)
target_include_directories(tst_filepaste PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_filepaste PRIVATE wscore Qt6::Test)
set_target_properties(tst_filepaste PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_filepaste COMMAND tst_filepaste)
set_tests_properties(tst_filepaste PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Image exports and upload file names, with a real store and Shell data object.
qt_add_executable(tst_clipfiles tst_clipfiles.cpp ${PROJECT_SOURCE_DIR}/src/app/ClipFiles.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp)
target_include_directories(tst_clipfiles PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_clipfiles PRIVATE wscore Qt6::Gui Qt6::Test)
set_target_properties(tst_clipfiles PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_clipfiles COMMAND tst_clipfiles)
set_tests_properties(tst_clipfiles PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Picking the whole clipboard list (Ctrl+A, delete all): ClipModel with a real store.
qt_add_executable(tst_clipselect tst_clipselect.cpp ${PROJECT_SOURCE_DIR}/src/app/ClipModel.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ColorText.cpp)
target_include_directories(tst_clipselect PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_clipselect PRIVATE wscore Qt6::Quick Qt6::Test)
set_target_properties(tst_clipselect PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_clipselect COMMAND tst_clipselect)
set_tests_properties(tst_clipselect PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# Let ctest find the Qt DLLs without a deploy step.
set_tests_properties(tst_core tst_service tst_keylistener tst_clipboard tst_favorites tst_backup tst_color tst_elide tst_placement tst_dragout tst_siteicon tst_ocr tst_tools tst_semantic tst_mcp tst_plugin PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The in-app update (Updater): the update service's answer, downloads from
# mirrors then GitHub checked by size and SHA-256 (a fake server on
# 127.0.0.1), the version resource, unpacking a portable zip safely, and
# WinShunUpdate.exe's swap of a portable copy's files (data kept, rolled back).
qt_add_executable(tst_updater tst_updater.cpp ${PROJECT_SOURCE_DIR}/src/app/UpdatePackage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp ${PROJECT_SOURCE_DIR}/src/updater/Swap.cpp)
target_include_directories(tst_updater PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/updater)
target_link_libraries(tst_updater PRIVATE wscore miniz Qt6::Test version winhttp ws2_32)
target_compile_definitions(tst_updater PRIVATE WINSHUN_UPDATE_EXE="$<TARGET_FILE:WinShunUpdate>")
set_target_properties(tst_updater PROPERTIES WIN32_EXECUTABLE FALSE)
add_dependencies(tst_updater WinShunUpdate)
add_test(NAME tst_updater COMMAND tst_updater)
set_tests_properties(tst_updater PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
