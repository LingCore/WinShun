# WinShunTranslate.exe: the winshun.translate plugin ("翻译") for the plugin
# library, a resident C++ plugin on the single-header SDK
# (src/plugins-sdk/winshun.h): "tr sentence" through the user's own DeepL or
# OpenAI-compatible service (WinHTTP). No Qt, the C runtime linked in. The
# logic (Translate) is plain C++ that tests/tst_translate.cpp builds in again.
add_executable(WinShunTranslate
    main.cpp
    Http.cpp Http.h
    Translate.cpp Translate.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunTranslate.rc
)
target_include_directories(WinShunTranslate PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunTranslate PRIVATE winshun_options winhttp)
set_target_properties(WinShunTranslate PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
