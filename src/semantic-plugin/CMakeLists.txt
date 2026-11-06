# WinShunSemantic.exe: the winshun.semantic plugin that comes with Win顺
# (plugins/winshun.semantic, "按意思找"), a resident C++ plugin on the
# single-header SDK (src/plugins-sdk/winshun.h). It takes the text of
# documents from Win顺's content index, has a model of the user's choosing
# (an OpenAI-compatible /embeddings endpoint, WinHTTP) turn it into vectors,
# and keeps and searches those itself. No Qt, the C runtime linked in.
add_executable(WinShunSemantic
    main.cpp
    Chunker.cpp Chunker.h
    Embedder.cpp Embedder.h
    Store.cpp Store.h
    ${PROJECT_SOURCE_DIR}/resources/WinShunSemantic.rc
)
target_include_directories(WinShunSemantic PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk)
target_link_libraries(WinShunSemantic PRIVATE winshun_options winhttp psapi)
set_target_properties(WinShunSemantic PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
