# HEADLESS-2026-10 -- docs/roadmap/HEADLESS-2026-10.md. The engine, its
# command line and its MCP server, built without AppKit, Metal or any Apple
# framework, for a host that has no window (Linux container, CI runner, an
# agent's sandbox).
#
# Not a second build to maintain by hand: the model's sources, the pinned
# dependencies (H3) and the test suites are read from the main
# CMakeLists.txt, so what is added there is built here too. The
# substitutions:
#   - src/app/main.mm (AppKit) -> HeadlessMain.cc, over the same command line
#     (CliMain.h, H1)
# The renderers that need Core Text, Core Graphics or Vision
# (cutmachine_motion_render) are not built: the tools that need one find
# none installed and say so. SQLite is the system's.
#
#   cmake -S headless -B build-headless -G Ninja
#   cmake --build build-headless
cmake_minimum_required(VERSION 3.24)
project(CUTMACHINE_HEADLESS VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ language standard" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CUTMACHINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
cmake_path(NORMAL_PATH CUTMACHINE_ROOT)
file(READ "${CUTMACHINE_ROOT}/CMakeLists.txt" _main_cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample libswscale)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# H3 -- each dependency at the commit the main build pins, checked against
# the same SHA-256 (PRD23), read from its FetchContent_Declare: a pin moved
# there moves here, and nothing is downloaded that the main build would not
# take. FETCHCONTENT_SOURCE_DIR_<NAME> replaces a download with a local
# checkout, as it does there.
include(FetchContent)
function(cutmachine_pinned_dependency name)
    string(REGEX MATCH "FetchContent_Declare\\([ \t\r\n]+${name}[ \t\r\n][^)]*\\)"
           _block "${_main_cmake}")
    set(_url)
    set(_hash)
    if(_block MATCHES "[ \t]URL[ \t]+([^ \t\r\n]+)")
        set(_url "${CMAKE_MATCH_1}")
    endif()
    if(_block MATCHES "URL_HASH[ \t]+SHA256=([0-9a-f]+)")
        set(_hash "${CMAKE_MATCH_1}")
    endif()
    if(NOT _url OR NOT _hash)
        message(FATAL_ERROR "CMakeLists.txt pins no URL and SHA256 for ${name}")
    endif()
    set(_arguments URL "${_url}" URL_HASH "SHA256=${_hash}")
    if(_block MATCHES "SOURCE_SUBDIR[ \t]+([^ \t\r\n]+)")
        list(APPEND _arguments SOURCE_SUBDIR "${CMAKE_MATCH_1}")
    endif()
    message(STATUS "${name}: ${_url}")
    FetchContent_Declare(${name} ${_arguments})
endfunction()

# whisper.cpp -- same options as the main build.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
cutmachine_pinned_dependency(whispercpp)
FetchContent_MakeAvailable(whispercpp)

# OpenColorIO -- the pinned version; what the system already has (yaml-cpp,
# expat, pystring, Imath, zlib) is reused, the rest is built. The main build
# builds all of them (ALL): here they are the distribution's.
set(OCIO_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_GPU_TESTS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_NUKE OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_OPENFX OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OCIO_INSTALL_EXT_PACKAGES MISSING CACHE STRING "" FORCE)
cutmachine_pinned_dependency(opencolorio)
set_property(DIRECTORY PROPERTY EP_UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(opencolorio)
set_property(DIRECTORY PROPERTY EP_UPDATE_DISCONNECTED)

# Yoga -- as in the main build, without -Werror and -fno-rtti.
cutmachine_pinned_dependency(yoga)
FetchContent_MakeAvailable(yoga)
get_target_property(YOGA_COMPILE_OPTIONS yogacore COMPILE_OPTIONS)
if(YOGA_COMPILE_OPTIONS)
    list(REMOVE_ITEM YOGA_COMPILE_OPTIONS -Werror
         "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
    set_target_properties(yogacore PROPERTIES
        COMPILE_OPTIONS "${YOGA_COMPILE_OPTIONS}")
endif()

# zstd -- DEC15 (GR15): the decoder only, from the same recipe as the main
# build (cmake/CutmachineZstd.cmake). Resolve's grades are zstd frames.
cutmachine_pinned_dependency(zstd)
FetchContent_MakeAvailable(zstd)
include("${CUTMACHINE_ROOT}/cmake/CutmachineZstd.cmake")

# Strudel -- SN9: Node reads its pattern engine to write a pattern as a
# partition (the sound suite runs it); nothing of it is built.
cutmachine_pinned_dependency(strudel)
cutmachine_pinned_dependency(fractionjs)
FetchContent_MakeAvailable(strudel fractionjs)
include("${CUTMACHINE_ROOT}/cmake/StrudelModules.cmake")

# Generated sources -- the same rules as the main build.
find_package(Git QUIET)
set(BUILD_INFO_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo.cc")
add_custom_target(cutmachine_build_info
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CUTMACHINE_ROOT}"
            "-DTEMPLATE=${CUTMACHINE_ROOT}/src/engine/BuildInfo.cc.in"
            "-DOUTPUT=${BUILD_INFO_SOURCE}"
            "-DVERSION=${PROJECT_VERSION}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${CUTMACHINE_ROOT}/cmake/BuildInfo.cmake"
    BYPRODUCTS "${BUILD_INFO_SOURCE}"
    VERBATIM)

file(GLOB MOTION_TEMPLATE_FILES "${CUTMACHINE_ROOT}/assets/motion-templates/*.json")
list(SORT MOTION_TEMPLATE_FILES)
set(CUTMACHINE_MOTION_TEMPLATE_ENTRIES "")
foreach(_path IN LISTS MOTION_TEMPLATE_FILES)
    get_filename_component(_template "${_path}" NAME_WE)
    file(READ "${_path}" _json)
    string(APPEND CUTMACHINE_MOTION_TEMPLATE_ENTRIES
           "        {\"${_template}\", R\"CUTMACHINEJSON(${_json})CUTMACHINEJSON\"},\n")
endforeach()
configure_file("${CUTMACHINE_ROOT}/src/motion/MotionTemplatesData.cc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/MotionTemplatesData.cc" @ONLY)

set(SKILLS_DIR "${CUTMACHINE_ROOT}/.claude/skills")
file(GLOB_RECURSE SKILL_FILES "${SKILLS_DIR}/*.md" "${SKILLS_DIR}/*.json"
     "${SKILLS_DIR}/*.py")
set(PROCEDURES_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/ProceduresData.cc")
add_custom_command(
    OUTPUT "${PROCEDURES_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DSKILLS_DIR=${SKILLS_DIR}"
            "-DOUTPUT=${PROCEDURES_SOURCE}"
            -P "${CUTMACHINE_ROOT}/cmake/EmbedProcedures.cmake"
    DEPENDS ${SKILL_FILES} "${CUTMACHINE_ROOT}/cmake/EmbedProcedures.cmake"
    VERBATIM)

set(AFTER_BRIDGE_SCRIPT_DIR "${CMAKE_CURRENT_BINARY_DIR}/after-bridge-scripts")
file(COPY "${CUTMACHINE_ROOT}/tools/after-bridge/scripts/"
     DESTINATION "${AFTER_BRIDGE_SCRIPT_DIR}")

# The model's sources, read from the main build.
string(FIND "${_main_cmake}" "add_library(cutmachine_model" _start)
if(_start LESS 0)
    message(FATAL_ERROR "add_library(cutmachine_model ...) not found")
endif()
string(SUBSTRING "${_main_cmake}" ${_start} -1 _model_block)
string(FIND "${_model_block}" "\n)" _end)
string(SUBSTRING "${_model_block}" 0 ${_end} _model_block)
string(REGEX MATCHALL "src/[a-z]+/[A-Za-z0-9_]+\\.(cc|mm)" _model_sources
       "${_model_block}")
list(FILTER _model_sources EXCLUDE REGEX "\\.mm$")
list(TRANSFORM _model_sources PREPEND "${CUTMACHINE_ROOT}/")
list(LENGTH _model_sources _model_count)
message(STATUS "cutmachine_model (headless): ${_model_count} sources from CMakeLists.txt")

add_library(cutmachine_model STATIC
    ${_model_sources}
    "${BUILD_INFO_SOURCE}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/MotionTemplatesData.cc"
    "${PROCEDURES_SOURCE}")
add_dependencies(cutmachine_model cutmachine_build_info)
# STRUCTURE-2026-11 ST3 -- the seven module directories, as in the main
# build: a header is included by its bare name.
set(CUTMACHINE_SOURCE_DIRS)
foreach(_cutmachine_module engine media motion ui agent platform app)
    list(APPEND CUTMACHINE_SOURCE_DIRS
         "${CUTMACHINE_ROOT}/src/${_cutmachine_module}")
endforeach()
target_include_directories(cutmachine_model PUBLIC ${CUTMACHINE_SOURCE_DIRS})
# CommonCrypto's SHA-256, answered by OpenSSL (headless/compat).
find_package(OpenSSL REQUIRED)
target_include_directories(cutmachine_model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_compile_definitions(cutmachine_model PRIVATE
    CUTMACHINE_AFTER_BRIDGE_SCRIPTS="${AFTER_BRIDGE_SCRIPT_DIR}"
    CUTMACHINE_MFA_PYTHON_DEFAULT="${CMAKE_BINARY_DIR}/mfa/env/bin/python"
    CUTMACHINE_MFA_ALIGNER_DEFAULT="${CMAKE_BINARY_DIR}/mfa/env/bin/mfa"
    CUTMACHINE_MFA_SCRIPT_DEFAULT="${CUTMACHINE_ROOT}/scripts/mfa_align.py"
    CUTMACHINE_VOICE_PYTHON_DEFAULT="${CMAKE_BINARY_DIR}/voice/env/bin/python"
    CUTMACHINE_VOICE_SCRIPT_DEFAULT="${CUTMACHINE_ROOT}/scripts/voice_over.py"
    CUTMACHINE_VOICE_MODELS_DEFAULT="${CMAKE_BINARY_DIR}/voice/models")
target_link_libraries(cutmachine_model PUBLIC
    PkgConfig::FFMPEG OpenColorIO whisper Threads::Threads SQLite::SQLite3
    OpenSSL::Crypto)
target_link_libraries(cutmachine_model PRIVATE yogacore cutmachine_zstd)

add_executable(cutmachine "${CUTMACHINE_ROOT}/src/agent/HeadlessMain.cc")
target_link_libraries(cutmachine PRIVATE cutmachine_model)

# The suites the main build labels "portable", declared again from its
# CMakeLists.txt (portable_tests.py lists what it cannot carry over).
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Variables the main build's test declarations name.
    find_program(FFMPEG_EXECUTABLE NAMES ffmpeg REQUIRED)
    find_program(CUTMACHINE_NODE_EXECUTABLE NAMES node)
    set(CUTMACHINE_SKILLS_DIR "${SKILLS_DIR}")
    set(_portable "${CMAKE_CURRENT_BINARY_DIR}/portable-tests.cmake")
    # HEADLESS_ALL_SUITES=ON: every suite that builds without Apple code,
    # labelled "engine" when the main build does not call it portable.
    option(HEADLESS_ALL_SUITES "Declare every suite free of Apple code" OFF)
    set(_portable_scope)
    if(HEADLESS_ALL_SUITES)
        set(_portable_scope --all)
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I
                "${CMAKE_CURRENT_SOURCE_DIR}/portable_tests.py"
                "${CUTMACHINE_ROOT}/CMakeLists.txt" "${_portable}"
                ${_portable_scope}
        RESULT_VARIABLE _portable_result)
    if(NOT _portable_result EQUAL 0)
        message(FATAL_ERROR "portable_tests.py failed")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${CUTMACHINE_ROOT}/CMakeLists.txt")
    set_source_files_properties("${CUTMACHINE_ROOT}/tests/test_environment.cc"
        PROPERTIES COMPILE_DEFINITIONS
        "CUTMACHINE_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
    include("${_portable}")
endif()
