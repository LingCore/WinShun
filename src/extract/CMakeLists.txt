# Reading the text out of documents (Word, Excel, PowerPoint, PDF). The
# parsers (wsextract) run in WinShunExtract.exe, a sandboxed process the app
# starts (src/core/DocExtractor), never in the app itself; tools and tests
# link them directly. The app shares only the headers of what goes over the
# pipe (wsextract_api).

# PDFium, prebuilt (github.com/bblanchon/pdfium-binaries, BSD-3 and Apache-2.0,
# its third-party licenses in licenses/), downloaded once per build folder.
set(PDFIUM_BUILD 8086)
set(PDFIUM_SHA256 1fd8af952832dbb0eb16d9249f68fe09e5f5ebf7c3dd9f6066ea2720cc28487d)
set(PDFIUM_DIR "${CMAKE_BINARY_DIR}/_deps/pdfium-${PDFIUM_BUILD}" CACHE INTERNAL "")
if(NOT EXISTS "${PDFIUM_DIR}/bin/pdfium.dll")
    set(archive "${CMAKE_BINARY_DIR}/_deps/pdfium-win-x64-${PDFIUM_BUILD}.tgz")
    file(DOWNLOAD
        "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/${PDFIUM_BUILD}/pdfium-win-x64.tgz"
        "${archive}" EXPECTED_HASH SHA256=${PDFIUM_SHA256} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Could not download PDFium: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${PDFIUM_DIR}")
endif()
add_library(pdfium SHARED IMPORTED GLOBAL)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION "${PDFIUM_DIR}/bin/pdfium.dll"
    IMPORTED_IMPLIB "${PDFIUM_DIR}/lib/pdfium.dll.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_DIR}/include")
# Next to every program of the build (they all go to the build folder).
file(COPY "${PDFIUM_DIR}/bin/pdfium.dll" DESTINATION "${CMAKE_BINARY_DIR}")

add_library(miniz STATIC ${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC ${PROJECT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

add_library(wsextract_api INTERFACE)
target_include_directories(wsextract_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_library(wsextract STATIC
    Cfb.cpp Cfb.h
    Doc.cpp
    DocText.h
    Extract.cpp Extract.h
    Formats.h
    Markup.cpp
    Ooxml.cpp
    Pdf.cpp
    Ppt.cpp
    Protocol.h
    Source.h
    Writer.cpp Writer.h
    Xls.cpp
    Xml.cpp Xml.h
    Zip.cpp Zip.h
)
target_link_libraries(wsextract
    PUBLIC wsextract_api winshun_options
    PRIVATE miniz pdfium)
# pdfium.dll is loaded with the first PDF (Pdf.cpp), so a program starts without it.
target_link_libraries(wsextract INTERFACE delayimp)
target_link_options(wsextract INTERFACE /DELAYLOAD:pdfium.dll)

add_executable(WinShunExtract WIN32 main.cpp)
target_link_libraries(WinShunExtract PRIVATE wsextract)

install(TARGETS WinShunExtract RUNTIME DESTINATION .)
install(FILES "${PDFIUM_DIR}/bin/pdfium.dll" DESTINATION .)
