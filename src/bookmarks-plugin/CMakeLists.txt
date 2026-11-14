# WinShunBookmarks.exe: the winshun.bookmarks plugin ("浏览器书签") for the
# plugin library, a once C++ plugin on the single-header SDK
# (src/plugins-sdk/winshun.h): searches the bookmarks of Chrome, Edge and one
# more Chromium browser. No Qt, the C runtime linked in. The logic
# (Bookmarks) is plain C++ that tests/tst_bookmarks.cpp builds in again.
# Chinese names are found by pinyin with the app's own matcher and table
# (src/core/Pinyin, plain C++ too), built in: one table for both.
add_executable(WinShunBookmarks
    main.cpp
    Bookmarks.cpp Bookmarks.h
    ${PROJECT_SOURCE_DIR}/src/core/Pinyin.cpp ${PROJECT_SOURCE_DIR}/src/core/Pinyin.h
    ${PROJECT_SOURCE_DIR}/src/core/PinyinTable.cpp
    ${PROJECT_SOURCE_DIR}/resources/WinShunBookmarks.rc
)
target_include_directories(WinShunBookmarks PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(WinShunBookmarks PRIVATE winshun_options)
set_target_properties(WinShunBookmarks PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
