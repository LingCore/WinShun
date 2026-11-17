# WinShunBookmarks.exe: the winshun.bookmarks plugin ("浏览器书签") for the
# plugin library, a once C++ plugin on the single-header SDK
# (src/plugins-sdk/winshun.h): searches the bookmarks of Chrome, Edge, Firefox
# and one more browser. No Qt, the C runtime linked in. The logic
# (Bookmarks) is plain C++ that tests/tst_bookmarks.cpp builds in again.
# Chinese names are found by pinyin with the app's own matcher and table
# (src/core/Pinyin, plain C++ too), built in: one table for both.
# The browsing history (History) is read with Windows' own SQLite
# (winsqlite3.dll, Windows 10 and later): nothing is shipped for it.
add_executable(WinShunBookmarks
    main.cpp
    Bookmarks.cpp Bookmarks.h
    History.cpp History.h
    Firefox.cpp Firefox.h
    ${PROJECT_SOURCE_DIR}/src/core/Pinyin.cpp ${PROJECT_SOURCE_DIR}/src/core/Pinyin.h
    ${PROJECT_SOURCE_DIR}/src/core/PinyinTable.cpp
    ${PROJECT_SOURCE_DIR}/resources/WinShunBookmarks.rc
)
target_include_directories(WinShunBookmarks PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk ${PROJECT_SOURCE_DIR}/src/core)
target_link_libraries(WinShunBookmarks PRIVATE winshun_options winsqlite3)
set_target_properties(WinShunBookmarks PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
