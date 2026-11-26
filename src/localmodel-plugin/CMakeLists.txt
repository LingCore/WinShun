# WinShunLocalModel.exe: the winshun.localmodel plugin for the plugin library
# (package/, "本地模型"; not in the setup), a resident C++ plugin on the
# single-header SDK (src/plugins-sdk/winshun.h): runs a model (GGUF) on this
# computer with llama.cpp's llama-server and offers it as an OpenAI-compatible
# API on 127.0.0.1, which the translation plugin's "本地模型" service uses.
# No Qt, the C runtime linked in. The parts that are plain C++ (wslocalmodel)
# tests/tst_localmodel.cpp links in again.

# llama.cpp's own Windows build (MIT; ggml-org/llama.cpp release), the Vulkan
# one: it runs on any GPU with a Vulkan driver (NVIDIA, AMD, Intel) and on
# the CPU (ggml-cpu-*.dll, the one for this CPU picked at run time). It is
# downloaded once per build folder and staged into the package's llama\.
set(LLAMA_CPP_BUILD b12359)
set(LLAMA_CPP_SHA256 e8d18c6e4210cca87e7307c2ea78363ac8d158e4e1397358ec96ab70b53a0d5e)
set(LLAMA_CPP_DIR "${CMAKE_BINARY_DIR}/_deps/llama-${LLAMA_CPP_BUILD}-win-vulkan-x64")
if(NOT EXISTS "${LLAMA_CPP_DIR}/llama-server.exe")
    set(archive "${CMAKE_BINARY_DIR}/_deps/llama-${LLAMA_CPP_BUILD}-bin-win-vulkan-x64.zip")
    winshun_download(
        "https://github.com/ggml-org/llama.cpp/releases/download/${LLAMA_CPP_BUILD}/llama-${LLAMA_CPP_BUILD}-bin-win-vulkan-x64.zip"
        "${archive}" "${LLAMA_CPP_SHA256}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${LLAMA_CPP_DIR}")
    file(REMOVE "${archive}")
endif()
# What llama-server.exe loads (dumpbin /dependents, and ggml's backends it
# looks for next to itself); not the other tools, not ggml-rpc.dll (only
# for --rpc). vulkan-1.dll comes with the GPU's driver: without it the
# Vulkan backend is skipped and the CPU does the work.
file(GLOB llama_cpu_backends "${LLAMA_CPP_DIR}/ggml-cpu-*.dll")
set(llama_files
    "${LLAMA_CPP_DIR}/llama-server.exe" "${LLAMA_CPP_DIR}/llama-server-impl.dll" "${LLAMA_CPP_DIR}/llama-common.dll"
    "${LLAMA_CPP_DIR}/llama.dll" "${LLAMA_CPP_DIR}/mtmd.dll" "${LLAMA_CPP_DIR}/ggml.dll" "${LLAMA_CPP_DIR}/ggml-base.dll"
    "${LLAMA_CPP_DIR}/ggml-vulkan.dll" "${LLAMA_CPP_DIR}/libomp.dll" ${llama_cpu_backends})
foreach(file IN LISTS llama_files)
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "llama.cpp ${LLAMA_CPP_BUILD} lacks ${file}")
    endif()
endforeach()
# llama.cpp's build uses the C++ runtime as DLLs: it goes next to it (the
# package's own copy; a PC without the VC++ redistributable has none).
set(llama_crt "")
if(DEFINED ENV{VCToolsRedistDir})
    file(GLOB llama_crt_dirs "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CRT")
    list(SORT llama_crt_dirs ORDER DESCENDING)
    foreach(dir IN LISTS llama_crt_dirs)
        if(EXISTS "${dir}/vcruntime140.dll")
            file(GLOB llama_crt "${dir}/msvcp140*.dll" "${dir}/vcruntime140*.dll")
            break()
        endif()
    endforeach()
endif()
if(NOT llama_crt)
    message(FATAL_ERROR "No x64 VC++ runtime (msvcp140.dll) under VCToolsRedistDir: build from scripts/devshell.ps1")
endif()
set(WS_LLAMA_FILES ${llama_files} ${llama_crt} PARENT_SCOPE)
set(WS_LLAMA_LICENSE_OPENMP "${LLAMA_CPP_DIR}/LICENSE-LLVM-OpenMP" PARENT_SCOPE)

# The logic, without the protocol loop: compiled into the program (the C
# runtime linked in) and, as wslocalmodel, into tst_localmodel (Qt's runtime).
set(localmodel_sources
    Catalog.cpp Catalog.h
    Download.cpp Download.h
    Engine.cpp Engine.h
    LlamaServer.cpp LlamaServer.h
    Relay.cpp Relay.h
    ${PROJECT_SOURCE_DIR}/src/localsend-plugin/HttpLite.cpp ${PROJECT_SOURCE_DIR}/src/localsend-plugin/HttpLite.h # request heads with limits
)
set(localmodel_libraries ws2_32 winhttp bcrypt psapi)

add_executable(WinShunLocalModel
    main.cpp
    ${localmodel_sources}
    ${PROJECT_SOURCE_DIR}/resources/WinShunLocalModel.rc
)
target_include_directories(WinShunLocalModel PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk ${PROJECT_SOURCE_DIR}/src/localsend-plugin)
target_link_libraries(WinShunLocalModel PRIVATE winshun_options ${localmodel_libraries})
set_target_properties(WinShunLocalModel PROPERTIES AUTOMOC OFF MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(wslocalmodel STATIC ${localmodel_sources})
target_include_directories(wslocalmodel
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/src/plugins-sdk ${PROJECT_SOURCE_DIR}/src/localsend-plugin)
target_link_libraries(wslocalmodel PUBLIC winshun_options ${localmodel_libraries})
set_target_properties(wslocalmodel PROPERTIES AUTOMOC OFF)
