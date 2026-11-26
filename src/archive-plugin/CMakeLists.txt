# WinShunArchive.exe: the winshun.archive plugin for the plugin library
# (package/, "压缩包搜索"; not in the setup), a resident C++ plugin on the
# single-header SDK (src/plugins-sdk/winshun.h): finds files by name inside
# zip, 7z, rar, tar and other archives. No Qt, the C runtime linked in.
# Reading archives is libarchive's (cmake/Libarchive.cmake: built from its
# release tarball with zlib and liblzma, static). What it keeps of them and
# how it searches that (Index) is plain C++ that tests/tst_archive.cpp builds
# in again; the reading itself the test asks of the program (--list, --take).
winshun_libarchive()
add_executable(WinShunArchive
    main.cpp
    Index.cpp Index.h
    Lister.cpp Lister.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunArchive.rc
)
target_include_directories(WinShunArchive PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk ${PROJECT_SOURCE_DIR}/src/plugins-common)
target_link_libraries(WinShunArchive PRIVATE winshun_options wsarchive_deps psapi)
set_target_properties(WinShunArchive PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# For the staging of the package (library_plugins): the licenses of what is linked in.
set(WS_LIBARCHIVE_LICENSES "${WS_LIBARCHIVE_LICENSES}" PARENT_SCOPE)
