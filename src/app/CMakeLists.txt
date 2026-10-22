if(APPLE)
  set(OS_BUNDLE MACOSX_BUNDLE)
elseif(WIN32)
  set(OS_BUNDLE WIN32)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/documentation.qrc.in ${DOCUMENTATION_RESOURCE} @ONLY)

add_custom_command(
  OUTPUT ${DOCUMENTATION_HTML}
  COMMAND $<TARGET_FILE:maddy> ${DOCUMENTATION_FILE} ${DOCUMENTATION_HTML}
  DEPENDS maddy ${DOCUMENTATION_FILE}
)

set(TS_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/i18n/en.ts
  ${CMAKE_CURRENT_SOURCE_DIR}/i18n/zh_CN.ts
)

find_package(Qt${QT_VERSION_MAJOR}LinguistTools REQUIRED)
if (${QT_VERSION_MAJOR} EQUAL 5)
  qt5_add_translation(QM_FILES ${TS_FILES})
else (${QT_VERSION_MAJOR} EQUAL 6)
  qt6_add_translation(QM_FILES ${TS_FILES})
endif()

if (NOT TARGET Qt${QT_VERSION_MAJOR}::lupdate)
    message(FATAL_ERROR "The package \"Qt${QT_VERSION_MAJOR}LinguistTools\" is required.")
endif()
set(Qt_LUPDATE_EXECUTABLE Qt${QT_VERSION_MAJOR}::lupdate)
set(Qt_LCONVERT_EXECUTABLE Qt${QT_VERSION_MAJOR}::lconvert)
add_custom_target(lupdate
    COMMAND ${Qt_LUPDATE_EXECUTABLE} -recursive -no-obsolete ${CMAKE_SOURCE_DIR}/src -ts ${TS_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src
    COMMENT "Updating translations"
)

include(TranslationUtils)
add_app_translations_resource(KLOGG_I18N_RES ${QM_FILES})
add_qt_translations_resource(KLOGG_QT_TRANSLATION_RES zh_CN)

set(MAIN_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cli.h
                 ${CMAKE_CURRENT_SOURCE_DIR}/klogg.qrc 
                 ${KLOGG_I18N_RES}
                 ${KLOGG_QT_TRANSLATION_RES})

set(KLOGG_UI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/messagereceiver.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kloggapp.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${DOCUMENTATION_RESOURCE}
    ${ICON_FILE}
)

set(KLOGG_GREP_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/klogg_grep.cpp)

set(MAIN_LIBS
    klogg_logdata
    klogg_crash_handler
    roaring
    kdsingleapp
)

add_executable(flogg ${OS_BUNDLE} ${MAIN_SOURCES} ${KLOGG_UI_SOURCES})
add_executable(flogg_portable ${OS_BUNDLE} ${MAIN_SOURCES} ${KLOGG_UI_SOURCES})
add_executable(flogg_grep ${MAIN_SOURCES} ${KLOGG_GREP_SOURCES})

add_dependencies(ci_build flogg flogg_grep)

if(WIN32)
  add_dependencies(ci_build flogg_portable)
endif()

set_target_properties(flogg PROPERTIES AUTORCC ON)
set_target_properties(flogg PROPERTIES AUTOMOC ON)
set_target_properties(flogg_portable PROPERTIES AUTORCC ON)
set_target_properties(flogg_portable PROPERTIES AUTOMOC ON)
set_target_properties(flogg_grep PROPERTIES AUTORCC ON)
set_target_properties(flogg_grep PROPERTIES AUTOMOC ON)

if(KLOGG_USE_LTO)
  set_property(TARGET flogg PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET flogg_portable PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET flogg_grep PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_link_libraries(flogg PUBLIC ${MAIN_LIBS} klogg_ui)
target_link_libraries(flogg_portable PUBLIC ${MAIN_LIBS} klogg_ui)
target_link_libraries(flogg_grep PUBLIC ${MAIN_LIBS})

target_compile_definitions(flogg_portable PUBLIC -DKLOGG_PORTABLE)

if(WIN32)

  target_sources(flogg PRIVATE ${ProductVersionResourceFiles})
  target_sources(flogg_portable PRIVATE ${ProductVersionResourceFiles})
  target_sources(flogg_grep PRIVATE ${ProductVersionResourceFiles})

elseif(APPLE)
  set_source_files_properties(${ICON_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_source_files_properties(${NOTICE_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)
  set_source_files_properties(${COPYING_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)

  set_source_files_properties(${README_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)
  set_source_files_properties(${DOCUMENTATION_FILE} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)
  set_source_files_properties(${DOCUMENTATION_HTML} PROPERTIES MACOSX_PACKAGE_LOCATION SharedSupport)

  set(MACOSX_BUNDLE_BUNDLE_DISPLAY_NAME ${PROJECT_DESCRIPTION})
  set(MACOSX_BUNDLE_BUNDLE_NAME ${PROJECT_NAME})
  set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_LONG_VERSION_STRING ${PROJECT_VERSION})
  set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
  set(MACOSX_BUNDLE_COPYRIGHT ${COPYRIGHT})
  set(MACOSX_BUNDLE_GUI_IDENTIFIER ${IDENTIFIER})
  set(MACOSX_BUNDLE_ICON_FILE ${ICON_NAME})

endif()

install(
  TARGETS flogg
  DESTINATION ${CMAKE_INSTALL_BINDIR}
  BUNDLE DESTINATION .
)

if(WIN32)
  if(TARGET tbb)
    add_custom_command(
      TARGET flogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:tbb> $<TARGET_FILE_DIR:flogg>
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy tbb"
    )
  endif()
  if(TARGET tbbmalloc_proxy)
    add_custom_command(
      TARGET flogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:tbbmalloc> $<TARGET_FILE_DIR:flogg>
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy tbbmalloc"
    )
    add_custom_command(
      TARGET flogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:tbbmalloc_proxy> $<TARGET_FILE_DIR:flogg>
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy tbbmalloc_proxy"
    )
  endif()
endif()

if(KLOGG_USE_SENTRY)
  if(NOT WIN32)
    if(APPLE)
      find_program(DSYMUTIL dsymutil)
      add_custom_command(
        TARGET flogg
        POST_BUILD
        COMMAND ${DSYMUTIL} --out=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/flogg.dSym $<TARGET_FILE:flogg>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:flogg>"
      )

      add_custom_command(
        TARGET flogg_portable
        POST_BUILD
        COMMAND ${DSYMUTIL} --out=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/flogg_portable.dSym $<TARGET_FILE:flogg_portable>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:flogg_portable>"
      )
    else()
      find_program(OBJCOPY objcopy)
      add_custom_command(
        TARGET flogg
        POST_BUILD
        COMMAND ${OBJCOPY} --only-keep-debug --compress-debug-sections=zlib $<TARGET_FILE:flogg>
                ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/flogg.debug
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:flogg>"
      )
      add_custom_command(
        TARGET flogg
        POST_BUILD
        COMMAND ${OBJCOPY} --strip-debug --strip-unneeded $<TARGET_FILE:flogg>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Stripping $<TARGET_FILE:flogg>"
      )
      add_custom_command(
        TARGET flogg
        POST_BUILD
        COMMAND ${OBJCOPY} --add-gnu-debuglink=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/flogg.debug $<TARGET_FILE:flogg>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Linking symbols for $<TARGET_FILE:flogg>"
      )

      add_custom_command(
        TARGET flogg_portable
        POST_BUILD
        COMMAND ${OBJCOPY} --only-keep-debug $<TARGET_FILE:flogg_portable>
                ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/flogg_portable.debug
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating symbols for $<TARGET_FILE:flogg>"
      )
      add_custom_command(
        TARGET flogg_portable
        POST_BUILD
        COMMAND ${OBJCOPY} --strip-debug --strip-unneeded $<TARGET_FILE:flogg_portable>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Stripping $<TARGET_FILE:flogg_portable>"
      )
      add_custom_command(
        TARGET flogg_portable
        POST_BUILD
        COMMAND ${OBJCOPY} --add-gnu-debuglink=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/flogg_portable.debug
                $<TARGET_FILE:flogg_portable>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Linking symbols for $<TARGET_FILE:flogg_portable>"
      )
    endif()

    add_custom_command(
      TARGET flogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:crashpad_handler> $<TARGET_FILE_DIR:flogg>/klogg_crashpad_handler
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy crash_handler"
    )

    add_custom_command(
      TARGET flogg
      POST_BUILD
      COMMAND cmake -E copy ${SYMS_TOOLS}/minidump_dump $<TARGET_FILE_DIR:flogg>/klogg_minidump_dump
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy stackwalk"
    )
  else()
    add_custom_command(
      TARGET flogg
      POST_BUILD
      COMMAND cmake -E copy $<TARGET_FILE:crashpad_handler> $<TARGET_FILE_DIR:flogg>/klogg_crashpad_handler.exe
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Copy crash_handler"
    )

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      add_custom_command(
        TARGET flogg
        POST_BUILD
        COMMAND cmake -E copy ${SYMS_TOOLS}/minidump_dump_x64.exe $<TARGET_FILE_DIR:flogg>/klogg_minidump_dump.exe
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Copy stackwalk"
      )
    else()
      add_custom_command(
        TARGET flogg
        POST_BUILD
        COMMAND cmake -E copy ${SYMS_TOOLS}/minidump_dump_x86.exe $<TARGET_FILE_DIR:flogg>/klogg_minidump_dump.exe
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Copy stackwalk"
      )
    endif()
  endif()

  if(APPLE)
    install(PROGRAMS $<TARGET_FILE_DIR:flogg>/klogg_minidump_dump $<TARGET_FILE_DIR:flogg>/klogg_crashpad_handler
            DESTINATION $<TARGET_BUNDLE_CONTENT_DIR:flogg>/MacOS
    )
  elseif(NOT WIN32)
    install(PROGRAMS $<TARGET_FILE_DIR:flogg>/klogg_minidump_dump $<TARGET_FILE_DIR:flogg>/klogg_crashpad_handler
            DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
  endif()
endif(KLOGG_USE_SENTRY)
