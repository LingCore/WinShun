cmake_minimum_required(VERSION 3.21)
project(desktop_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Sql Test)
get_filename_component(repo "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(built "${CMAKE_CURRENT_SOURCE_DIR}/../host")
add_executable(probe main.cpp)
target_compile_definitions(probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(probe PRIVATE /utf-8)
target_include_directories(probe PRIVATE "${repo}/src/plugin" "${repo}/src/core" "${repo}/src/mcp" "${repo}/src/extract")
target_link_libraries(probe PRIVATE
    "${built}/src/plugin/wsplugin.lib" "${built}/src/mcp/wsmcp.lib"
    "${built}/src/core/wscore.lib" "${built}/src/mcp/wsmcp_proto.lib" "${built}/src/extract/miniz.lib"
    "${built}/src/core/CMakeFiles/wscore_resources_1.dir/.qt/rcc/qrc_places_init.cpp.obj"
    "${built}/src/core/CMakeFiles/wscore_resources_2.dir/.qt/rcc/qrc_apps_init.cpp.obj"
    Qt6::Core Qt6::Sql Qt6::Test cabinet cfgmgr32 netapi32 ws2_32 advapi32 bcrypt
    user32 shlwapi shell32 ole32 version mpr userenv)
