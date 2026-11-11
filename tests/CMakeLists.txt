find_package(Qt6 6.8 REQUIRED COMPONENTS Test QuickTest)

qt_add_executable(tst_windowsearch tst_windowsearch.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/WindowSearch.cpp)
target_include_directories(tst_windowsearch PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_windowsearch PRIVATE wscore Qt6::Test user32 dwmapi)
set_target_properties(tst_windowsearch PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_windowsearch COMMAND tst_windowsearch -o -,txt)
set_tests_properties(tst_windowsearch PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_queryhistory tst_queryhistory.cpp)
target_link_libraries(tst_queryhistory PRIVATE wscore Qt6::Test)
qt_add_executable(tst_pluginreadme tst_pluginreadme.cpp ${PROJECT_SOURCE_DIR}/src/app/PluginReadme.cpp)
target_include_directories(tst_pluginreadme PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_pluginreadme PRIVATE winshun_options Qt6::Gui Qt6::Test)
foreach(test IN ITEMS tst_queryhistory tst_pluginreadme)
    set_target_properties(${test} PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
    add_test(NAME ${test} COMMAND ${test} -o -,txt)
    set_tests_properties(${test} PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()

qt_add_executable(tst_paster tst_paster.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/Paster.cpp)
target_include_directories(tst_paster PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_paster PRIVATE winshun_options Qt6::Test user32 comctl32)
set_target_properties(tst_paster PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_paster COMMAND tst_paster -o -,txt)
set_tests_properties(tst_paster PROPERTIES RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The paste the index service presses in an administrator's window (GitHub
# #277): the keys, and the request (its live part needs the elevated harness).
qt_add_executable(tst_servicepaste tst_servicepaste.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/ServicePaste.cpp)
target_include_directories(tst_servicepaste PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_servicepaste PRIVATE wscore Qt6::Test user32)
set_target_properties(tst_servicepaste PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_servicepaste COMMAND tst_servicepaste)
set_tests_properties(tst_servicepaste PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The request that takes an administrator's file dialog to a folder (GitHub
# #104, #143): with no service; its live part needs the elevated harness.
qt_add_executable(tst_servicedialognav tst_servicedialognav.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/ServiceDialogNav.cpp)
target_include_directories(tst_servicedialognav PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_servicedialognav PRIVATE wscore Qt6::Test user32)
set_target_properties(tst_servicedialognav PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_servicedialognav COMMAND tst_servicedialognav)
set_tests_properties(tst_servicedialognav PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_pluginhotkeys tst_pluginhotkeys.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/PluginHotkeys.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/PlainHotkeys.cpp)
target_include_directories(tst_pluginhotkeys PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_pluginhotkeys PRIVATE winshun_options Qt6::Test user32)
set_target_properties(tst_pluginhotkeys PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_pluginhotkeys COMMAND tst_pluginhotkeys -o -,txt)
set_tests_properties(tst_pluginhotkeys PROPERTIES RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The OCR worker stub loads Qt, its plugins and QML imports, ONNX Runtime and
# the models from Win顺's folder alone (no PATH: ocr-search-plugin/stub), so
# the tests that start it get a deployed Win顺 folder, not the build folder
# (which has no Qt: the process could not start).
set(ocr_app_dir "${CMAKE_CURRENT_BINARY_DIR}/ocr-app")
set(ocr_app_stamp "${ocr_app_dir}/deployed.stamp")
add_custom_command(OUTPUT "${ocr_app_stamp}"
    COMMAND Qt6::windeployqt --dir "${ocr_app_dir}" --qmldir "${PROJECT_SOURCE_DIR}/src" --no-translations
        --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
        --skip-plugin-types qmltooling,generic,networkinformation,tls,iconengines
        --exclude-plugins qsqlibase,qsqlmimer,qsqloci,qsqlodbc,qsqlpsql
        $<IF:$<CONFIG:Debug>,--debug,--release> "$<TARGET_FILE:WinShunOcrSearchApp>"
    # WinShun.exe: the plugin window takes its icon from it.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WinShun>" "${ORT_DIR}/lib/onnxruntime.dll"
        "$<TARGET_FILE:pdfium>" "${ocr_app_dir}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ocr_app_dir}/ocr"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OCR_MODEL_FILES} "${ocr_app_dir}/ocr"
    COMMAND ${CMAKE_COMMAND} -E touch "${ocr_app_stamp}"
    DEPENDS WinShunOcrSearchApp WinShun ${OCR_MODEL_FILES}
    COMMENT "Deploying a Win顺 folder for the OCR worker tests into ${ocr_app_dir}"
    VERBATIM)
add_custom_target(tst_ocrsearch_app DEPENDS "${ocr_app_stamp}")

qt_add_executable(tst_ocrsearch tst_ocrsearch.cpp)
target_link_libraries(tst_ocrsearch PRIVATE wsocrsearch wsplugin miniz Qt6::Test)
target_compile_definitions(tst_ocrsearch PRIVATE WINSHUN_OCR_MANIFEST="${OCR_PLUGIN_MANIFEST}"
    WINSHUN_OCR_APP_DIR="${ocr_app_dir}" WINSHUN_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_dependencies(tst_ocrsearch WinShunOcrSearch WinShunOcrSearchApp WinShunPluginHost tst_ocrsearch_app)
set_target_properties(tst_ocrsearch PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_ocrsearch COMMAND tst_ocrsearch -o -,txt)
set_tests_properties(tst_ocrsearch PROPERTIES TIMEOUT 180
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_contentdetails tst_contentdetails.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ContentReportPrint.cpp)
target_include_directories(tst_contentdetails PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_contentdetails PRIVATE wscore Qt6::Test miniz comdlg32 gdi32 ole32)
set_target_properties(tst_contentdetails PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_contentdetails COMMAND tst_contentdetails -o -,txt)
set_tests_properties(tst_contentdetails PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
add_test(NAME tst_contentpane COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/content-pane" --content-pane --renderer d3d11)
set_tests_properties(tst_contentpane PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# #111: the settings window keeps its Screen attached objects few; each one is
# work on every move to another monitor (docs/pitfalls.md).
add_test(NAME tst_settingsscreen COMMAND $<TARGET_FILE:WinShun>
    --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/settings-screen" --screen-objects)
set_tests_properties(tst_settingsscreen PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# #163: the settings page header stays; rows reached by search, scrolling or Tab land below it.
add_test(NAME tst_settingsscroll COMMAND $<TARGET_FILE:WinShun>
    --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/settings-scroll" --page-scroll)
set_tests_properties(tst_settingsscroll PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# #230: a plugin's 管理员 确认 that fails says why on its card, with 再试一次 there.
add_test(NAME tst_pluginconfirm COMMAND $<TARGET_FILE:WinShun>
    --preview-plugins "${CMAKE_CURRENT_BINARY_DIR}/plugin-confirm.png" --elevated --confirm)
set_tests_properties(tst_pluginconfirm PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# #248: an entry of the plugin library offers 主页 beside its name where the
# list gives an address of the web, and not where it gives none or something
# else (a file, a program); its tip would name where it leads; it is faint
# until the pointer is on it. The pointer is put over one, never pressed.
add_test(NAME tst_pluginhomepage COMMAND $<TARGET_FILE:WinShun>
    --preview-plugins "${CMAKE_CURRENT_BINARY_DIR}/plugin-homepage.png" --homepage-check --scale 1.5)
set_tests_properties(tst_pluginhomepage PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# A command's keywords typed in its card (GitHub #282): several words, a
# quote refused with the reason under the box, the plugin's own typed again,
# none, and 默认, each followed to the INI and to the catalog's index.
add_test(NAME tst_pluginkeywords COMMAND $<TARGET_FILE:WinShun>
    --preview-plugins "${CMAKE_CURRENT_BINARY_DIR}/plugin-keywords.png" --keyword-check --scale 1.5)
set_tests_properties(tst_pluginkeywords PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Combo menus share the theme frame, corner setting, inner padding and control gap.
foreach(combo_style IN ITEMS fluent brand tactile liquid)
    foreach(combo_corners IN ITEMS round square)
        set(combo_test "tst_combopopup_${combo_style}_${combo_corners}")
        add_test(NAME ${combo_test} COMMAND $<TARGET_FILE:WinShun>
            --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/${combo_test}" --combo-popup
            --style ${combo_style} --corners ${combo_corners} --dark --scale 1.5 --renderer d3d11)
        set_tests_properties(${combo_test} PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
endforeach()

# #235: the fourth style is reachable, saved and keyboard-operable in the
# actual settings view. Controls also run against the production launcher.
foreach(liquid_mode IN ITEMS light dark square)
    set(liquid_args --style liquid --renderer d3d11)
    if(NOT liquid_mode STREQUAL "light")
        list(APPEND liquid_args --dark --scale 1.5)
    endif()
    if(liquid_mode STREQUAL "square")
        list(APPEND liquid_args --corners square)
    endif()
    add_test(NAME tst_liquid_buttons_${liquid_mode} COMMAND $<TARGET_FILE:WinShun>
        --preview-results "${CMAKE_CURRENT_BINARY_DIR}/liquid-buttons-${liquid_mode}" --style-buttons ${liquid_args})
    add_test(NAME tst_liquid_picker_${liquid_mode} COMMAND $<TARGET_FILE:WinShun>
        --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/liquid-picker-${liquid_mode}" --style-picker ${liquid_args})
    set_tests_properties(tst_liquid_buttons_${liquid_mode} tst_liquid_picker_${liquid_mode} PROPERTIES
        RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()

foreach(glass_mode IN ITEMS light dark_english)
    set(glass_args --style liquid --renderer d3d11)
    if(glass_mode STREQUAL "dark_english")
        list(APPEND glass_args --dark --english --scale 1.5)
    endif()
    add_test(NAME tst_glass_controls_${glass_mode} COMMAND $<TARGET_FILE:WinShun>
        --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/glass-controls-${glass_mode}" --glass-controls ${glass_args})
    set_tests_properties(tst_glass_controls_${glass_mode} PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endforeach()

# The screenshot hotkey row and its 按键一览 entry follow the capture plugin's state.
add_test(NAME tst_settingscapturehotkey COMMAND $<TARGET_FILE:WinShun>
    --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/settings-capture-hotkey" --capture-hotkey --scale 1.5)
set_tests_properties(tst_settingscapturehotkey PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The settings window is kept once made: closed, it hides and comes back as new
# (first page, no search, no dialogs, no recording, nothing running while
# hidden), and the ways in from elsewhere still land; the real App, isolated
# (--probe-runtime), shown without activation.
add_test(NAME tst_settingsreopen COMMAND $<TARGET_FILE:WinShun>
    --probe-runtime "${CMAKE_CURRENT_BINARY_DIR}/settings-reopen" --settings-reopen-check --settle-ms 1500
    --renderer d3d11)
set_tests_properties(tst_settingsreopen PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The settings pages are made when needed: opening makes the page on show only,
# each way in from elsewhere its own, a word typed all of them (results in page
# order, as many as before), and after the first frame the rest, one per tick.
add_test(NAME tst_settingslazy COMMAND $<TARGET_FILE:WinShun>
    --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/settings-lazy" --lazy-pages)
set_tests_properties(tst_settingslazy PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# The same probe as tst_settingsreopen, first opened on the 剪贴板 page (as
# App::showClipboardSettings opens it): that page alone made for the first frame.
add_test(NAME tst_settingsopenlink COMMAND $<TARGET_FILE:WinShun>
    --probe-runtime "${CMAKE_CURRENT_BINARY_DIR}/settings-open-link" --settings-reopen-check --settle-ms 1500
    --renderer d3d11 --settings-first-open clipboard --settings-opens 1)
set_tests_properties(tst_settingsopenlink PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Clipboard sizing uses the production QML/native frame with temporary history.
add_test(NAME tst_clipboardsizing COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/clipboard-sizing" --clipboard-sizing --renderer d3d11)
set_tests_properties(tst_clipboardsizing PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_querysyntax tst_querysyntax.cpp)
target_link_libraries(tst_querysyntax PRIVATE wscore Qt6::Test)
set_target_properties(tst_querysyntax PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_querysyntax COMMAND tst_querysyntax)
set_tests_properties(tst_querysyntax PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

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

qt_add_executable(tst_explorerview tst_explorerview.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp)
target_include_directories(tst_explorerview PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_explorerview PRIVATE winshun_options Qt6::Test oleacc oleaut32 ole32 user32)
set_target_properties(tst_explorerview PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_explorerview COMMAND tst_explorerview)
set_tests_properties(tst_explorerview PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_previewplacement tst_previewplacement.cpp)
add_test(NAME tst_previewinteractions COMMAND WinShun --preview-results
    "${CMAKE_CURRENT_BINARY_DIR}/preview-interactions" --preview-interactions)
add_test(NAME tst_previewinteractions_scaled COMMAND WinShun --preview-results
    "${CMAKE_CURRENT_BINARY_DIR}/preview-interactions-scaled" --preview-interactions --scale 1.5 --english)
set_tests_properties(tst_previewinteractions tst_previewinteractions_scaled PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
target_include_directories(tst_previewplacement PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_previewplacement PRIVATE Qt6::Core Qt6::Test)
set_target_properties(tst_previewplacement PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_previewplacement COMMAND tst_previewplacement -o -,txt)
set_tests_properties(tst_previewplacement PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_launchernavigation tst_launchernavigation.cpp)
target_include_directories(tst_launchernavigation PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_launchernavigation PRIVATE wscore Qt6::Test)
set_target_properties(tst_launchernavigation PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_launchernavigation COMMAND tst_launchernavigation)
set_tests_properties(tst_launchernavigation PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_test(NAME tst_searchlogo COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/search-logo" --search-logo --renderer d3d11)
set_tests_properties(tst_searchlogo PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_test(NAME tst_shortcutpanel COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/shortcut-panel" --shortcut-panel --renderer d3d11)
set_tests_properties(tst_shortcutpanel PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Real QML windows, real name searches, temporary files, process-local input.
add_test(NAME tst_searchnavigation COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/search-navigation" --search-navigation --renderer d3d11)
set_tests_properties(tst_searchnavigation PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

add_test(NAME tst_foregroundmemory COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/foreground-memory" --foreground-memory --require-lazy-panel --renderer d3d11 --scale 1.5)
set_tests_properties(tst_foregroundmemory PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# The same, with the panel made ahead in the background, as the app does.
add_test(NAME tst_foregroundpreload COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/foreground-preload" --foreground-memory --require-lazy-panel --preload-panel --renderer d3d11 --scale 1.5)
set_tests_properties(tst_foregroundpreload PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# #153: production Main.qml/Launcher/SearchEngine over temporary OOXML/text
# files and content indexes, including the explicit continuation action.
add_test(NAME tst_searchworkload COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/search-workload" --search-workload --renderer d3d11)
set_tests_properties(tst_searchworkload PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# First-run gating and draft/commit behavior in the real App and QML, with
# temporary profiles and process-local HKCU redirection. No clipboard writes.
foreach(case IN ITEMS fresh later apply legacy)
    add_test(NAME tst_onboarding_${case} COMMAND $<TARGET_FILE:WinShun>
        --probe-runtime "${CMAKE_CURRENT_BINARY_DIR}/onboarding-${case}"
        --onboarding-probe ${case} --renderer d3d11)
    set_tests_properties(tst_onboarding_${case} PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()

qt_add_executable(tst_notificationturns tst_notificationturns.cpp)
target_include_directories(tst_notificationturns PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_notificationturns PRIVATE winshun_options Qt6::Test)
set_target_properties(tst_notificationturns PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_notificationturns COMMAND tst_notificationturns)
set_tests_properties(tst_notificationturns PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_admintaphint tst_admintaphint.cpp)
target_include_directories(tst_admintaphint PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_admintaphint PRIVATE winshun_options Qt6::Test)
set_target_properties(tst_admintaphint PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_admintaphint COMMAND tst_admintaphint)
set_tests_properties(tst_admintaphint PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# When a portable copy offers to update the index service it installed (GitHub #243).
qt_add_executable(tst_serviceupdate tst_serviceupdate.cpp)
target_include_directories(tst_serviceupdate PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_serviceupdate PRIVATE winshun_options Qt6::Test)
set_target_properties(tst_serviceupdate PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_serviceupdate COMMAND tst_serviceupdate)
set_tests_properties(tst_serviceupdate PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The same for file dialogs of such programs (GitHub #216).
qt_add_executable(tst_admindialoghint tst_admindialoghint.cpp)
target_include_directories(tst_admindialoghint PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_admindialoghint PRIVATE winshun_options Qt6::Test)
set_target_properties(tst_admindialoghint PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_admindialoghint COMMAND tst_admindialoghint)
set_tests_properties(tst_admindialoghint PROPERTIES
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
    ${PROJECT_SOURCE_DIR}/src/app/Phosphor.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_displaygeometry PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_displaygeometry PRIVATE winshun_options Qt6::Gui Qt6::Qml Qt6::Test
    comctl32 dwmapi ole32 shell32 user32 advapi32)
set_target_properties(tst_displaygeometry PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_displaygeometry COMMAND tst_displaygeometry -o -,txt)
set_tests_properties(tst_displaygeometry PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

set(file_dialog_backend
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileDialog.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/BlenderFileDialog.cpp)
qt_add_executable(tst_filedialog tst_filedialog.cpp ${file_dialog_backend})
target_include_directories(tst_filedialog PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_filedialog PRIVATE wscore Qt6::Test ole32 oleaut32 uiautomationcore user32)
# Process identity is part of the CAD adapter's guard. This is a copy of our
# native protocol fixture, never AutoCAD; no Autodesk software is installed.
set(cad_dialog_fixture "$<TARGET_FILE_DIR:tst_filedialog>/cad-dialog-fixture/acad.exe")
add_custom_command(TARGET tst_filedialog POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tst_filedialog>/cad-dialog-fixture"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:tst_filedialog>" "${cad_dialog_fixture}")
add_test(NAME tst_filedialog COMMAND "${cad_dialog_fixture}" -o -,txt)
add_test(NAME tst_filedialog_other_program COMMAND tst_filedialog customRejectsOtherProgram -o -,txt)
set_tests_properties(tst_filedialog tst_filedialog_other_program PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The Qt-free dialog-navigation classifier used by the #104 service helper,
# its folder-request check and command-line quoting.
qt_add_executable(tst_dialognav tst_dialognav.cpp)
target_include_directories(tst_dialognav PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_dialognav PRIVATE wscore Qt6::Test ole32 oleaut32 shell32 user32)
add_test(NAME tst_dialognav COMMAND tst_dialognav)
# AutoCAD's own file window is taken only in acad.exe / acadlt.exe (GitHub
# #143): a copy of the test under that name, with the stand-in window. It
# contains nothing of AutoCAD.
set(cad_nav_fixture "$<TARGET_FILE_DIR:tst_dialognav>/cad-nav-fixture/acad.exe")
add_custom_command(TARGET tst_dialognav POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tst_dialognav>/cad-nav-fixture"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:tst_dialognav>" "${cad_nav_fixture}")
add_test(NAME tst_dialognav_cad COMMAND "${cad_nav_fixture}" cadByName cadLeftAlone)
set_tests_properties(tst_dialognav tst_dialognav_cad PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Launch only a short-lived copy of this test, writing into QTemporaryDir.
qt_add_executable(tst_shelllaunch WIN32 tst_shelllaunch.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Shell.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ShellDialogs.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp
    ${file_dialog_backend}
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileManagers.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Integrity.cpp)
target_include_directories(tst_shelllaunch PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_shelllaunch PRIVATE wscore Qt6::Gui Qt6::Test
    ole32 oleaut32 oleacc shell32 shlwapi user32 advapi32 taskschd uiautomationcore userenv)
add_test(NAME tst_shelllaunch COMMAND tst_shelllaunch -o -,txt)
set_tests_properties(tst_shelllaunch PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Folder-menu commands use the production shell worker and temporary files.
qt_add_executable(tst_foldermenu tst_foldermenu.cpp
    ${PROJECT_SOURCE_DIR}/src/app/FolderMenu.cpp
    ${PROJECT_SOURCE_DIR}/src/app/WindowFrame.cpp
    ${PROJECT_SOURCE_DIR}/src/app/FileIconProvider.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SiteIconImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SvgIcon.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/PhosphorGlyph.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Phosphor.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/SiteIcons.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp
    ${PROJECT_SOURCE_DIR}/src/app/RecentFolders.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ComWorker.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/KeyRouter.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Shell.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ShellDialogs.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp
    ${file_dialog_backend}
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileManagers.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Integrity.cpp)
target_include_directories(tst_foldermenu PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_foldermenu PRIVATE wscore wsplugin Qt6::Quick Qt6::Svg Qt6::Test
    ole32 oleaut32 oleacc shell32 shlwapi user32 advapi32 taskschd uiautomationcore userenv dwmapi comctl32 shcore gdi32 winhttp)
target_compile_definitions(tst_foldermenu PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
qt_add_resources(tst_foldermenu "menu_icons"
    PREFIX /fonts BASE "${PROJECT_SOURCE_DIR}/third_party/phosphor"
    FILES "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf"
          "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor-Fill.ttf")
set_target_properties(tst_foldermenu PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_foldermenu COMMAND tst_foldermenu -o -,txt)
set_tests_properties(tst_foldermenu PROPERTIES
    RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The command line ws as the settings page sees it (src/app/CommandLine,
# McpClients): the PATH as text, where ws is, the user's PATH written in keys
# of the test's own, and Claude Code's Skill installed through a stand-in for
# ws.exe (fake_ws.cpp).
add_executable(fake_ws fake_ws.cpp)
set_target_properties(fake_ws PROPERTIES AUTOMOC OFF)
qt_add_executable(tst_commandline tst_commandline.cpp
    ${PROJECT_SOURCE_DIR}/src/app/CommandLine.cpp
    ${PROJECT_SOURCE_DIR}/src/app/McpClients.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Shell.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileManagers.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Integrity.cpp)
target_include_directories(tst_commandline PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_commandline PRIVATE wscore Qt6::Gui Qt6::Test
    ole32 oleaut32 oleacc shell32 shlwapi user32 advapi32 taskschd userenv)
target_compile_definitions(tst_commandline PRIVATE WINSHUN_FAKE_WS="$<TARGET_FILE:fake_ws>")
add_dependencies(tst_commandline fake_ws WinShunMcp)
set_target_properties(tst_commandline PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_commandline COMMAND tst_commandline)
set_tests_properties(tst_commandline PROPERTIES
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

# A frameless window's dark mode after Qt's palette changes (GitHub #120): real windows.
qt_add_executable(tst_darkframe tst_darkframe.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_darkframe PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_darkframe PRIVATE winshun_options Qt6::Gui Qt6::Test comctl32 gdi32 dwmapi ole32 shell32 user32)
set_target_properties(tst_darkframe PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_darkframe COMMAND tst_darkframe)
set_tests_properties(tst_darkframe PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The real passive window lifecycle: render/GUI ordering, stale frames,
# native visibility, and native window identity across Qt close/hide.
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

# A right-click on the top-left pixel of a window without an active focus
# item (our menus) crashed Qt Quick (QTBUG-146671): the guard drops only that event.
qt_add_executable(tst_contextmenuguard tst_contextmenuguard.cpp ${PROJECT_SOURCE_DIR}/src/app/ContextMenuGuard.cpp)
target_include_directories(tst_contextmenuguard PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_contextmenuguard PRIVATE winshun_options Qt6::Quick Qt6::Test)
set_target_properties(tst_contextmenuguard PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_contextmenuguard COMMAND tst_contextmenuguard -o -,txt)
set_tests_properties(tst_contextmenuguard PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_PLUGIN_PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

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
target_compile_definitions(tst_aisettings PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
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

# The keys and mouse buttons a recording shows (GitHub #98, src/capture/Keycaps):
# what is never shown, names, rows, counts, timing and size.
qt_add_executable(tst_keycaps tst_keycaps.cpp)
target_include_directories(tst_keycaps PRIVATE ${PROJECT_SOURCE_DIR}/src/core) # OwnInput.h
target_link_libraries(tst_keycaps PRIVATE wscapture Qt6::Test)
set_target_properties(tst_keycaps PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_keycaps COMMAND tst_keycaps)
set_tests_properties(tst_keycaps PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The pin window's text pick (GitHub #258): the pill that says what to do
# stays while the words are read, then fades off them. A real (Win32) pin
# window for a few seconds.
qt_add_executable(tst_pinwindow tst_pinwindow.cpp ${PROJECT_SOURCE_DIR}/src/capture/PinWindow.cpp) # built into the capture plugin, not wscapture
target_link_libraries(tst_pinwindow PRIVATE wscapture Qt6::Test dwmapi gdi32 imm32 shcore user32)
set_target_properties(tst_pinwindow PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_pinwindow COMMAND tst_pinwindow)
set_tests_properties(tst_pinwindow PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Network shares, WSL and WebDAV: their files' contents (GitHub #114).
qt_add_executable(tst_remotecontent tst_remotecontent.cpp)
target_link_libraries(tst_remotecontent PRIVATE wscore Qt6::Test ws2_32)
set_target_properties(tst_remotecontent PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_remotecontent COMMAND tst_remotecontent)
set_tests_properties(tst_remotecontent PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_settings tst_settings.cpp ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp)
target_include_directories(tst_settings PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_settings PRIVATE wscore Qt6::Gui Qt6::Test)
set_target_properties(tst_settings PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_settings COMMAND tst_settings)
set_tests_properties(tst_settings PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The screenshot plugin's settings, capture.ini (src/capture-plugin/CaptureSettings).
qt_add_executable(tst_capturesettings tst_capturesettings.cpp ${PROJECT_SOURCE_DIR}/src/capture-plugin/CaptureSettings.cpp)
target_include_directories(tst_capturesettings PRIVATE ${PROJECT_SOURCE_DIR}/src/capture-plugin ${PROJECT_SOURCE_DIR}/src/capture ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_capturesettings PRIVATE winshun_options Qt6::Gui Qt6::Qml Qt6::Test)
set_target_properties(tst_capturesettings PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_capturesettings COMMAND tst_capturesettings)
set_tests_properties(tst_capturesettings PROPERTIES
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

# The live loupe and the picture area's native alpha cursor, without App or user data.
qt_add_executable(tst_captureui tst_captureui.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/CaptureImage.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/CapturePointer.cpp)
target_include_directories(tst_captureui PRIVATE ${PROJECT_SOURCE_DIR}/src/capture)
target_link_libraries(tst_captureui PRIVATE winshun_options Qt6::Quick Qt6::Test)
set_target_properties(tst_captureui PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
foreach(scale IN ITEMS 1 1.25 1.5 1.75 2)
    add_test(NAME tst_captureui_${scale} COMMAND tst_captureui -o -,txt)
    set_tests_properties(tst_captureui_${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=${scale}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    add_test(NAME tst_captureui_d3d11_${scale} COMMAND tst_captureui
        crosshairIsAlphaAndKeepsItsHotspot loupeKeepsExactPixelsAtTheScreenEdges loupeReusesItsTextureWhileMoving -o -,txt)
    set_tests_properties(tst_captureui_d3d11_${scale} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=;QSG_RHI_BACKEND=d3d11;QSG_RENDER_LOOP=basic;QT_SCALE_FACTOR=1;QT_SCREEN_SCALE_FACTORS=${scale}"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endforeach()

add_test(NAME tst_captureui_idle_graphics COMMAND tst_captureui
    hiddenGraphicsAreReleasedAndTheWindowReopens -o -,txt)
set_tests_properties(tst_captureui_idle_graphics PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_QUICK_BACKEND=;QSG_RHI_BACKEND=d3d11;QSG_RENDER_LOOP=threaded;QT_SCALE_FACTOR=1;QT_SCREEN_SCALE_FACTORS=1.5"
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

# The presenter (GitHub #189, docs/design-presenter.md): its rules
# (PresenterState), the session with no window (Presenter: boards, back,
# through, undo across screens, styles), the transparent compositor, and how
# long a pen move takes on a 4K canvas (the long stroke's cost).
qt_add_executable(tst_presenter tst_presenter.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/Presenter.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/AnnotationModel.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/AnnotationCanvas.cpp)
target_link_libraries(tst_presenter PRIVATE wscapture Qt6::Qml Qt6::Quick Qt6::Test)
set_target_properties(tst_presenter PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_presenter COMMAND tst_presenter)
set_tests_properties(tst_presenter PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The laser pointer's trail (LaserTrail): its taper, its life, its outline.
qt_add_executable(tst_lasertrail tst_lasertrail.cpp)
target_link_libraries(tst_lasertrail PRIVATE wscapture Qt6::Test)
set_target_properties(tst_lasertrail PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_lasertrail COMMAND tst_lasertrail)
set_tests_properties(tst_lasertrail PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_traymenu tst_traymenu.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/PinManager.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/PinWindow.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ShellDialogs.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp
    ${PROJECT_SOURCE_DIR}/src/app/TrayMenu.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Phosphor.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_traymenu PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(tst_traymenu PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
qt_add_resources(tst_traymenu "phosphor"
    PREFIX /fonts
    BASE "${PROJECT_SOURCE_DIR}/third_party/phosphor"
    FILES "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf"
          "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor-Fill.ttf")
target_link_libraries(tst_traymenu PRIVATE wscore wscapture Qt6::Test Qt6::Quick comctl32 dwmapi gdi32 imm32 ole32 oleacc oleaut32 shcore shell32 user32)
set_target_properties(tst_traymenu PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_traymenu COMMAND tst_traymenu)
set_tests_properties(tst_traymenu PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_appearance tst_appearance.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Settings.cpp
    ${PROJECT_SOURCE_DIR}/src/app/WindowFrame.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/PhosphorGlyph.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Phosphor.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_appearance PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(tst_appearance PRIVATE WINSHUN_QML_DIR="${PROJECT_SOURCE_DIR}/src/app/qml")
qt_add_resources(tst_appearance "phosphor"
    PREFIX /fonts
    BASE "${PROJECT_SOURCE_DIR}/third_party/phosphor"
    FILES "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf"
          "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor-Fill.ttf")
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

qt_add_executable(tst_activity tst_activity.cpp ${PROJECT_SOURCE_DIR}/src/app/ActivityText.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ProcessUsage.cpp)
target_include_directories(tst_activity PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_activity PRIVATE winshun_options Qt6::Test psapi)
set_target_properties(tst_activity PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_activity COMMAND tst_activity -o -,txt)
set_tests_properties(tst_activity PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

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
    ${PROJECT_SOURCE_DIR}/src/app/ClipThumbnailProvider.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipFiles.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ColorSwatch.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/PhosphorGlyph.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Phosphor.cpp
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
    ${PROJECT_SOURCE_DIR}/src/app/platform/OcrFiles.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipImage.cpp)
target_include_directories(tst_cliplifecycle PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_cliplifecycle PRIVATE winshun_options wscore wscapture Qt6::Gui Qt6::Test
    onnxruntime_headers windowsapp user32 gdi32 shell32 ole32 version)
set_target_properties(tst_cliplifecycle PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_cliplifecycle COMMAND tst_cliplifecycle -o -,txt
    -o "${CMAKE_CURRENT_BINARY_DIR}/cliplifecycle.txt,txt")
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
    ${PROJECT_SOURCE_DIR}/src/app/platform/NumberKeys.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/PlainHotkeys.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ServiceKeyListener.cpp)
target_include_directories(tst_keylistener PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_keylistener PRIVATE wscore Qt6::Test user32 advapi32 shcore)
target_compile_definitions(tst_keylistener PRIVATE WINSHUN_SERVICE_EXE="$<TARGET_FILE:WinShunService>")
add_dependencies(tst_keylistener WinShunService)

qt_add_executable(tst_keyrouter tst_keyrouter.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/KeyRouter.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/HoldWatch.cpp ${PROJECT_SOURCE_DIR}/src/app/Diagnostics.cpp)
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
# Its helper process for an elevated Win顺 (DropPieHelper), for real: started,
# favorites and appearance passed on, started again after it is ended, stopped.
add_test(NAME tst_droppiehelper COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/drop-pie-helper" --drop-pie-check --renderer d3d11)
set_tests_properties(tst_droppiehelper PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# Whether Windows takes every point over the pie's window for the pie's
# (GitHub #150): a drag reaches it only there. By both renderers, revealed
# and as it waits for a drag with nothing of it drawn. Each shows the real
# window at the middle of the primary screen for under a second, never
# active, and sends no input; at 150 %, the scale it was measured at.
# Skipped (5) while a mouse button is held; not measured (6) fails.
foreach(renderer IN ITEMS d3d11 software)
    foreach(state IN ITEMS revealed unrevealed)
        set(test tst_droppiehit_${state}_${renderer})
        set(arguments --preview-results "${CMAKE_CURRENT_BINARY_DIR}/drop-pie-hit/${state}-${renderer}"
            --drop-pie-hit --renderer ${renderer} --scale 1.5)
        if(state STREQUAL "unrevealed")
            list(APPEND arguments --unrevealed)
        endif()
        add_test(NAME ${test} COMMAND $<TARGET_FILE:WinShun> ${arguments})
        set_tests_properties(${test} PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 SKIP_RETURN_CODE 5
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
    endforeach()
endforeach()

# The favorites: their file, the store, and the search's list with nothing typed.
qt_add_executable(tst_favorites tst_favorites.cpp)
target_link_libraries(tst_favorites PRIVATE wscore Qt6::Test)

# How a custom command is started without a console window (GitHub #112): the
# kinds of program, the command lines, and real runs of a .bat and a .ps1.
qt_add_executable(tst_commandlaunch tst_commandlaunch.cpp)
target_link_libraries(tst_commandlaunch PRIVATE wscore Qt6::Test)

# The plugins that left the setup for the plugin library (GitHub #138,
# src/app/MovedPlugins): the screenshot's settings moved to its plugin.
qt_add_executable(tst_movedplugins tst_movedplugins.cpp ${PROJECT_SOURCE_DIR}/src/app/MovedPlugins.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp)
target_include_directories(tst_movedplugins PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_movedplugins PRIVATE wscore wsplugin Qt6::Test winhttp ws2_32)
target_compile_definitions(tst_movedplugins PRIVATE WINSHUN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
set_target_properties(tst_movedplugins PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_movedplugins COMMAND tst_movedplugins)
set_tests_properties(tst_movedplugins PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The plugin updates' background service (src/app/PluginUpdates): the signed
# list, what is installed, kept aside, told once.
qt_add_executable(tst_pluginupdates tst_pluginupdates.cpp ${PROJECT_SOURCE_DIR}/src/app/PluginUpdates.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp)
target_include_directories(tst_pluginupdates PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_pluginupdates PRIVATE wscore wsplugin miniz Qt6::Test winhttp ws2_32 bcrypt)
set_target_properties(tst_pluginupdates PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_pluginupdates COMMAND tst_pluginupdates)
set_tests_properties(tst_pluginupdates PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The proxy setting (GitHub #84): the address check, and WinHTTP going where
# the setting says, against two small servers of the test's own.
qt_add_executable(tst_proxy tst_proxy.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp)
target_include_directories(tst_proxy PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_proxy PRIVATE wscore Qt6::Test winhttp ws2_32)
add_test(NAME tst_proxy COMMAND tst_proxy)
set_tests_properties(tst_proxy PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The actual shell/image-provider path: resolve commands, decode custom
# pictures and fall back immediately for offline programs. Never executes a command.
qt_add_executable(tst_commandicons tst_commandicons.cpp
    ${PROJECT_SOURCE_DIR}/src/app/FileIconProvider.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SiteIconImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SvgIcon.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SystemTheme.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Phosphor.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/SiteIcons.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Shell.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ShellDialogs.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp
    ${file_dialog_backend}
    ${PROJECT_SOURCE_DIR}/src/app/platform/FileManagers.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/FolderViewHit.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Integrity.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/WindowEffects.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Foreground.cpp)
target_include_directories(tst_commandicons PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_commandicons PRIVATE wscore wsplugin Qt6::Quick Qt6::Svg Qt6::Test
    comctl32 gdi32 dwmapi ole32 oleaut32 oleacc shell32 shlwapi user32 advapi32 taskschd uiautomationcore userenv winhttp)
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

# What a click on the "截图已保存" notification shows: file, folder or nothing.
qt_add_executable(tst_savedfile tst_savedfile.cpp)
target_include_directories(tst_savedfile PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_savedfile PRIVATE Qt6::Core Qt6::Test)

# The diagnostics' glyph self-check (GitHub #70): clean in the fonts drawn
# with, a font's glyph numbers drawn with another reported; the display part
# of an export's summary.
qt_add_executable(tst_textcheck tst_textcheck.cpp ${PROJECT_SOURCE_DIR}/src/app/TextCheck.cpp
    ${PROJECT_SOURCE_DIR}/src/app/DisplayDiagnostics.cpp)
target_include_directories(tst_textcheck PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_compile_definitions(tst_textcheck PRIVATE WINSHUN_FONTS_DIR="${PROJECT_SOURCE_DIR}/resources/fonts")
target_link_libraries(tst_textcheck PRIVATE wscore Qt6::Quick Qt6::Test dxgi)
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

# The folders used lately (the dialog bar's and the folder menu's): Windows'
# Recent shortcuts listed newest first without touching their targets.
qt_add_executable(tst_recentfolders tst_recentfolders.cpp ${PROJECT_SOURCE_DIR}/src/app/RecentFolders.cpp)
target_include_directories(tst_recentfolders PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_recentfolders PRIVATE winshun_options Qt6::Core Qt6::Test ole32 shell32)
set_target_properties(tst_recentfolders PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_recentfolders COMMAND tst_recentfolders)
set_tests_properties(tst_recentfolders PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

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
qt_add_executable(tst_ocr tst_ocr.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/PaddleOcr.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/OcrFiles.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/CaptureOcr.cpp)
target_include_directories(tst_ocr PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_include_directories(tst_ocr PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_ocr PRIVATE winshun_options wscapture Qt6::Qml Qt6::Test onnxruntime_headers)
set_target_properties(tst_ocr PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

qt_add_executable(tst_ocrpackage tst_ocrpackage.cpp
    ${PROJECT_SOURCE_DIR}/src/capture/CaptureOcr.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/OcrPackage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/OcrFiles.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/PaddleOcr.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp)
target_include_directories(tst_ocrpackage PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_ocrpackage PRIVATE winshun_options wscore wscapture miniz Qt6::Gui Qt6::Qml Qt6::Network Qt6::Test onnxruntime_headers winhttp)
set_target_properties(tst_ocrpackage PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
set_source_files_properties("${PROJECT_SOURCE_DIR}/resources/ocr/package.json" PROPERTIES QT_RESOURCE_ALIAS ocr.json)
set_source_files_properties("${PROJECT_SOURCE_DIR}/third_party/ppocr/LICENSE" PROPERTIES QT_RESOURCE_ALIAS paddleocr-license.txt)
qt_add_resources(tst_ocrpackage "ocr_package_test"
    PREFIX /components
    FILES
        "${PROJECT_SOURCE_DIR}/resources/ocr/package.json"
        "${PROJECT_SOURCE_DIR}/third_party/ppocr/LICENSE")
add_test(NAME tst_ocrpackage COMMAND tst_ocrpackage)

qt_add_executable(tst_uifont tst_uifont.cpp ${PROJECT_SOURCE_DIR}/src/app/UiFont.cpp)
target_include_directories(tst_uifont PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
set_target_properties(tst_uifont PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
target_link_libraries(tst_uifont PRIVATE winshun_options Qt6::Quick Qt6::Test)
add_test(NAME tst_uifont_freetype COMMAND tst_uifont)
set_tests_properties(tst_uifont_freetype PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows:fontengine=freetype")
add_test(NAME tst_uifont_gdi COMMAND tst_uifont)
set_tests_properties(tst_uifont_gdi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows:fontengine=gdi")
set_tests_properties(tst_ocrpackage tst_uifont_freetype tst_uifont_gdi PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

qt_add_executable(tst_download tst_download.cpp)
target_link_libraries(tst_download PRIVATE winshun_options wsplugin Qt6::Test)
add_test(NAME tst_download COMMAND tst_download)
set_tests_properties(tst_download PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
add_test(NAME tst_download_cmake COMMAND ${CMAKE_COMMAND}
    "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/download-cmake" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tst_download.cmake")

# What a drop target gets when results are dragged out of Win顺.
qt_add_executable(tst_dragout tst_dragout.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/DragOut.cpp)
target_include_directories(tst_dragout PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_dragout PRIVATE winshun_options Qt6::Gui Qt6::Test)

# The system switches' cache (Switches): toggles turned into On and Off, writes
# coalesced, failures, changes made elsewhere, through a fake backend on the
# real switch thread. Nothing of the system is read or changed.
qt_add_executable(tst_switches tst_switches.cpp ${PROJECT_SOURCE_DIR}/src/app/Switches.cpp
    ${PROJECT_SOURCE_DIR}/src/app/Switches.h ${PROJECT_SOURCE_DIR}/src/app/platform/ComWorker.cpp
    ${PROJECT_SOURCE_DIR}/src/switches-plugin/platform/SwitchKeepAwake.cpp) # keep awake is built in, over a fake clock
target_include_directories(tst_switches PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/app/platform ${PROJECT_SOURCE_DIR}/src/switches-plugin)
target_link_libraries(tst_switches PRIVATE wscore Qt6::Test ole32 powrprof)
set_target_properties(tst_switches PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_switches COMMAND tst_switches)
set_tests_properties(tst_switches PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Host -> runner -> production service, with isolated fake hardware writes;
# the actual bundled plugin is also read and its own power request released.
qt_add_executable(tst_switchplugin tst_switchplugin.cpp
    ${PROJECT_SOURCE_DIR}/src/app/SwitchPlugin.cpp ${PROJECT_SOURCE_DIR}/src/app/SwitchPlugin.h
    ${PROJECT_SOURCE_DIR}/src/app/Switches.cpp ${PROJECT_SOURCE_DIR}/src/app/Switches.h
    ${PROJECT_SOURCE_DIR}/src/app/platform/SettingBroadcast.cpp)
target_link_libraries(tst_switchplugin PRIVATE wsplugin wsswitch_service Qt6::Test psapi)
target_compile_definitions(tst_switchplugin PRIVATE
    WINSHUN_PLUGIN_HOST="$<TARGET_FILE:WinShunPluginHost>"
    WINSHUN_QT_BIN="$<TARGET_FILE_DIR:Qt6::Core>"
    WINSHUN_SWITCH_EXE="$<TARGET_FILE:WinShunSwitches>"
    WINSHUN_SWITCH_DLL="$<TARGET_FILE:WinShunSwitchesApp>"
    WINSHUN_SWITCH_MANIFEST="${PROJECT_SOURCE_DIR}/src/switches-plugin/package/plugin.json")
add_dependencies(tst_switchplugin WinShunPluginHost WinShunSwitches WinShunSwitchesApp)
set_target_properties(tst_switchplugin PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_switchplugin COMMAND tst_switchplugin -o -,txt)
set_tests_properties(tst_switchplugin PROPERTIES TIMEOUT 180
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
add_test(NAME tst_switchprovider_ui COMMAND WinShun --preview-results
    "${CMAKE_CURRENT_BINARY_DIR}/switch-provider-ui" --switch-provider-probe)
set_tests_properties(tst_switchprovider_ui PROPERTIES TIMEOUT 40 RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# The scope the user chose comes back at the next start (GitHub #263): kept
# when chosen by a tab (the property, as QML writes it), by Tab or by a chip;
# not when a prefix typed or the program itself sets it. 文件夹 comes back with
# the strip's choice; 全部 where the tabs have no 文件, or 内容 is not searched.
# The real Launcher and a state.ini of the run, each "start" a new Launcher
# set up in App's order.
add_test(NAME tst_scopememory COMMAND $<TARGET_FILE:WinShun> --preview-results
    "${CMAKE_CURRENT_BINARY_DIR}/scope-memory" --scope-memory)
set_tests_properties(tst_scopememory PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# App's part of it: the real App, isolated (--probe-runtime), started on a
# profile whose state.ini says where the scope was left, has the launcher
# there; and in 全部 all the same with 每次打开时清空搜索 on.
foreach(scope_left IN ITEMS files content)
    add_test(NAME tst_scoperestore_${scope_left} COMMAND $<TARGET_FILE:WinShun>
        --probe-runtime "${CMAKE_CURRENT_BINARY_DIR}/scope-restore-${scope_left}" --scope-restore-check ${scope_left}
        --settle-ms 1500 --renderer d3d11)
endforeach()
add_test(NAME tst_scoperestore_clearonopen COMMAND $<TARGET_FILE:WinShun>
    --probe-runtime "${CMAKE_CURRENT_BINARY_DIR}/scope-restore-clearonopen" --scope-restore-check files --clear-on-open
    --settle-ms 1500 --renderer d3d11)
set_tests_properties(tst_scoperestore_files tst_scoperestore_content tst_scoperestore_clearonopen PROPERTIES
    RUN_SERIAL TRUE TIMEOUT 120
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Win+S / Win+V and Explorer's DisabledHotkeys (platform/WinV): the decision and
# the registry writes, in HKCU redirected to a scratch key (never the real one).
qt_add_executable(tst_winkeys tst_winkeys.cpp ${PROJECT_SOURCE_DIR}/src/app/platform/WinV.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Integrity.cpp)
target_include_directories(tst_winkeys PRIVATE ${PROJECT_SOURCE_DIR}/src/app/platform)
target_link_libraries(tst_winkeys PRIVATE wscore Qt6::Test advapi32)
set_target_properties(tst_winkeys PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_winkeys COMMAND tst_winkeys)
set_tests_properties(tst_winkeys PROPERTIES
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

# The command line ws.exe (src/cli): its pieces built in again (the option
# table, the parser, the encoder, the start decision), and the real ws.exe,
# whose path it is told, against the MCP server in the test on a test pipe.
# The runner's JSON and Launch.cpp are built in as ws has them (wsplugin
# would bring the JSON in twice).
qt_add_executable(tst_cli tst_cli.cpp ${WS_CLI_SOURCES}
    ${PROJECT_SOURCE_DIR}/src/plugin/runner/Json.cpp ${PROJECT_SOURCE_DIR}/src/mcp/Launch.cpp)
target_include_directories(tst_cli PRIVATE ${PROJECT_SOURCE_DIR}/src/cli ${PROJECT_SOURCE_DIR}/src/plugin/runner ${PROJECT_SOURCE_DIR}/src/mcp)
target_link_libraries(tst_cli PRIVATE wsmcp Qt6::Test ntdll shell32 ole32 oleaut32 uuid advapi32)
target_compile_definitions(tst_cli PRIVATE
    WINSHUN_CLI="$<TARGET_FILE:ws>"
    WINSHUN_CLI_SKILL="${PROJECT_SOURCE_DIR}/src/cli/SKILL.md")
add_dependencies(tst_cli ws)

# The pieces of the tools plugin that compute (src/tools-plugin), built in
# again: the calculator and the conversions have edges (division by zero,
# overflow, bad input) worth pinning down.
qt_add_executable(tst_tools tst_tools.cpp
    ${PROJECT_SOURCE_DIR}/src/tools-plugin/Calc.cpp ${PROJECT_SOURCE_DIR}/src/tools-plugin/Units.cpp
    ${PROJECT_SOURCE_DIR}/src/tools-plugin/Color.cpp ${PROJECT_SOURCE_DIR}/src/tools-plugin/Codec.cpp)
target_include_directories(tst_tools PRIVATE ${PROJECT_SOURCE_DIR}/src/tools-plugin)
target_compile_definitions(tst_tools PRIVATE WINSHUN_TOOLS_MANIFEST="${PROJECT_SOURCE_DIR}/plugins/winshun.tools/plugin.json")
target_link_libraries(tst_tools PRIVATE winshun_options Qt6::Test bcrypt)

# winshun.semantic's pieces (src/semantic-plugin), built in again: passages,
# the vector store, the model services' settings.
qt_add_executable(tst_semantic tst_semantic.cpp
    ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Chunker.cpp ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Store.cpp
    ${PROJECT_SOURCE_DIR}/src/semantic-plugin/Embedder.cpp)
target_include_directories(tst_semantic PRIVATE ${PROJECT_SOURCE_DIR}/src/semantic-plugin ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(tst_semantic PRIVATE winshun_options Qt6::Test winhttp)

# winshun.localsend's pieces (src/localsend-plugin, the wslocalsend library):
# names from other devices made safe, files written whole or not at all,
# LocalSend's HTTP and JSON, a receiver and a sender talking TLS on 127.0.0.1.
qt_add_executable(tst_localsend tst_localsend.cpp)
target_link_libraries(tst_localsend PRIVATE wslocalsend Qt6::Test)
target_compile_definitions(tst_localsend PRIVATE WINSHUN_LOCALSEND_EXE="$<TARGET_FILE:WinShunLocalSend>")
add_dependencies(tst_localsend WinShunLocalSend)
set_target_properties(tst_localsend PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_localsend COMMAND tst_localsend)
set_tests_properties(tst_localsend PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# winshun.break's clock (src/break-plugin/Schedule), built in again: the
# notice, the wait for a pause in typing, long breaks, time away, quiet
# times, postpone, skip and strict, with made-up times.
qt_add_executable(tst_break tst_break.cpp ${PROJECT_SOURCE_DIR}/src/break-plugin/Schedule.cpp)
target_include_directories(tst_break PRIVATE ${PROJECT_SOURCE_DIR}/src/break-plugin)
target_link_libraries(tst_break PRIVATE winshun_options Qt6::Test)
set_target_properties(tst_break PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_break COMMAND tst_break)
set_tests_properties(tst_break PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# winshun.explorer-tabs's rules (src/explorer-tabs-plugin/Rules), built in
# again: which new folder windows become tabs, where they go, the same
# folder already open, Ctrl+Shift, a tab dragged out, Control Panel, the
# integrity level and the test-root restriction, with made-up windows.
qt_add_executable(tst_explorertabs tst_explorertabs.cpp ${PROJECT_SOURCE_DIR}/src/explorer-tabs-plugin/Rules.cpp
    ${PROJECT_SOURCE_DIR}/src/explorer-tabs-plugin/Hider.cpp ${PROJECT_SOURCE_DIR}/src/explorer-tabs-plugin/Closer.cpp)
target_include_directories(tst_explorertabs PRIVATE ${PROJECT_SOURCE_DIR}/src/explorer-tabs-plugin)
target_link_libraries(tst_explorertabs PRIVATE winshun_options Qt6::Test)
set_target_properties(tst_explorertabs PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_explorertabs COMMAND tst_explorertabs)
set_tests_properties(tst_explorertabs PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# winshun.bookmarks (src/bookmarks-plugin/Bookmarks), built in again: reading
# Bookmarks files, the folder text, percent-decoding, matching, scoring,
# the 50-row cap and the "nothing found" rows.
qt_add_executable(tst_bookmarks tst_bookmarks.cpp ${PROJECT_SOURCE_DIR}/src/bookmarks-plugin/Bookmarks.cpp)
target_include_directories(tst_bookmarks PRIVATE ${PROJECT_SOURCE_DIR}/src/bookmarks-plugin ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(tst_bookmarks PRIVATE winshun_options Qt6::Test)
set_target_properties(tst_bookmarks PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_bookmarks COMMAND tst_bookmarks)
set_tests_properties(tst_bookmarks PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# winshun.translate's logic (src/translate-plugin/Translate), built in again:
# the target language, the DeepL and chat requests, reading the answers and
# the services' errors, and the rows in both languages; the production HTTP
# path against local servers that stall before headers or in the body.
qt_add_executable(tst_translate tst_translate.cpp ${PROJECT_SOURCE_DIR}/src/translate-plugin/Translate.cpp
    ${PROJECT_SOURCE_DIR}/src/translate-plugin/Http.cpp)
add_dependencies(tst_translate WinShunTranslate)
target_include_directories(tst_translate PRIVATE ${PROJECT_SOURCE_DIR}/src/translate-plugin ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(tst_translate PRIVATE winshun_options Qt6::Test Qt6::Network winhttp)
target_compile_definitions(tst_translate PRIVATE WINSHUN_TRANSLATE_MANIFEST="${PROJECT_SOURCE_DIR}/src/translate-plugin/package/plugin.json")
set_target_properties(tst_translate PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_translate COMMAND tst_translate)
set_tests_properties(tst_translate PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The plugin system (src/plugin): manifests, items, zero-code, the catalog,
# and plugin processes started through the runner WinShunPluginHost.exe,
# played by plugin_fake.exe (no Qt, as a plugin would be); both paths are
# told to the test.
add_executable(plugin_fake plugin_fake.cpp)
target_link_libraries(plugin_fake PRIVATE wsplugin_lite)
set_target_properties(plugin_fake PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
qt_add_executable(tst_plugin tst_plugin.cpp)
qt_add_executable(plugin_qt plugin_qt.cpp)
target_link_libraries(plugin_qt PRIVATE winshun_options Qt6::Quick)
set_target_properties(plugin_qt PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
set(shared_qt_dir "${CMAKE_CURRENT_BINARY_DIR}/shared-qt-runtime")
add_custom_command(OUTPUT "${shared_qt_dir}/deployed.stamp"
    COMMAND Qt6::windeployqt --dir "${shared_qt_dir}" --qmldir "${PROJECT_SOURCE_DIR}/src/app/qml"
        --no-translations --no-compiler-runtime --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
        --include-plugins qoffscreen $<IF:$<CONFIG:Debug>,--debug,--release> "$<TARGET_FILE:plugin_qt>"
    COMMAND ${CMAKE_COMMAND} -E touch "${shared_qt_dir}/deployed.stamp"
    DEPENDS plugin_qt
    VERBATIM)
add_custom_target(tst_plugin_qt_runtime DEPENDS "${shared_qt_dir}/deployed.stamp")
target_compile_definitions(tst_plugin PRIVATE WINSHUN_PLUGIN_QT="$<TARGET_FILE:plugin_qt>" WINSHUN_SHARED_QT_DIR="${shared_qt_dir}")
add_dependencies(tst_plugin tst_plugin_qt_runtime)
add_test(NAME tst_puresearch COMMAND $<TARGET_FILE:WinShun>
    --preview-results "${CMAKE_CURRENT_BINARY_DIR}/pure-search" --pure-search --renderer d3d11 --scale 1.5)
add_test(NAME tst_pluginreviewsettings COMMAND $<TARGET_FILE:WinShun>
    --preview-settings "${CMAKE_CURRENT_BINARY_DIR}/plugin-review-settings" --plugin-review-settings --renderer d3d11 --scale 1.5)
set_tests_properties(tst_puresearch tst_pluginreviewsettings PROPERTIES RUN_SERIAL TRUE TIMEOUT 90
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
target_include_directories(tst_plugin PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_plugin PRIVATE wsplugin miniz Qt6::Test) # miniz: the test writes zips to install
target_compile_definitions(tst_plugin PRIVATE
    WINSHUN_PLUGIN_HOST="$<TARGET_FILE:WinShunPluginHost>"
    WINSHUN_PLUGIN_FAKE="$<TARGET_FILE:plugin_fake>"
    WINSHUN_TEST_PLUGINS="${CMAKE_CURRENT_SOURCE_DIR}/plugins"
    WINSHUN_SOURCE_PLUGINS="${PROJECT_SOURCE_DIR}/plugins"
    WINSHUN_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_dependencies(tst_plugin WinShunPluginHost plugin_fake)

add_test(NAME tst_core COMMAND tst_core)
add_test(NAME tst_service COMMAND tst_service)
add_test(NAME tst_keylistener COMMAND tst_keylistener)
add_test(NAME tst_clipboard COMMAND tst_clipboard)
add_test(NAME tst_favorites COMMAND tst_favorites)
add_test(NAME tst_commandlaunch COMMAND tst_commandlaunch)
set_tests_properties(tst_commandlaunch PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
add_test(NAME tst_color COMMAND tst_color)
add_test(NAME tst_elide COMMAND tst_elide)
add_test(NAME tst_savedfile COMMAND tst_savedfile)
add_test(NAME tst_placement COMMAND tst_placement)
add_test(NAME tst_dragout COMMAND tst_dragout)
add_test(NAME tst_siteicon COMMAND tst_siteicon)
add_test(NAME tst_backup COMMAND tst_backup)
add_test(NAME tst_ocr COMMAND tst_ocr)
add_test(NAME tst_tools COMMAND tst_tools)
add_test(NAME tst_semantic COMMAND tst_semantic)
add_test(NAME tst_mcp COMMAND tst_mcp)
add_test(NAME tst_cli COMMAND tst_cli)
add_test(NAME tst_plugin COMMAND tst_plugin -o -,txt)
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
    ${file_dialog_backend})
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
    ${PROJECT_SOURCE_DIR}/src/app/ClipThumbnailProvider.cpp ${PROJECT_SOURCE_DIR}/src/app/ColorText.cpp)
target_include_directories(tst_clipselect PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_clipselect PRIVATE wscore Qt6::Quick Qt6::Test)
set_target_properties(tst_clipselect PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_clipselect COMMAND tst_clipselect)
set_tests_properties(tst_clipselect PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
# Let ctest find the Qt DLLs without a deploy step.
set_tests_properties(tst_core tst_service tst_keylistener tst_clipboard tst_favorites tst_backup tst_color tst_elide tst_savedfile tst_placement tst_dragout tst_siteicon tst_ocr tst_tools tst_semantic tst_mcp tst_cli tst_plugin PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The in-app update (Updater): the update service's answer, downloads from
# mirrors then GitHub checked by size and SHA-256 (a fake server on
# 127.0.0.1), the version resource, unpacking a portable zip safely, and
# WinShunUpdate.exe's swap of a portable copy's files (data kept, rolled back),
# and a whole run of it: the new version started, also one that asks for
# administrator rights (GitHub #274).
qt_add_executable(tst_updater tst_updater.cpp ${PROJECT_SOURCE_DIR}/src/app/UpdatePackage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/Http.cpp ${PROJECT_SOURCE_DIR}/src/updater/Swap.cpp)
target_include_directories(tst_updater PRIVATE ${PROJECT_SOURCE_DIR}/src/app ${PROJECT_SOURCE_DIR}/src/updater)
target_link_libraries(tst_updater PRIVATE wscore miniz Qt6::Test version winhttp ws2_32)
# WinShun.exe as the helper sees it, for its whole run (tst_updater_fakeapp.cpp);
# no Qt and the C runtime linked in, so that a copy runs from any folder.
add_executable(tst_updater_fakeapp WIN32 tst_updater_fakeapp.cpp)
target_link_libraries(tst_updater_fakeapp PRIVATE winshun_options)
set_target_properties(tst_updater_fakeapp PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(tst_updater PRIVATE WINSHUN_UPDATE_EXE="$<TARGET_FILE:WinShunUpdate>"
    WINSHUN_FAKE_APP_EXE="$<TARGET_FILE:tst_updater_fakeapp>")
set_target_properties(tst_updater PROPERTIES WIN32_EXECUTABLE FALSE)
add_dependencies(tst_updater WinShunUpdate tst_updater_fakeapp)
add_test(NAME tst_updater COMMAND tst_updater)
set_tests_properties(tst_updater PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# A Scoop install (core/ScoopInstall): detection, the stable "current" path
# (real directory junctions), and bucket/winshun.json.
qt_add_executable(tst_scoopinstall tst_scoopinstall.cpp)
target_include_directories(tst_scoopinstall PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(tst_scoopinstall PRIVATE wscore Qt6::Test)
target_compile_definitions(tst_scoopinstall PRIVATE WINSHUN_BUCKET_MANIFEST="${PROJECT_SOURCE_DIR}/bucket/winshun.json")
set_target_properties(tst_scoopinstall PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_scoopinstall COMMAND tst_scoopinstall)
set_tests_properties(tst_scoopinstall PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# Clipboard pinning: real native formats on a private window station, WIC
# file decoding, asynchronous provenance and physical multi-monitor placement.
qt_add_executable(tst_pinclipboard tst_pinclipboard.cpp
    ${PROJECT_SOURCE_DIR}/src/capture-plugin/PinImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/PictureFile.cpp
    ${PROJECT_SOURCE_DIR}/src/app/ClipImage.cpp
    ${PROJECT_SOURCE_DIR}/src/app/platform/ClipboardWatcher.cpp)
target_include_directories(tst_pinclipboard PRIVATE ${PROJECT_SOURCE_DIR}/src/capture-plugin ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_pinclipboard PRIVATE wscore Qt6::Gui Qt6::Test user32 gdi32 shell32 ole32 windowscodecs version)
set_target_properties(tst_pinclipboard PROPERTIES WIN32_EXECUTABLE FALSE QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
add_test(NAME tst_pinclipboard COMMAND tst_pinclipboard -o -,txt)
set_tests_properties(tst_pinclipboard PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_FORCE_STDERR_LOGGING=1"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The cross-process clipboard view rejects malformed/unbounded UI frames.
qt_add_executable(tst_clipboarddisplay tst_clipboarddisplay.cpp
    ${PROJECT_SOURCE_DIR}/src/app/DisplayProfile.cpp)
target_include_directories(tst_clipboarddisplay PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_clipboarddisplay PRIVATE winshun_options Qt6::Core Qt6::Test)
add_test(NAME tst_clipboarddisplay COMMAND tst_clipboarddisplay)
set_tests_properties(tst_clipboarddisplay PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

# The capture's key map (src/capture/CaptureKeys.h): defaults, the settings text,
# matching, and which keys a user may give to which action (#187).
qt_add_executable(tst_capturekeys tst_capturekeys.cpp)
target_include_directories(tst_capturekeys PRIVATE ${PROJECT_SOURCE_DIR}/src/capture)
target_link_libraries(tst_capturekeys PRIVATE Qt6::Gui Qt6::Test)
set_target_properties(tst_capturekeys PROPERTIES WIN32_EXECUTABLE FALSE)
add_test(NAME tst_capturekeys COMMAND tst_capturekeys -o -,txt)
set_tests_properties(tst_capturekeys PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
