# PRD19 -- ROADMAP.md. Writes the build's identity (src/engine/BuildInfo.h) into a
# source file of the build tree. Run by the `cutmachine_build_info` target at
# every build, not at configuration: a build tree is configured once and then
# built across many commits, and a hash read at configuration would name the
# commit the tree was configured at.
#
#   cmake -DSOURCE_DIR=<checkout> -DTEMPLATE=<BuildInfo.cc.in>
#         -DOUTPUT=<BuildInfo.cc> -DVERSION=<x.y.z> [-DGIT_EXECUTABLE=<git>]
#         -P BuildInfo.cmake
#
# configure_file leaves OUTPUT alone when its content would not change, so a
# build at the same commit recompiles nothing.

cmake_minimum_required(VERSION 3.24)

foreach(_required IN ITEMS SOURCE_DIR TEMPLATE OUTPUT VERSION)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "BuildInfo.cmake needs -D${_required}=...")
    endif()
endforeach()

set(CUTMACHINE_BUILD_VERSION "${VERSION}")
# An archive of the sources has no commit: the build still passes.
set(CUTMACHINE_BUILD_COMMIT "unknown")
set(CUTMACHINE_BUILD_DIRTY "false")

if(GIT_EXECUTABLE)
    # The checkout must be SOURCE_DIR itself. Sources unpacked inside some
    # other repository would otherwise be given that repository's commit.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --show-toplevel
        OUTPUT_VARIABLE _toplevel
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _status)
    get_filename_component(_source "${SOURCE_DIR}" REALPATH)
    if(_status EQUAL 0 AND NOT _toplevel STREQUAL "")
        get_filename_component(_toplevel "${_toplevel}" REALPATH)
    endif()
    if(_status EQUAL 0 AND _toplevel STREQUAL _source)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" rev-parse --short HEAD
            OUTPUT_VARIABLE _commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _status)
        if(_status EQUAL 0 AND _commit MATCHES "^[0-9a-f]+$")
            set(CUTMACHINE_BUILD_COMMIT "${_commit}")
            # Tracked files only, as `git describe --dirty` means it: a
            # scratch file beside the sources does not change what was
            # compiled. --no-optional-locks: a build must not take the index
            # lock from under a git command running beside it.
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" --no-optional-locks
                        -C "${SOURCE_DIR}" status --porcelain
                        --untracked-files=no
                OUTPUT_VARIABLE _changes
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _status)
            # A status git could not give is not a clean one.
            if(NOT _status EQUAL 0 OR NOT _changes STREQUAL "")
                set(CUTMACHINE_BUILD_DIRTY "true")
            endif()
        endif()
    endif()
endif()

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)
