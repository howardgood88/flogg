set(UI_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/qtests_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logdata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/logfiltereddata_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/crawlerwidget_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/filtersearch_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/filetree_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/renderview_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/editview_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/updatechecker_test.cpp
)

if(NOT APPLE)
    LIST(APPEND UI_TEST_SOURCES  ${CMAKE_CURRENT_SOURCE_DIR}/mainwindow_test.cpp)
endif()

add_executable(klogg_itests ${UI_TEST_SOURCES} ${BACKWARD_ENABLE})

target_link_libraries(klogg_itests klogg_ui klogg_utils Catch2 Qt${QT_VERSION_MAJOR}::Test test_utils)
# The forms of klogg_ui (ui_optionsdialog.h), for the dialogs made in the tests
target_include_directories(klogg_itests PRIVATE ${CMAKE_BINARY_DIR}/src/ui/klogg_ui_autogen/include)
set_target_properties(klogg_itests PROPERTIES AUTOMOC ON)

add_backward(klogg_itests)

add_test(
    NAME klogg_itests
    COMMAND klogg_itests -platform offscreen
)