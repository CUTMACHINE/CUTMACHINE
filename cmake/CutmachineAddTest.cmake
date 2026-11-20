# STRUCTURE-2026-11 ST2 -- docs/roadmap/STRUCTURE-2026-11.md. A suite in one
# call: its executable, built from SOURCES, and the test that runs it.
#
#   cutmachine_add_test(<name>
#       SOURCES <file>...          the executable's sources
#       [INCLUDES <dir>...]        target_include_directories, PRIVATE
#       [OPTIONS <option>...]      target_compile_options, PRIVATE
#       [DEFINITIONS <def>...]     target_compile_definitions, PRIVATE
#       [LIBS <library>...]        target_link_libraries, PRIVATE
#       [DEPENDS <target>...]      add_dependencies
#       [ARGS <argument>...]       after the command: add_test(NAME <name>
#                                  COMMAND <name> <argument>...)
#       [LABELS <label>...])       the test's LABELS
#
# The executable and the test both take <name>; what a suite needs beyond
# this (an environment, a timeout, a lock) is set after the call, on that
# name. headless/portable_tests.py reads these calls as it reads the plain
# ones: what it carries over to the build without a window is unchanged.
function(cutmachine_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 SUITE ""  ""
        "SOURCES;INCLUDES;OPTIONS;DEFINITIONS;LIBS;DEPENDS;ARGS;LABELS")
    if(SUITE_UNPARSED_ARGUMENTS OR NOT SUITE_SOURCES)
        message(FATAL_ERROR "cutmachine_add_test(${name}): SOURCES expected, "
                            "and nothing but its keywords: "
                            "${SUITE_UNPARSED_ARGUMENTS}")
    endif()
    add_executable(${name} ${SUITE_SOURCES})
    if(SUITE_INCLUDES)
        target_include_directories(${name} PRIVATE ${SUITE_INCLUDES})
    endif()
    if(SUITE_OPTIONS)
        target_compile_options(${name} PRIVATE ${SUITE_OPTIONS})
    endif()
    if(SUITE_DEFINITIONS)
        target_compile_definitions(${name} PRIVATE ${SUITE_DEFINITIONS})
    endif()
    if(SUITE_LIBS)
        target_link_libraries(${name} PRIVATE ${SUITE_LIBS})
    endif()
    if(SUITE_DEPENDS)
        add_dependencies(${name} ${SUITE_DEPENDS})
    endif()
    add_test(NAME ${name} COMMAND ${name} ${SUITE_ARGS})
    if(SUITE_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${SUITE_LABELS}")
    endif()
endfunction()
