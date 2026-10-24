# Win顺 as an MCP server (Model Context Protocol): AI clients (Claude
# Desktop, Claude Code, Cursor, VS Code) start WinShunMcp.exe over stdio,
# which relays the JSON-RPC messages to the running WinShun over a named
# pipe, where the protocol and the tools live (McpServer, McpTools). The
# command line ws.exe (src/cli) talks on the same pipe.
#
# wsmcp_proto: the message framing and the pipe's name and security, shared
# by the relay and WinShun; Qt Core and Win32 only, so the relay stays small
# and starts fast. PipeClient.cpp is Qt-free: ws.exe builds it in again.
qt_add_library(wsmcp_proto STATIC
    JsonRpc.cpp JsonRpc.h
    LineChannel.cpp LineChannel.h
    Pipe.cpp Pipe.h
    PipeClient.cpp PipeClient.h
)
target_include_directories(wsmcp_proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wsmcp_proto PUBLIC Qt6::Core winshun_options PRIVATE advapi32)

# wsmcp: the server side, on top of the search engine (wscore).
qt_add_library(wsmcp STATIC
    Aside.cpp Aside.h
    McpServer.cpp McpServer.h
    McpTools.cpp McpTools.h
    PipeServer.cpp PipeServer.h
    Visibility.cpp Visibility.h
)
target_link_libraries(wsmcp PUBLIC wscore wsmcp_proto PRIVATE user32) # user32: GetLastInputInfo

# WinShunMcp.exe: the stdio relay the AI clients run (see relay/main.cpp).
# A console program, as MCP's stdio transport wants; runs as the invoker, not
# elevated like WinShun. Launch.cpp (is Win顺 running, starting it) is
# Qt-free: ws.exe builds it in again.
qt_add_executable(WinShunMcp relay/main.cpp Launch.cpp Launch.h ${PROJECT_SOURCE_DIR}/resources/WinShunMcp.rc)
set_target_properties(WinShunMcp PROPERTIES WIN32_EXECUTABLE OFF)
target_link_libraries(WinShunMcp PRIVATE wsmcp_proto)

install(TARGETS WinShunMcp RUNTIME DESTINATION .)
