# The screen capture's parts that need no QML (docs/architecture.md, 截图):
# the geometry the tests check (Geometry), the pictures (Grabber: Windows.
# Graphics.Capture through C++/WinRT from the Windows SDK, BitBlt; Hdr), the
# windows on the screen (WindowSnapshot) and the PrtScn takeover
# (PrintScreen), and the annotations' core (Annotation*: the document, its
# commands, the compositor that draws tiles and exports, the project format
# and the tools' styles; docs/design-capture-roadmap.md, 3) and the editing
# window's zoom and tile arithmetic (EditView), and the smart redaction's
# rules (SmartRedact), and the long screenshot's stitching, scrolling and
# diagnostics (Scroll*; docs/design-capture-roadmap.md, 4.2), and stage 7's
# parts that need no QML: the pins' turning, collapsing and text picking
# (PinTools), the small features (CaptureTools), the QR reader (QrCode), the
# UI Automation probe (UiaProbe) and the delayed capture's countdown
# (CountdownWindow), and the keys a recording shows (GitHub #98: Keycaps, which
# keys and rows; KeycapRender, their picture; KeyStream, the hooks while
# recording). What the
# overlays bind to (CaptureSession, CaptureImage) and the controller
# (Capture) are in this folder too, but built into WinShun's QML module
# (src/app/CMakeLists.txt), which registers their types.
#
# zxing-cpp 2.3.0 (Apache-2.0, third_party/zxing-cpp: the library's core/ and
# its LICENSE as released, the writers' libzint left out) reads the QR codes
# (QrCode). Its writers are on for the tests, which make their own codes; a
# static library, so a program without them links none. EXCLUDE_FROM_ALL keeps
# its install rules out of ours.
find_package(Threads REQUIRED)
set(ZXING_READERS ON)
set(ZXING_WRITERS ON)
set(ZXING_C_API OFF)
set(ZXING_EXPERIMENTAL_API OFF)
set(ZXING_UNIT_TESTS OFF)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/zxing-cpp/core ${CMAKE_BINARY_DIR}/third_party/zxing-cpp/core EXCLUDE_FROM_ALL)
# No moc and no precompiled header for it: the project's Chinese path broke
# Ninja on the PCH's ".\\" output, and the library has nothing for moc.
set_target_properties(ZXing PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF DISABLE_PRECOMPILE_HEADERS ON)

add_library(wscapture STATIC
    Geometry.cpp Geometry.h
    Hdr.cpp Hdr.h
    Recording.cpp Recording.h
    GifExport.cpp GifExport.h
    RecordingPointer.cpp RecordingPointer.h
    Keycaps.cpp Keycaps.h
    KeycapRender.cpp KeycapRender.h
    KeyStream.cpp KeyStream.h
    PrintScreen.cpp PrintScreen.h
    WindowSnapshot.cpp WindowSnapshot.h
    Grabber.cpp Grabber.h
    EditView.cpp EditView.h
    Annotation.cpp Annotation.h
    AnnotationCommands.cpp AnnotationCommands.h
    AnnotationRender.cpp AnnotationRender.h
    AnnotationFormat.cpp AnnotationFormat.h
    AnnotationStyles.cpp AnnotationStyles.h
    SmartRedact.cpp SmartRedact.h
    ScrollStitcher.cpp ScrollStitcher.h
    ScrollInput.cpp ScrollInput.h
    ScrollGrabber.cpp ScrollGrabber.h
    ScrollDiagnostics.cpp ScrollDiagnostics.h
    QrCode.cpp QrCode.h
    PinTools.cpp PinTools.h
    CaptureTools.cpp CaptureTools.h
    UiaProbe.cpp UiaProbe.h
    CountdownWindow.cpp CountdownWindow.h
    CaptureDiagnostics.cpp CaptureDiagnostics.h
)
target_include_directories(wscapture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# miniz: the projects' zip (AnnotationFormat). ole32: UI Automation (ScrollInput).
target_link_libraries(wscapture PUBLIC winshun_options Qt6::Gui
    PRIVATE miniz ZXing::ZXing windowsapp windowscodecs d3d11 d2d1 dxgi mfplat mfreadwrite mfuuid oleaut32 dwmapi user32 gdi32 advapi32 ole32)

# Optional media components: Windows N still starts and takes screenshots.
target_link_options(wscapture INTERFACE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
target_link_libraries(wscapture PRIVATE delayimp)
