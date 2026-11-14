# PRD19 -- ROADMAP.md. cmake/BuildInfo.cmake, run as the build runs it,
# against checkouts made here: the commit and its dirty flag are read from
# the repository, a directory that is not one gives "unknown", and a second
# run with nothing changed leaves the generated source untouched -- which is
# what keeps a build at the same commit from recompiling it.
#
#   cmake -DSCRIPT=<cmake/BuildInfo.cmake> -DTEMPLATE=<src/BuildInfo.cc.in>
#         -DWORK_DIR=<scratch directory> -DGIT_EXECUTABLE=<git>
#         -P build_info_script_tests.cmake

cmake_minimum_required(VERSION 3.24)

set(_failures 0)

function(check condition_text)
    if(NOT (${ARGN}))
        message(SEND_ERROR "FAIL: ${condition_text}")
        math(EXPR _count "${_failures} + 1")
        set(_failures "${_count}" PARENT_SCOPE)
    endif()
endfunction()

# The repository below is made without the user's git configuration: a
# signing key, a template or a hook must not decide whether a commit works.
set(_git "${CMAKE_COMMAND}" -E env GIT_CONFIG_GLOBAL=/dev/null
         GIT_CONFIG_NOSYSTEM=1 "${GIT_EXECUTABLE}")

function(git directory)
    execute_process(
        COMMAND ${_git} -C "${directory}" -c user.name=cutmachine-tests
                -c user.email=tests@cutmachine.invalid -c commit.gpgsign=false
                ${ARGN}
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _status)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed in ${directory}: ${_error}")
    endif()
    set(GIT_OUTPUT "${_output}" PARENT_SCOPE)
endfunction()

# Runs the script on `directory` and reads back what it wrote.
function(read_build_info directory git_executable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${directory}"
                "-DTEMPLATE=${TEMPLATE}" "-DOUTPUT=${WORK_DIR}/BuildInfo.cc"
                -DVERSION=4.5.6 "-DGIT_EXECUTABLE=${git_executable}"
                -P "${SCRIPT}"
        RESULT_VARIABLE _status
        ERROR_VARIABLE _error)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "BuildInfo.cmake failed: ${_error}")
    endif()
    file(READ "${WORK_DIR}/BuildInfo.cc" _source)
    if(NOT _source MATCHES
       "build{\"([^\"]*)\",[ \n]*\"([^\"]*)\",[ \n]*(true|false)}")
        message(FATAL_ERROR "unexpected generated source:\n${_source}")
    endif()
    set(BUILD_VERSION "${CMAKE_MATCH_1}" PARENT_SCOPE)
    set(BUILD_COMMIT "${CMAKE_MATCH_2}" PARENT_SCOPE)
    set(BUILD_DIRTY "${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()

# Gives the generated source a date in the year 2000. A run after which it
# still carries that year did not write it.
function(age_output)
    execute_process(COMMAND touch -t 200006151200 "${WORK_DIR}/BuildInfo.cc"
                    RESULT_VARIABLE _status)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR "unable to age the generated source")
    endif()
endfunction()

function(output_year variable)
    file(TIMESTAMP "${WORK_DIR}/BuildInfo.cc" _year "%Y")
    set(${variable} "${_year}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/archive" "${WORK_DIR}/repository")

# A directory that is no checkout of its own: an archive of the sources, or
# sources unpacked inside some other repository -- which is what this one is
# when the build tree lives in the project's checkout.
file(WRITE "${WORK_DIR}/archive/source.cc" "int archived;\n")
read_build_info("${WORK_DIR}/archive" "${GIT_EXECUTABLE}")
check("sources that are not a checkout have no commit: ${BUILD_COMMIT}"
      BUILD_COMMIT STREQUAL "unknown" AND BUILD_DIRTY STREQUAL "false")
check("the version is the one the build passes: ${BUILD_VERSION}"
      BUILD_VERSION STREQUAL "4.5.6")

git("${WORK_DIR}/repository" init --quiet)
file(WRITE "${WORK_DIR}/repository/source.cc" "int first;\n")
git("${WORK_DIR}/repository" add source.cc)
git("${WORK_DIR}/repository" commit --quiet -m first)
git("${WORK_DIR}/repository" rev-parse --short HEAD)
set(_first "${GIT_OUTPUT}")

read_build_info("${WORK_DIR}/repository" "${GIT_EXECUTABLE}")
check("a clean checkout gives its commit: ${BUILD_COMMIT} for ${_first}"
      BUILD_COMMIT STREQUAL _first AND BUILD_DIRTY STREQUAL "false")

age_output()
read_build_info("${WORK_DIR}/repository" "${GIT_EXECUTABLE}")
output_year(_year)
check("a second run with nothing changed leaves the source untouched: ${_year}"
      _year STREQUAL "2000" AND BUILD_COMMIT STREQUAL _first)

# A file git does not track changes nothing that was compiled from the
# commit.
file(WRITE "${WORK_DIR}/repository/scratch.txt" "notes\n")
read_build_info("${WORK_DIR}/repository" "${GIT_EXECUTABLE}")
output_year(_year)
check("an untracked file does not make the build dirty: ${BUILD_DIRTY}"
      BUILD_DIRTY STREQUAL "false" AND _year STREQUAL "2000")

file(WRITE "${WORK_DIR}/repository/source.cc" "int edited;\n")
read_build_info("${WORK_DIR}/repository" "${GIT_EXECUTABLE}")
output_year(_year)
check("an edited tracked file makes the build dirty, at the same commit"
      BUILD_COMMIT STREQUAL _first AND BUILD_DIRTY STREQUAL "true")
check("and the source is rewritten: ${_year}" NOT _year STREQUAL "2000")

git("${WORK_DIR}/repository" commit --quiet -a -m second)
git("${WORK_DIR}/repository" rev-parse --short HEAD)
set(_second "${GIT_OUTPUT}")
age_output()
read_build_info("${WORK_DIR}/repository" "${GIT_EXECUTABLE}")
output_year(_year)
check("a new commit gives the new hash, clean: ${BUILD_COMMIT} for ${_second}"
      BUILD_COMMIT STREQUAL _second AND NOT _second STREQUAL _first AND
      BUILD_DIRTY STREQUAL "false" AND NOT _year STREQUAL "2000")

# Sources below the checkout's root are not the checkout: the commit would
# be the enclosing repository's, not theirs.
file(MAKE_DIRECTORY "${WORK_DIR}/repository/vendored")
read_build_info("${WORK_DIR}/repository/vendored" "${GIT_EXECUTABLE}")
check("a directory inside a repository is not given that repository's commit"
      BUILD_COMMIT STREQUAL "unknown" AND BUILD_DIRTY STREQUAL "false")

read_build_info("${WORK_DIR}/repository" "")
check("without git the commit is unknown and the build still passes"
      BUILD_COMMIT STREQUAL "unknown" AND BUILD_DIRTY STREQUAL "false")

file(REMOVE_RECURSE "${WORK_DIR}")
if(NOT _failures EQUAL 0)
    message(FATAL_ERROR "${_failures} assertion(s) failed")
endif()
message(STATUS "All build info script tests passed")
