# WinShunBookmarks.exe: the winshun.bookmarks plugin ("浏览器书签") for the
# plugin library, a once C++ plugin on the single-header SDK
# (src/plugins-sdk/winshun.h): searches the bookmarks of Chrome, Edge and one
# more Chromium browser. No Qt, the C runtime linked in. The logic
# (Bookmarks) is plain C++ that tests/tst_bookmarks.cpp builds in again.
add_executable(WinShunBookmarks
    main.cpp
    Bookmarks.cpp Bookmarks.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunBookmarks.rc
)
target_include_directories(WinShunBookmarks PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunBookmarks PRIVATE winshun_options)
set_target_properties(WinShunBookmarks PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
