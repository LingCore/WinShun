find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE wscore Qt6::Test)

qt_add_executable(tst_clipboard tst_clipboard.cpp)
target_link_libraries(tst_clipboard PRIVATE wscore Qt6::Test)

# Colour values as text: app code that needs nothing but QColor.
qt_add_executable(tst_color tst_color.cpp ${PROJECT_SOURCE_DIR}/src/app/ColorText.cpp)
target_include_directories(tst_color PRIVATE ${PROJECT_SOURCE_DIR}/src/app)
target_link_libraries(tst_color PRIVATE Qt6::Gui Qt6::Test)

add_test(NAME tst_core COMMAND tst_core)
add_test(NAME tst_clipboard COMMAND tst_clipboard)
add_test(NAME tst_color COMMAND tst_color)
# Let ctest find the Qt DLLs without a deploy step.
set_tests_properties(tst_core tst_clipboard tst_color PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
