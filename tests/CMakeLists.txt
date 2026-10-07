find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_core tst_core.cpp)
target_link_libraries(tst_core PRIVATE qfcore Qt6::Test)

add_test(NAME tst_core COMMAND tst_core)
# Let ctest find the Qt DLLs without a deploy step.
set_tests_properties(tst_core PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
