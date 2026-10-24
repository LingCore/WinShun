# ws.exe: Win顺 on the command line (docs/design-cli.md), a one-shot MCP
# client of the running Win顺 on its pipe. No Qt and the C runtime linked in,
# as the runner: one file that starts in milliseconds, alone in <install>\bin
# so that only that folder goes on PATH (the install folder has Qt's DLLs).
#
# The pipe client (src/mcp/PipeClient.cpp) and Launch.cpp are the relay's
# too, built in again here: the relay has Qt's C runtime, ws the static one,
# and the two do not link together (LNK2038). JSON is the runner's
# (wsplugin_lite, static C runtime as well).
set(ws_sources
    Args.cpp Args.h
    Clip.cpp
    Commands.cpp Commands.h
    Config.cpp Config.h
    Connect.cpp Connect.h
    Context.cpp Context.h
    Encoding.cpp Encoding.h
    Format.cpp Format.h
    Grep.cpp
    Help.cpp
    Interrupt.cpp Interrupt.h
    Messages.h
    Open.cpp Open.h
    Output.cpp Output.h
    Print.cpp Print.h
    Result.h
    Run.cpp Run.h
    Search.cpp
    Session.cpp Session.h
    Skill.cpp Skill.h
    Status.cpp
    Text.cpp Text.h
    TextCommand.cpp
    Win.cpp Win.h
)
# tst_cli builds them in too (all but main.cpp).
list(TRANSFORM ws_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/" OUTPUT_VARIABLE ws_cli_sources)
set(WS_CLI_SOURCES ${ws_cli_sources} PARENT_SCOPE)

add_executable(ws
    main.cpp
    ${ws_sources}
    ${PROJECT_SOURCE_DIR}/src/mcp/PipeClient.cpp ${PROJECT_SOURCE_DIR}/src/mcp/PipeClient.h
    ${PROJECT_SOURCE_DIR}/src/mcp/Launch.cpp ${PROJECT_SOURCE_DIR}/src/mcp/Launch.h
    ${PROJECT_SOURCE_DIR}/resources/ws.rc)
# The resource compiler does not see that SKILL.md is part of ws.rc.
set_source_files_properties(${PROJECT_SOURCE_DIR}/resources/ws.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/SKILL.md)
target_include_directories(ws PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src/mcp)
# ntdll: the parent process (Encoding.cpp). shell32, ole32 and oleaut32 only
# for `ws open` and starting Win顺: loaded when used, not on every run.
target_link_libraries(ws PRIVATE wsplugin_lite advapi32 ntdll shell32 ole32 oleaut32 uuid delayimp)
target_link_options(ws PRIVATE /DELAYLOAD:shell32.dll /DELAYLOAD:ole32.dll /DELAYLOAD:oleaut32.dll)
set_target_properties(ws PROPERTIES
    AUTOMOC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    # Alone in its folder: that folder goes on PATH, never one with Qt's DLLs.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

install(TARGETS ws RUNTIME DESTINATION bin)
