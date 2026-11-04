set(app_dir ${PROJECT_SOURCE_DIR}/src/app)
qt_add_library(wsocrsearch STATIC OcrStore.cpp OcrStore.h OcrSource.cpp OcrSource.h
    OcrWorker.cpp OcrWorker.h
    ${app_dir}/platform/PaddleOcr.cpp ${app_dir}/platform/OcrFiles.cpp ${app_dir}/PictureFile.cpp)
target_include_directories(wsocrsearch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${app_dir})
target_link_libraries(wsocrsearch PUBLIC wscore Qt6::Gui Qt6::Sql PRIVATE wsextract pdfium onnxruntime_headers windowscodecs)
# Traditional to simplified Chinese characters for search: OpenCC's
# data/dictionary/TSCharacters.txt (Apache-2.0) as published at commit
# 5c764a5a886f46eb365656ed91a0410d7162fac6, unchanged (SHA-256
# 9ff46a7d30e5765375eb13d33f2b03a34d298913caf2b120380679f33ae1642d).
# Its license goes into the package (licenses/opencc.txt).
qt_add_resources(wsocrsearch "ocr_search_opencc" PREFIX /ocr-search BASE "${PROJECT_SOURCE_DIR}/third_party/opencc"
    FILES "${PROJECT_SOURCE_DIR}/third_party/opencc/TSCharacters.txt")

add_executable(WinShunOcrSearch WIN32 stub/main.cpp ${PROJECT_SOURCE_DIR}/resources/WinShunOcrSearch.rc)
target_link_libraries(WinShunOcrSearch PRIVATE winshun_options)
target_link_options(WinShunOcrSearch PRIVATE /ENTRY:wmainCRTStartup)
set_target_properties(WinShunOcrSearch PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(ocr_qml)
foreach(name IN ITEMS Theme Hook UiScale WindowEdge HoverTip DropMenu Glyph TitleBar ToggleSwitch FlatButton InputBox
    ListScrollBar ListWheelScroll ScrollPixelAlignment)
    list(APPEND ocr_qml "${app_dir}/qml/${name}.qml")
endforeach()
foreach(name IN ITEMS OcrSearchWindow OcrLibraryPane OcrFolderRow OcrIssuesPopup OcrSearchPane OcrResultRow OcrPreviewPane)
    list(APPEND ocr_qml "${CMAKE_CURRENT_SOURCE_DIR}/qml/${name}.qml")
endforeach()
foreach(file IN LISTS ocr_qml)
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
list(APPEND ocr_qml ${theme_qml})
file(GLOB theme_images CONFIGURE_DEPENDS "${app_dir}/qml/themes/*/*.png")
foreach(file IN LISTS theme_images)
    file(RELATIVE_PATH alias "${app_dir}/qml" "${file}")
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
endforeach()
qt_add_library(WinShunOcrSearchApp SHARED OcrMain.cpp OcrController.cpp OcrController.h
    ${PROJECT_SOURCE_DIR}/src/capture-plugin/Protocol.cpp
    ${app_dir}/SystemTheme.cpp ${app_dir}/SystemTheme.h
    ${app_dir}/PhosphorGlyph.cpp ${app_dir}/PhosphorGlyph.h
    ${app_dir}/WindowFrame.cpp ${app_dir}/WindowFrame.h
    ${app_dir}/Phosphor.cpp ${app_dir}/platform/WindowEffects.cpp ${app_dir}/platform/ShellDialogs.cpp
    ${app_dir}/platform/Foreground.cpp ${app_dir}/platform/DragOut.cpp)
qt_add_qml_module(WinShunOcrSearchApp URI WinShun VERSION 1.0 NO_PLUGIN NO_IMPORT_SCAN NO_GENERATE_EXTRA_QMLDIRS
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/WinShun" QML_FILES ${ocr_qml} RESOURCES ${theme_images})
set_target_properties(WinShunOcrSearchApp PROPERTIES QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode")
target_include_directories(WinShunOcrSearchApp PRIVATE ${app_dir})
target_link_libraries(WinShunOcrSearchApp PRIVATE wsocrsearch Qt6::Qml Qt6::Quick
    advapi32 comctl32 dwmapi gdi32 imm32 ole32 oleaut32 psapi shell32 shcore shlwapi user32 uxtheme)
qt_add_resources(WinShunOcrSearchApp "ocr_search_icons" PREFIX /fonts BASE "${PROJECT_SOURCE_DIR}/third_party/phosphor"
    FILES "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor.ttf" "${PROJECT_SOURCE_DIR}/third_party/phosphor/Phosphor-Fill.ttf")

qt_add_lupdate(WinShunOcrSearchApp LUPDATE_TARGET ocr_search_lupdate
    TS_FILES i18n/ocr-search_en.ts SOURCES OcrSource.cpp OcrWorker.cpp OcrStore.cpp)
set(qm_file "${CMAKE_CURRENT_BINARY_DIR}/ocr-search_en.qm")
file(RELATIVE_PATH qm_relative "${CMAKE_CURRENT_SOURCE_DIR}" "${qm_file}")
add_custom_command(OUTPUT "${qm_file}"
    COMMAND Qt6::lrelease i18n/ocr-search_en.ts -qm "${qm_relative}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/i18n/ocr-search_en.ts"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
qt_add_resources(WinShunOcrSearchApp "ocr_search_translation" PREFIX /i18n
    BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES "${qm_file}")
