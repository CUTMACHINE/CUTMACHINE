cmake_minimum_required(VERSION 3.24)
# PRD19 -- ROADMAP.md. The one version number of the project: the bundle's
# Info.plist and the binary (src/BuildInfo.h, `cutmachine --version`) both
# read it from here. Changing it is a release decision, not a build detail.
project(CUTMACHINE VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ language standard" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

# BUILD-2026-09 -- checking out another branch rewrites timestamps, and make
# then recompiles every file they touch -- whisper.cpp and OpenColorIO
# included. With ccache installed (`brew install ccache`) an unchanged
# translation unit comes back from its cache instead. Optional: without it,
# or with a launcher already chosen by the caller, nothing changes.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    foreach(_language C CXX OBJC OBJCXX)
        set(CMAKE_${_language}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    endforeach()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswresample
    libswscale
)
# The MCP server (HttpServer.cc/McpServer.cc) uses std::thread for its
# accept loop.
find_package(Threads REQUIRED)

# F1.4 -- local transcription. Fetched, not vendored, following this
# project's general preference for build-time dependencies over committed
# third-party source. whisper.cpp is MIT licensed (compatible with this
# project's AGPL-3.0-or-later) and runs entirely on-device: pulling it in does
# not touch PHILOSOPHY.md's "pas de service central" guard-rail, since inference
# still happens against a local model file the caller supplies, never a
# network call. Examples/tests/server and the GPU backends this project's
# target platform (macOS/Apple Silicon) does not use are disabled; GGML_METAL
# is left at its own default, which auto-enables on Apple platforms and
# stays off elsewhere.
include(FetchContent)

# PRD23 -- ROADMAP.md. Every licence the application bundle carries is copied
# into one directory, which cutmachine_app ships as Contents/Resources/licenses/:
# the files of this repository here, each downloaded dependency's right after
# it is made available. THIRD_PARTY_NOTICES.md lists the same files, and
# tests/architecture_tests.py fails when a FetchContent dependency has no row
# there or no licence copied here. NO_SOURCE_PERMISSIONS: a source tree checked
# out with mode 0700 made the bundled notices unreadable to other accounts.
set(CUTMACHINE_LICENSE_DIR "${CMAKE_CURRENT_BINARY_DIR}/licenses")
set(CUTMACHINE_LICENSE_RESOURCES)
function(cutmachine_bundle_license bundled_name source_file)
    set(_output "${CUTMACHINE_LICENSE_DIR}/${bundled_name}")
    configure_file("${source_file}" "${_output}" COPYONLY NO_SOURCE_PERMISSIONS)
    set(CUTMACHINE_LICENSE_RESOURCES ${CUTMACHINE_LICENSE_RESOURCES}
        "${_output}" PARENT_SCOPE)
endfunction()
cutmachine_bundle_license(THIRD_PARTY_NOTICES.md
    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
cutmachine_bundle_license(Lucide-LICENSE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/LICENSE")
# FILMCRAFT-2026-10 -- code ported from FilmCraft keeps its MIT notice.
cutmachine_bundle_license(FilmCraft-LICENSE-MIT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/filmcraft/LICENSE-MIT")
# The motion-design procedure, which the binary embeds, adapts the rules of
# an MIT-licensed skill and keeps its notice.
cutmachine_bundle_license(claude-remotion-skill-LICENSE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/claude-remotion-skill/LICENSE")

# PRD23 -- each dependency below is the archive of one exact commit, checked
# against its SHA-256 before CMake reads a file of it. A tag can be moved
# upstream; this archive cannot change without the download failing, and
# should GitHub ever serve other bytes for the same commit, the build stops
# rather than compiling them. The archives build without git or submodules.
# Moving to another version is a decision, never a routine bump: put the new
# full commit in URL, hash that archive once (`curl -sL <URL> | shasum -a 256`)
# and paste the digest in URL_HASH, then change the tag in the comment and
# the commit in THIRD_PARTY_NOTICES.md (tests/architecture_tests.py checks
# that the notices carry it). FETCHCONTENT_SOURCE_DIR_<NAME> still replaces
# a download with a local checkout; nothing is downloaded or hashed then.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    whispercpp
    # Tag v1.7.4.
    URL      https://github.com/ggml-org/whisper.cpp/archive/8a9ad7844d6e2a10cddf4b92de4089d7ac2b14a9.tar.gz
    URL_HASH SHA256=d81e5d9f6207d78db35f2eea97a60449e68a7ac88956eafa08e433d3a43cdfcb
)
FetchContent_MakeAvailable(whispercpp)
cutmachine_bundle_license(whisper.cpp-LICENSE "${whispercpp_SOURCE_DIR}/LICENSE")

# COLOR-2026-08 -- OpenColorIO owns the project color transforms rather than
# keeping a second, hand-written approximation in the player and exporter.
# Pin both the library and the built-in config selected in ColorManagement.cc:
# upgrading either changes rendered pixels and therefore needs an explicit
# project decision. Only the C++ core is built; CUTMACHINE neither ships nor
# needs OCIO's applications, language bindings, plug-ins, or own test suite.
set(OCIO_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_GPU_TESTS OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_JAVA OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_NUKE OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_OPENFX OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OCIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OCIO_INSTALL_EXT_PACKAGES ALL CACHE STRING "" FORCE)
FetchContent_Declare(
    opencolorio
    # Tag v2.5.2.
    URL      https://github.com/AcademySoftwareFoundation/OpenColorIO/archive/c52966a6677723d5bd2dbef0ccec3fed9cbc3790.tar.gz
    URL_HASH SHA256=67d9eebea32345cbd0f6ae1e45118ecf346d6b817de9f39c8a75c1ff770374c7
)
# PRD23 -- OpenColorIO builds yaml-cpp, expat, pystring, Imath, zlib and
# minizip-ng through ExternalProject, from git tags. A git update step is
# always out of date, so every build, even with nothing changed, re-ran their
# update, configure and install steps; the install script of a static
# library on macOS runs ranlib on it, the six archives got a new date, and
# every executable linking them was relinked (12 to 14 s at -j3). The pinned
# OpenColorIO fixes those tags, so there is nothing to ask the remotes for:
# their update step is made disconnected, for OpenColorIO's directory only.
# Another tag still changes the download step, which clones again.
set_property(DIRECTORY PROPERTY EP_UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(opencolorio)
set_property(DIRECTORY PROPERTY EP_UPDATE_DISCONNECTED)
# PRD23 -- what OpenColorIO compiles into the binary besides its own code:
# THIRD-PARTY.md covers what it vendors (SampleICC, xxHash), and its CDL
# reader carries a BSD notice of its own (Cinesite VFX) that neither file
# reproduces, so the opening comment of that source is copied as it is.
cutmachine_bundle_license(OpenColorIO-LICENSE "${opencolorio_SOURCE_DIR}/LICENSE")
cutmachine_bundle_license(OpenColorIO-THIRD-PARTY.md
    "${opencolorio_SOURCE_DIR}/THIRD-PARTY.md")
file(READ
    "${opencolorio_SOURCE_DIR}/src/OpenColorIO/fileformats/FileFormatCDL.cpp"
    _ocio_cdl_source LIMIT 4096)
string(FIND "${_ocio_cdl_source}" "*/" _ocio_cdl_notice_end)
if(_ocio_cdl_notice_end LESS 0
   OR NOT _ocio_cdl_source MATCHES "^/\\*[^*]*Cinesite VFX")
    message(FATAL_ERROR "OpenColorIO's FileFormatCDL.cpp no longer opens "
        "with the Cinesite VFX notice: read its licence again and update "
        "CMakeLists.txt and THIRD_PARTY_NOTICES.md (PRD23).")
endif()
math(EXPR _ocio_cdl_notice_end "${_ocio_cdl_notice_end} + 2")
string(SUBSTRING "${_ocio_cdl_source}" 0 ${_ocio_cdl_notice_end}
       _ocio_cdl_notice)
set(_ocio_cdl_output
    "${CUTMACHINE_LICENSE_DIR}/OpenColorIO-FileFormatCDL-NOTICE")
file(CONFIGURE OUTPUT "${_ocio_cdl_output}"
     CONTENT "${_ocio_cdl_notice}\n" @ONLY)
list(APPEND CUTMACHINE_LICENSE_RESOURCES "${_ocio_cdl_output}")

# The libraries OpenColorIO builds for itself (OCIO_INSTALL_EXT_PACKAGES ALL)
# are linked into the binary too. Its ExternalProject steps clone them when
# building, not when configuring, so their licences are copied by a target
# that runs after those steps; copy_if_different leaves an unchanged file
# alone, and nothing downstream is redone. sse2neon, which OpenColorIO
# fetches while configuring and compiles in on arm64 only, is there already.
include(ExternalProject)
set(_ocio_ext_targets)
set(_ocio_ext_licenses)
set(_ocio_ext_copies)
foreach(_entry IN ITEMS
        "expat_install:COPYING:expat-LICENSE"
        "yaml-cpp_install:LICENSE:yaml-cpp-LICENSE"
        "pystring_install:LICENSE:pystring-LICENSE"
        "imath_install:LICENSE.md:Imath-LICENSE"
        "ZLIB_install:LICENSE:zlib-LICENSE"
        "minizip-ng_install:LICENSE:minizip-ng-LICENSE")
    string(REPLACE ":" ";" _fields "${_entry}")
    list(GET _fields 0 _target)
    list(GET _fields 1 _file)
    list(GET _fields 2 _bundled)
    ExternalProject_Get_Property(${_target} SOURCE_DIR)
    list(APPEND _ocio_ext_targets ${_target})
    list(APPEND _ocio_ext_licenses "${CUTMACHINE_LICENSE_DIR}/${_bundled}")
    list(APPEND _ocio_ext_copies
         COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                 "${SOURCE_DIR}/${_file}" "${CUTMACHINE_LICENSE_DIR}/${_bundled}")
endforeach()
unset(SOURCE_DIR)
add_custom_target(cutmachine_ocio_ext_licenses
    ${_ocio_ext_copies}
    BYPRODUCTS ${_ocio_ext_licenses}
    COMMENT "Copying the licences of the libraries OpenColorIO builds"
    VERBATIM
)
add_dependencies(cutmachine_ocio_ext_licenses ${_ocio_ext_targets})
list(APPEND CUTMACHINE_LICENSE_RESOURCES ${_ocio_ext_licenses})
FetchContent_GetProperties(sse2neon)
if(sse2neon_POPULATED)
    cutmachine_bundle_license(sse2neon-LICENSE "${sse2neon_SOURCE_DIR}/LICENSE")
endif()

# Treat whisper.cpp's headers as system headers. Without this they are
# ordinary includes, so every translation unit that pulls in whisper.h
# inherits ggml's warnings -- and .clang-tidy's HeaderFilterRegex
# ('.*/src/.*') matches build/_deps/whispercpp-src/ggml/src/... just as
# happily as this project's own src/, burying real findings under hundreds
# of lines about a dependency we do not maintain.
# MOTION-2026-09 M1 -- ROADMAP.md. Yoga is the flexbox engine a motion
# composition is laid out with, so the author declares a layout and the engine
# measures it. Pinned: a new release can move boxes by a pixel, which is a
# rendering change and needs a decision. Only yoga/ is built (SOURCE_SUBDIR),
# not its gtest suite. Yoga compiles itself as C++20 inside its own directory
# scope and exposes a C API, so this project stays C++17. Two of its own flags
# are removed. -Werror: a warning from a newer compiler in code we do not
# maintain must not break our build. -fno-rtti: compiled that way, Yoga's
# objects carry their own reduced type_info for the standard exceptions they
# throw (std::length_error, std::bad_array_new_length). The linker then prefers
# those definitions to libc++abi's for every binary linking the model, and
# `catch (const std::exception&)` stops matching: document, project, CLI and
# export tests aborted on exceptions they normally catch.
FetchContent_Declare(
    yoga
    # Tag v3.2.1.
    URL           https://github.com/facebook/yoga/archive/042f5013152eb81c1552dec945b88f7b95ca350f.tar.gz
    URL_HASH      SHA256=4742f41722a16f181e3da37abf943390db1e928f00f26402cb154662ae7f110f
    SOURCE_SUBDIR yoga
)
FetchContent_MakeAvailable(yoga)
get_target_property(YOGA_COMPILE_OPTIONS yogacore COMPILE_OPTIONS)
if(YOGA_COMPILE_OPTIONS)
    list(REMOVE_ITEM YOGA_COMPILE_OPTIONS -Werror
         "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
    set_target_properties(yogacore PROPERTIES
        COMPILE_OPTIONS "${YOGA_COMPILE_OPTIONS}")
endif()
get_target_property(YOGA_INTERFACE_INCLUDES yogacore
                    INTERFACE_INCLUDE_DIRECTORIES)
if(YOGA_INTERFACE_INCLUDES)
    set_target_properties(yogacore PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${YOGA_INTERFACE_INCLUDES}")
endif()
cutmachine_bundle_license(Yoga-LICENSE "${yoga_SOURCE_DIR}/LICENSE")

# DB1 -- docs/roadmap/DONNEES-2026-10.md. Build SQLite from one upstream
# commit, including its default JSON functions. Generate the amalgamation
# out of tree: the source archive includes its own small Tcl interpreter.
# Only the core is linked, without extension loading or shell file functions.
FetchContent_Declare(
    sqlite
    # Tag version-3.53.4.
    URL https://github.com/sqlite/sqlite/archive/b09c88c14082339b66c7b7158d609a771e64ca69.tar.gz
    URL_HASH SHA256=e4073c78a6dc50a9abdf6d929c69c6adacf69e6b07f0f0c43b26aa1524d2076a
    SOURCE_SUBDIR cutmachine-no-subdirectory
)
FetchContent_MakeAvailable(sqlite)
cutmachine_bundle_license(SQLite-LICENSE.md "${sqlite_SOURCE_DIR}/LICENSE.md")
find_program(CUTMACHINE_SQLITE_MAKE make REQUIRED)
file(MAKE_DIRECTORY "${sqlite_BINARY_DIR}")
add_custom_command(
    OUTPUT "${sqlite_BINARY_DIR}/sqlite3.c" "${sqlite_BINARY_DIR}/sqlite3.h"
    COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
            "${sqlite_SOURCE_DIR}/configure" --disable-tcl
    COMMAND "${CUTMACHINE_SQLITE_MAKE}" sqlite3.c sqlite3.h
    WORKING_DIRECTORY "${sqlite_BINARY_DIR}"
    DEPENDS "${sqlite_SOURCE_DIR}/manifest.uuid"
    COMMENT "Generating the pinned SQLite amalgamation"
    VERBATIM
)
add_library(cutmachine_sqlite STATIC "${sqlite_BINARY_DIR}/sqlite3.c")
target_include_directories(cutmachine_sqlite SYSTEM PUBLIC "${sqlite_BINARY_DIR}")
target_compile_definitions(cutmachine_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
target_link_libraries(cutmachine_sqlite PRIVATE Threads::Threads)

# DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Resolve keeps each
# grade as a zstd frame in its project database; the engine reads them with
# the decoder of this pinned archive, nothing else of it (cmake/CutmachineZstd.cmake,
# shared with the headless build). BSD-3-Clause, the licence the notices cite.
FetchContent_Declare(
    zstd
    # Tag v1.5.7.
    URL      https://github.com/facebook/zstd/archive/f8745da6ff1ad1e7bab384bd1f9d742439278e99.tar.gz
    URL_HASH SHA256=4b0bd1f0cfb25e61b9103c35f27395530ff5b4c0d2513a00fd745849e85ea52c
    SOURCE_SUBDIR cutmachine-no-subdirectory
)
FetchContent_MakeAvailable(zstd)
cutmachine_bundle_license(zstd-LICENSE "${zstd_SOURCE_DIR}/LICENSE")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CutmachineZstd.cmake")

# SN9 -- docs/roadmap/SON-2026-10.md. A Strudel pattern written as a partition:
# tools/cutmachine-sound/strudel.mjs asks Strudel's pattern engine (core and
# mini) for a pattern's events, under Node. Nothing of it is compiled nor
# shipped in the app; the build only fetches the sources and lets Node find
# their imports (cmake/StrudelModules.cmake, shared with the headless build).
# AGPL-3.0-or-later like CUTMACHINE; fraction.js, MIT, is its exact time.
# DECOUPAGE-2026-10 -- a download that stops sending fails after a minute
# instead of holding the configure open: on 27 October 2026 codeberg's
# archive stalled at 0 bytes and a full build waited 7 h 47.
FetchContent_Declare(
    strudel
    # Tags @strudel/core@1.2.6 and @strudel/mini@1.2.6.
    URL      https://codeberg.org/uzu/strudel/archive/d198910a86fcaecd658ef528bfa6d7884e93490b.tar.gz
    URL_HASH SHA256=f1f96291863c0104906820beb80be810ddd49baf4eda892ebf4e0015dda4fe97
    INACTIVITY_TIMEOUT 60
    SOURCE_SUBDIR cutmachine-no-subdirectory
)
FetchContent_Declare(
    fractionjs
    # Tag v5.2.2.
    URL      https://github.com/rawify/Fraction.js/archive/859b36ee3de85172343b1cee23f3fb5d018ed091.tar.gz
    URL_HASH SHA256=83faa947657c23a79c229a2369f4cdffab66151fe4b5fe8e7db6919170be812e
    INACTIVITY_TIMEOUT 60
    SOURCE_SUBDIR cutmachine-no-subdirectory
)
FetchContent_MakeAvailable(strudel fractionjs)
cutmachine_bundle_license(Strudel-LICENSE "${strudel_SOURCE_DIR}/LICENSE")
cutmachine_bundle_license(Fraction.js-LICENSE "${fractionjs_SOURCE_DIR}/LICENSE")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/StrudelModules.cmake")

get_target_property(WHISPER_INTERFACE_INCLUDES whisper
                    INTERFACE_INCLUDE_DIRECTORIES)
if(WHISPER_INTERFACE_INCLUDES)
    set_target_properties(whisper PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${WHISPER_INTERFACE_INCLUDES}")
endif()

set(SHADER_RUNTIME_PATH "${CMAKE_CURRENT_BINARY_DIR}/shader.metal")
configure_file(src/shader.metal "${SHADER_RUNTIME_PATH}" COPYONLY)

# AFTER-COMP -- ExtendScript stays inspectable under tools/, while the binary
# reads a stable build-local copy at runtime. An environment override supports
# packaged/manual layouts without putting a machine path in project state.
set(AFTER_BRIDGE_SCRIPT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/after-bridge-scripts")
file(MAKE_DIRECTORY "${AFTER_BRIDGE_SCRIPT_DIR}")
foreach(_script IN ITEMS
        add_layer.jsx
        common.jsx
        create_project.jsx
        describe_comp.jsx
        describe_project.jsx
        duplicate_layer.jsx
        import_footage.jsx
        list_comps.jsx
        list_footage.jsx
        # MA1 -- build_motion_project, also run by hand from After.
        motion_import.jsx
        open_project_copy.jsx
        # MB7 -- read_comp, what import_after_comp translates.
        read_comp.jsx
        render_frame.jsx
        replace_footage.jsx
        set_layer_state.jsx
        set_property.jsx
        set_text.jsx
        save_project.jsx)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/after-bridge/scripts/${_script}"
        "${AFTER_BRIDGE_SCRIPT_DIR}/${_script}"
        COPYONLY)
endforeach()

# UI-2026-08 -- Lucide stays a build-time input. `sips` is part of macOS and
# rasterizes each SVG to a high-resolution RGBA mask; Renderer.mm packs these
# PNG alphas beside the CoreText glyphs in one R8 atlas. No SVG parser ships in
# the application and the generated files remain build artifacts.
find_program(SIPS_EXECUTABLE NAMES sips REQUIRED)
set(CUTMACHINE_ICON_NAMES
    chevrons-right-left
    eye
    hand
    headphones
    link
    lock
    magnet
    mouse-pointer-2
    scissors
    search
    unfold-horizontal
    volume-x
)
set(CUTMACHINE_ICON_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/ui-icons")
set(CUTMACHINE_ICON_PNGS)
foreach(_icon IN LISTS CUTMACHINE_ICON_NAMES)
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/${_icon}.svg")
    set(_output "${CUTMACHINE_ICON_OUTPUT_DIR}/${_icon}.png")
    add_custom_command(
        OUTPUT "${_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CUTMACHINE_ICON_OUTPUT_DIR}"
        COMMAND "${SIPS_EXECUTABLE}" -s format png -z 48 48 "${_source}"
                --out "${_output}"
        DEPENDS "${_source}"
        COMMENT "Rasterizing Lucide icon ${_icon} @2x"
        VERBATIM
    )
    list(APPEND CUTMACHINE_ICON_PNGS "${_output}")
endforeach()
add_custom_target(cutmachine_ui_icons DEPENDS ${CUTMACHINE_ICON_PNGS})

# M5 -- docs/roadmap/MOTION-2026-09.md. The agent's motion templates are data
# in the common composition format, versioned under assets/motion-templates
# and embedded in the binary, so every surface lists the same ones without a
# path to look them up. Editing one re-runs this configuration.
#
# MB1 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. Every piece file of the
# directory ships, by name: a shipped piece is added by dropping its file
# there, not by naming it here. cutmachine_motion_templates_tests evaluates
# each one in every format it declares.
file(GLOB CUTMACHINE_MOTION_TEMPLATE_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/assets/motion-templates/*.json")
list(SORT CUTMACHINE_MOTION_TEMPLATE_FILES)
set(CUTMACHINE_MOTION_TEMPLATE_ENTRIES "")
foreach(_path IN LISTS CUTMACHINE_MOTION_TEMPLATE_FILES)
    get_filename_component(_template "${_path}" NAME_WE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
    file(READ "${_path}" _json)
    string(APPEND CUTMACHINE_MOTION_TEMPLATE_ENTRIES
           "        {\"${_template}\", R\"CUTMACHINEJSON(${_json})CUTMACHINEJSON\"},\n")
endforeach()
configure_file(src/MotionTemplatesData.cc.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/MotionTemplatesData.cc"
               @ONLY)

# PRD18 -- ROADMAP.md (ACP4, docs/roadmap/ACP-2026-09.md). The repository's
# skills are the engine's procedures, served to every agent by the binary
# itself (src/Procedures.h): cmake/EmbedProcedures.cmake writes their text
# files into a generated source. It runs at build time when a skill file or
# the script changes, and only then. The list is read here: CONFIGURE_DEPENDS
# reconfigures when a file appears or goes, and the list file, rewritten only
# when it changes, then regenerates the source for a file that went.
set(CUTMACHINE_SKILLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.claude/skills")
file(GLOB_RECURSE CUTMACHINE_SKILL_FILES CONFIGURE_DEPENDS
     "${CUTMACHINE_SKILLS_DIR}/*.md" "${CUTMACHINE_SKILLS_DIR}/*.json"
     "${CUTMACHINE_SKILLS_DIR}/*.py")
list(SORT CUTMACHINE_SKILL_FILES)
string(REPLACE ";" "\n" _skill_file_list "${CUTMACHINE_SKILL_FILES}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/procedure-files.txt"
     CONTENT "${_skill_file_list}\n")
set(CUTMACHINE_PROCEDURES_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ProceduresData.cc")
add_custom_command(
    OUTPUT "${CUTMACHINE_PROCEDURES_SOURCE}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSKILLS_DIR=${CUTMACHINE_SKILLS_DIR}"
            "-DOUTPUT=${CUTMACHINE_PROCEDURES_SOURCE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedProcedures.cmake"
    DEPENDS ${CUTMACHINE_SKILL_FILES}
            "${CMAKE_CURRENT_BINARY_DIR}/generated/procedure-files.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedProcedures.cmake"
    COMMENT "Embedding the skills as the engine's procedures"
    VERBATIM
)

# PRD19 -- ROADMAP.md. The commit a binary comes from, read when it is built
# and not when the tree is configured: this target runs at every build, and
# cmake/BuildInfo.cmake rewrites the generated source only when the version,
# the commit or its dirty flag changed, so a build at the same commit
# recompiles nothing. Outside a git checkout the commit is "unknown" and the
# build passes.
find_package(Git QUIET)
set(CUTMACHINE_BUILD_INFO_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildInfo.cc")
add_custom_target(cutmachine_build_info
    COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/src/BuildInfo.cc.in"
            "-DOUTPUT=${CUTMACHINE_BUILD_INFO_SOURCE}"
            "-DVERSION=${PROJECT_VERSION}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cmake"
    BYPRODUCTS "${CUTMACHINE_BUILD_INFO_SOURCE}"
    COMMENT "Reading the commit this build comes from"
    VERBATIM
)

add_library(cutmachine_model
    src/AcpClient.cc
    src/AcpProcess.cc
    src/AfterBridge.cc
    # PRD25 -- what an agent relayed to the window did, as the Agent panel
    # lists it and the timeline marks it.
    src/AgentJournal.cc
    src/BeatDetection.cc
    src/BoundaryAir.cc
    # PRD19 -- what this build is (BuildInfo.h), written by the target above.
    "${CUTMACHINE_BUILD_INFO_SOURCE}"
    src/CameraMetadata.cc
    # RM3 -- docs/roadmap/REGLES-2026-10.md. Where a carton ends, learned from
    # delivered captions inside the rule's bounds, and the forge that admits
    # a model only when it beats the rule on held-out projects.
    src/CaptionBreakModel.cc
    src/CaptionGeneration.cc
    # LG1 -- docs/roadmap/LEGENDES-2026-10.md. Where a carton goes, the rule
    # the export, Record and the measuring tools draw by.
    src/CaptionLayout.cc
    src/ChatLlmClient.cc
    src/ChatPresentation.cc
    src/ChatSession.cc
    src/Cli.cc
    # PRD20 -- the commands of the command line, described once.
    src/CliCommands.cc
    # HEADLESS-2026-10 H1 -- and read once, for the app and for the build
    # without a window (headless/).
    src/CliMain.cc
    src/ClipAnimation.cc
    # PRD24 -- what a clip's rectangle says on the timeline, and where.
    src/ClipBadges.cc
    # Out of lot -- the author, 29 October 2026. What a disabled clip does to the
    # picture and the sound, the rule every renderer asks.
    src/ClipEnabled.cc
    src/ClipFraming.cc
    # GR11 -- one clip's grade pasted onto others.
    src/ClipGradeCopy.cc
    src/ClipSpeed.cc
    src/ColorEffects.cc
    src/CutInspection.cc
    src/CutInspectionRender.cc
    src/CutReview.cc
    src/CutWords.cc
    src/ColorManagement.cc
    # BH3 -- the HLG delivery's curve, as DaVinci Resolve renders it.
    src/HlgDelivery.cc
    src/ColorCdl.cc
    src/ColorMatch.cc
    src/ColorMeasure.cc
    # PERF12 -- which decoders the window opens ahead of playback.
    src/DecoderPool.cc
    # PERF13 -- which layers of Record an opaque shot above hides.
    src/MonitorOcclusion.cc
    # PERF14 -- the source frames playback reads of a clip.
    src/PlaybackReads.cc
    # PERF15 -- one reader for the bytes of the pictures about to be shown.
    src/PlaybackReader.cc
    src/Document.cc
    src/DocumentFormat.cc
    # AX6 -- the font families motion and captions can name.
    src/FontCatalog.cc
    # SYNCHRO-2026-10 SY1 -- the offset between two sounds of one take.
    src/AudioOffset.cc
    # MOTION-MARQUE-2026-10 MM1 -- BS.1770 loudness and a true-peak limiter.
    src/Loudness.cc
    # FC5 -- FILMCRAFT-2026-10. The engine's own sound processing: EQ,
    # dynamics, true-peak limiter, denoise, de-hum.
    src/AudioDsp.cc
    src/AudioDspDenoise.cc
    # FC6 -- the ducking a music would follow under a voice, proposed.
    src/AudioDspDucking.cc
    src/AudioDspDynamics.cc
    src/AudioDspFilters.cc
    src/AudioDspTruePeak.cc
    # SY2 -- a recorder's sound laid under its picture, linked.
    src/SeparateSound.cc
    # SN4 -- a rendered stem laid at its BWF timecode.
    src/SoundStem.cc
    # SN7 -- a ditty rendered by cutmachine-sound-host, beside the engine.
    src/SoundRender.cc
    # MB10 -- the motion library in the user's personal SQLite base.
    src/MotionLibraryBase.cc
    src/Hesitations.cc
    # PRD13 -- who the loopback HTTP server answers, decided without a socket.
    src/HttpRequestPolicy.cc
    src/HttpServer.cc
    src/IdResolver.cc
    src/Ingest.cc
    src/InterviewShort.cc
    src/Json.cc
    # Out of lot -- the author, 4 November 2026. A link group split by place, for
    # the timelines cut before #213 (LinkGroupRepair.h).
    src/LinkGroupRepair.cc
    src/LocalEnv.cc
    src/LookLut.cc
    src/LookLutStorage.cc
    src/MediaTaskManager.cc
    src/EditLog.cc
    # MOTIF1 -- docs/roadmap/MOTIF-2026-10.md. Why an edit was made.
    src/EditMotif.cc
    src/EditPlan.cc
    # FC7 -- CMX 3600 lists of a timeline and its markers.
    src/Edl.cc
    # PA1 -- docs/roadmap/PARITE-2026-11.md. OTIO and FCPXML outputs.
    src/Interchange.cc
    # PRD35 -- the engine log a server and the app keep.
    src/EngineLog.cc
    src/Export.cc
    src/ExportVerification.cc
    src/MotionParallelRender.cc
    # PRD1 -- docs/roadmap/PRODUIT-2026-10.md. The one launcher of other
    # programs, and the one resolution of the FFmpeg binary.
    src/FfmpegBinary.cc
    # PRD2 -- what the engine asks of FFmpeg, checked before it is asked.
    src/FfmpegCapabilities.cc
    src/FfmpegPipe.cc
    src/ProcessLauncher.cc
    src/FrameCapture.cc
    # GENERATIF-2026-10 -- paid generation: proposals, fal.ai's queue behind
    # an injected transport, the extension canvas (portable).
    src/Generation.cc
    src/IsolatedFrames.cc
    src/Keyframes.cc
    src/MotionClip.cc
    src/MotionClipPreview.cc
    src/MotionComposition.cc
    src/MotionDocument.cc
    src/MotionEvaluate.cc
    # REMOTION-PARITY-2026-09 REM2 -- the media a composition plays, and
    # the sound it mixes (MotionEvaluate.cc) written for an encoder.
    src/MotionAudio.cc
    src/MotionMedia.cc
    # MOTION-AFTER-2026-09 -- a composition described for After Effects.
    src/MotionAfterExport.cc
    # MA2 -- a project After built, compared with the engine frame by frame.
    src/MotionAfterVerify.cc
    # MB7 -- an After composition read by the bridge, as a composition.
    src/MotionAfterImport.cc
    # REM3 -- where an evaluated node lands once transformed, and the
    # raster effects on what it paints.
    src/MotionEffects.cc
    src/MotionGeometry.cc
    # REM5 -- what a click selects and what a gesture commits.
    src/MotionCanvas.cc
    # MA6 -- a machine's composition rewritten at a human's level, and a
    # composition as After shows it, for the editor's panel.
    src/MotionHumanize.cc
    src/MotionOutline.cc
    src/MotionOperations.cc
    src/MotionPath.cc
    src/MotionPlacement.cc
    src/MotionSafeArea.cc
    src/MotionTemplates.cc
    "${CMAKE_CURRENT_BINARY_DIR}/generated/MotionTemplatesData.cc"
    src/MotionSpring.cc
    src/MotionVideo.cc
    src/Multicam.cc
    src/MulticamCli.cc
    # PRD14 -- the two names of a rush and the case of an enumeration, read
    # from each tool's schema before its dispatcher runs.
    src/McpArguments.cc
    src/McpEndpoint.cc
    src/McpLiveBackend.cc
    src/McpProjectBackend.cc
    src/McpGenerationTools.cc
    src/McpMotionClipTools.cc
    src/McpMotionTools.cc
    src/McpServer.cc
    src/McpSession.cc
    src/McpTools.cc
    # PROFIL-2026-11 PF1 -- the tools a server serves of the catalog.
    src/McpToolProfiles.cc
    # DECOUPAGE-2026-10 DC2 -- the catalog's tools by domain, sharing
    # McpToolsInternal.h with McpTools.cc.
    src/McpAfterTools.cc
    src/McpCaptionTools.cc
    src/McpClipTools.cc
    src/McpColorTools.cc
    src/McpDeliveryTools.cc
    src/McpEditTools.cc
    src/McpPictureTools.cc
    src/McpProjectTools.cc
    src/McpSequenceTools.cc
    src/McpSoundTools.cc
    src/McpSpeechTools.cc
    # GN6b -- the local voice-over's two tools and what makes a voice the
    # engine's (GeneratedVoice.h).
    src/McpVoiceTools.cc
    src/GeneratedVoice.cc
    src/MfaAlignment.cc
    src/ToolFiles.cc
    src/VoiceOver.cc
    src/OperationFormat.cc
    src/Operations.cc
    src/PauseTightening.cc
    # PRD18 -- the skills, embedded: the engine's procedures. Compiled in
    # this target only, so the rule that writes the source runs once.
    src/Procedures.cc
    # DEC44 -- docs/roadmap/DONNEES-2026-10.md. A capture cut into chunks
    # stored once each, compressed.
    src/ArchiveManifest.cc
    src/ProjectArchive.cc
    src/RunStep.cc
    "${CUTMACHINE_PROCEDURES_SOURCE}"
    src/Project.cc
    src/ProjectBin.cc
    src/ProjectDelta.cc
    src/ProjectRecovery.cc
    src/ProjectStorage.cc
    # DEC39 -- docs/roadmap/PHASES-2026-08.md. What the editor checks before
    # it reopens on another project.
    src/ProjectSwitch.cc
    src/RunStepStorage.cc
    src/Proxy.cc
    src/Relink.cc
    src/ResolveExport.cc
    # PA16 -- docs/roadmap/PARITE-2026-11.md. The send to Resolve, whole.
    src/ResolveSend.cc
    # DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. What was graded
    # in a Resolve project, read from a copy of its database.
    src/ResolveGrade.cc
    src/ResolveContrast.cc
    src/ResolveContrastTable.cc
    src/ResolveImport.cc
    src/ResolveXmlExport.cc
    src/SequenceFormat.cc
    # PRD21 -- what this machine lacks, item by item, before a tool fails.
    src/SetupCheck.cc
    # And what the window says of it, in French (the welcome screen's line,
    # the diagnosis behind the menu).
    src/SetupNotice.cc
    src/SourceAddress.cc
    src/DocumentDelta.cc
    src/ShotQuality.cc
    # PL1 -- docs/roadmap/PLANS-2026-10.md. Described shots: validated,
    # merged and searched, portable.
    src/ShotDescriptions.cc
    src/SpeechSelection.cc
    src/TranscriptAlignment.cc
    src/SpeechOnset.cc
    # CA1 -- docs/roadmap/CADRAGE-2026-10.md. A framing that follows its
    # subject, as keys on R2's position; and the faces it is read from.
    src/SubjectTracking.cc
    src/SubjectTrackingSampling.cc
    src/Stabilization.cc
    src/StabilizationSampling.cc
    src/SpeakerDiarization.cc
    src/Subtitles.cc
    src/TempoGrid.cc
    src/MusicStructure.cc
    src/Thumbnail.cc
    src/Timeline.cc
    src/TimelineSheets.cc
    src/TimelineTranscription.cc
    src/TimelineStats.cc
    # OR4 -- docs/roadmap/DONNEES-2026-10.md. A montage checked against a
    # named policy, read-only.
    src/TimelineCheck.cc
    src/TimelineAudioReport.cc
    src/TimelineView.cc
    src/Transcription.cc
    src/WhisperDecodingProfile.cc
    # DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. One Whisper model
    # setting, the engine's, which the window's menu writes.
    src/WhisperModelSetting.cc
    src/TransitionRender.cc
    # EF1 -- docs/roadmap/EFFETS-2026-10.md. The effects library and where
    # a dropped effect lands; portable, the tab only draws it.
    src/EffectsLibrary.cc
    # FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode as policy.
    src/TrimSession.cc
    src/Ulid.cc
    # HEADLESS-2026-10 H4 -- where this user's data and log go, per system.
    src/UserDirectories.cc
    src/MonitorCompare.cc
    src/WordBoundaries.cc
    # AGENT-2026-09 A24 -- the montage's words from the rushes' alignments.
    src/TimelineWords.cc
    src/VideoScopes.cc
    src/Waveform.cc
    src/WordAlignment.cc
)
target_compile_options(cutmachine_model PRIVATE
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
)
# PRD19 -- the generated source exists, and is current, before it compiles.
add_dependencies(cutmachine_model cutmachine_build_info)
target_include_directories(cutmachine_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(cutmachine_model PRIVATE
    CUTMACHINE_AFTER_BRIDGE_SCRIPTS="${AFTER_BRIDGE_SCRIPT_DIR}"
    # AX2 -- docs/roadmap/AX-2026-09.md. Where the build installs the aligner
    # (MFA since ALIGNEMENT-2026-11), and the script the engine runs in its
    # environment; CUTMACHINE_MFA_PYTHON, CUTMACHINE_MFA_ALIGNER and
    # CUTMACHINE_MFA_SCRIPT override them.
    CUTMACHINE_MFA_PYTHON_DEFAULT="${CMAKE_BINARY_DIR}/mfa/env/bin/python"
    CUTMACHINE_MFA_ALIGNER_DEFAULT="${CMAKE_BINARY_DIR}/mfa/env/bin/mfa"
    CUTMACHINE_MFA_SCRIPT_DEFAULT="${CMAKE_SOURCE_DIR}/scripts/mfa_align.py"
    # GN6a -- docs/roadmap/GENERATIF-2026-10.md. Where the build installs the
    # local voice-over's environment and models, and its script;
    # CUTMACHINE_VOICE_PYTHON, CUTMACHINE_VOICE_SCRIPT and
    # CUTMACHINE_VOICE_MODELS override them.
    CUTMACHINE_VOICE_PYTHON_DEFAULT="${CMAKE_BINARY_DIR}/voice/env/bin/python"
    CUTMACHINE_VOICE_SCRIPT_DEFAULT="${CMAKE_SOURCE_DIR}/scripts/voice_over.py"
    CUTMACHINE_VOICE_MODELS_DEFAULT="${CMAKE_BINARY_DIR}/voice/models"
    # PA3 -- docs/roadmap/PARITE-2026-11.md. Where the build installs the
    # diarization's environment and its speaker embedding model, and its
    # script; CUTMACHINE_DIARIZATION_PYTHON, CUTMACHINE_DIARIZATION_SCRIPT
    # and CUTMACHINE_DIARIZATION_MODEL override them.
    CUTMACHINE_DIARIZATION_PYTHON_DEFAULT="${CMAKE_BINARY_DIR}/diarization/env/bin/python"
    CUTMACHINE_DIARIZATION_SCRIPT_DEFAULT="${CMAKE_SOURCE_DIR}/scripts/diarize.py"
    CUTMACHINE_DIARIZATION_MODEL_DEFAULT="${CMAKE_BINARY_DIR}/diarization/models/wespeaker_en_voxceleb_resnet34_LM.onnx"
    # PA16 -- docs/roadmap/PARITE-2026-11.md. The Resolve bridge the send
    # runs; CUTMACHINE_RESOLVE_BRIDGE and CUTMACHINE_RESOLVE_PYTHON override.
    CUTMACHINE_RESOLVE_BRIDGE_DEFAULT="${CMAKE_SOURCE_DIR}/sidecar/resolve_bridge.py"
)
target_link_libraries(cutmachine_model PUBLIC
    PkgConfig::FFMPEG
    OpenColorIO
    whisper
    Threads::Threads
    "-framework Foundation"
    "-framework Security"
)
# Yoga stays an implementation detail of MotionEvaluate.cc: no header of this
# project includes it.
target_link_libraries(cutmachine_model PRIVATE yogacore cutmachine_sqlite
                      cutmachine_zstd)

# MOTION-2026-09 M2 -- ROADMAP.md. The macOS half of motion: Core Text
# measures and Core Graphics draws what MotionEvaluate (portable, in the model)
# already laid out. Kept out of cutmachine_model so layout policy never
# depends on a framework.
add_library(cutmachine_motion_render STATIC
    # LG1 -- docs/roadmap/LEGENDES-2026-10.md. A carton measured and drawn
    # by Core Text, with the fonts motion resolves.
    src/CaptionRender.mm
    src/MotionCli.cc
    src/MotionClipPreviewRender.cc
    src/MotionRender.mm
    # MOTION-2026-09 M6 -- frames of a motion video, drawn for FFmpeg.
    src/MotionVideoRender.cc
    # AGENT-2026-09 A20 -- Vision's face rectangles, the engine's face
    # finder for a grade read on the skin (FaceFinder.h).
    src/FaceFinderVision.mm
)
target_include_directories(cutmachine_motion_render PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cutmachine_motion_render PUBLIC
    cutmachine_model
    "-framework CoreFoundation"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework ImageIO"
    "-framework Vision"
)

set(CUTMACHINE_APP_SOURCES
    src/main.mm
    # GENERATIF-2026-10 GN1 -- the HTTPS transport and the Keychain key of
    # paid generation (Generation.h), installed by main.mm.
    src/GenerationTransport.mm
    src/AudioPlayback.mm
    # PERF13 -- the monitor's sound of one rush, its sound alone. Plain C++
    # over FFmpeg (tests/audio_decode_tests.cc).
    src/AudioDecode.cc
    src/DecodeWorker.cc
    src/FrameCache.cc
    # PERF14 -- what playback reads ahead, and how far (portable).
    src/ReadAhead.cc
    src/PerformanceMetrics.cc
    src/MediaSource.mm
    src/Renderer.mm
    # CF1 -- a passive second-screen surface of Record's composite.
    src/VideoOutput.mm
    # VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The window drawn off
    # screen, Metal surfaces included (capture_editor_window, ui_smoke).
    src/WindowCapture.mm
    src/MetalUiAtlas.mm
    # F2.1 -- design system (ROADMAP.md). UiTheme.h/PanelLayout.h are plain
    # C++ (see tests/ui_theme_tests.cc, tests/panel_layout_tests.cc) and
    # need no .cc of their own; everything below is AppKit/Foundation-only.
    src/UiThemeAppKit.mm
    src/UiPreferences.mm
    src/UiComponents.mm
    src/PanelHostView.mm
    # PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The form panel of the
    # window, a sheet instead of an alert. FormPanelLayout.h (plain C++,
    # tests/form_panel_layout_tests.cc) decides; this only measures and
    # applies.
    src/FormPanel.mm
    # F2.2 -- Inspector (ROADMAP.md). InspectorGradeControls.h is plain
    # C++ (see tests/inspector_grade_controls_tests.cc) and needs no .cc of
    # its own; InspectorView.mm is AppKit-only, same as the rest of this
    # block.
    src/InspectorView.mm
    src/WorkspaceBar.mm
    src/TrimBarView.mm
    src/TrimCursors.mm
    # EF1 -- docs/roadmap/EFFETS-2026-10.md. The « Effets » tab; its policy
    # is EffectsLibrary.h.
    src/EffectsPanelView.mm
    # GR4, GR5 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The colour wheels
    # and the curve editor of the Inspector. GradeWheelModel.h and
    # GradeCurveModel.h (plain C++, tests/grade_control_models_tests.cc)
    # decide; these only draw and relay.
    src/GradeWheelView.mm
    src/GradeCurveView.mm
    # GR8 -- the hue curves' editor (GradeHueCurveModel.h decides).
    src/GradeHueCurveView.mm
    # F2.4 -- chat panel (ROADMAP.md). ChatLlmClient.cc/ChatSession.cc/
    # McpLiveBackend.cc are plain C++ and already part of cutmachine_model
    # above (see tests/chat_llm_client_tests.cc, tests/chat_session_tests.cc);
    # this is only the AppKit view that wires them to the live document.
    src/ChatPanelView.mm
    src/ChatMessageView.mm
    # REMOTION-PARITY-2026-09 REM5 -- the visual editor of motion: the
    # selection over the Record monitor and the Inspector of a node.
    # MotionCanvas.h (model, tested) decides; these only draw and relay.
    src/MotionCanvasView.mm
    src/MotionInspectorView.mm
)

# BUILD-2026-09 -- `cutmachine` and `cutmachine_app` compile the same sources
# with the same options, so they share one set of objects: main.mm alone takes
# about 15 s to compile and used to be compiled once per executable (three
# times with the UI smoke test, which keeps its own objects because
# CUTMACHINE_UI_SMOKE_TEST compiles its harness in).
add_library(cutmachine_app_objects OBJECT ${CUTMACHINE_APP_SOURCES})
add_dependencies(cutmachine_app_objects cutmachine_ui_icons)
target_include_directories(cutmachine_app_objects PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(cutmachine_app_objects PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
)
target_compile_definitions(cutmachine_app_objects PRIVATE
    CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
    CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
)
target_link_libraries(cutmachine_app_objects PUBLIC
    cutmachine_model
    cutmachine_motion_render
    PkgConfig::FFMPEG
    "-framework AppKit"
    "-framework AVFAudio"
    "-framework Foundation"
    "-framework Metal"
    # EF3 -- docs/roadmap/EFFETS-2026-10.md. Record's Gaussian blur.
    "-framework MetalPerformanceShaders"
    "-framework QuartzCore"
    "-framework Security"
    "-framework UniformTypeIdentifiers"
)

add_executable(cutmachine $<TARGET_OBJECTS:cutmachine_app_objects>)
target_link_libraries(cutmachine PRIVATE cutmachine_app_objects)

# A regular macOS application for Finder/Dock launches. The plain `cutmachine`
# executable remains available for the CLI commands and automated workflows.
add_executable(cutmachine_app MACOSX_BUNDLE
    $<TARGET_OBJECTS:cutmachine_app_objects>
    src/shader.metal
    ${CUTMACHINE_ICON_PNGS}
    assets/icons/LICENSE
    # PRD23 -- every third-party licence, and THIRD_PARTY_NOTICES.md.
    ${CUTMACHINE_LICENSE_RESOURCES}
)
set_source_files_properties(src/shader.metal PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
set_source_files_properties(${CUTMACHINE_ICON_PNGS} PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources/icons
)
set_source_files_properties(assets/icons/LICENSE PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources/icons
)
set_source_files_properties(${CUTMACHINE_LICENSE_RESOURCES}
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licenses
)
add_dependencies(cutmachine_app cutmachine_ui_icons cutmachine_ocio_ext_licenses)
set_target_properties(cutmachine_app PROPERTIES
    OUTPUT_NAME CUTMACHINE
    MACOSX_BUNDLE_BUNDLE_NAME CUTMACHINE
    MACOSX_BUNDLE_GUI_IDENTIFIER com.cutmachine.editor
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    # PRD19 -- the project's one version number, not a second literal.
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
)
target_link_libraries(cutmachine_app PRIVATE cutmachine_app_objects)

# SN7 -- docs/roadmap/SON-2026-10.md. The sound runtime's JavaScriptCore host: a process of its
# own, launched by the engine (SoundRender.h) from beside its executable, and the only one signed
# with com.apple.security.cs.allow-jit -- without it a JSContext runs interpreted, 16 times slower
# (SN6), and the entitlement would otherwise cover the editor's whole process. The runtime is
# tools/cutmachine-sound's own JavaScript, joined into one script at build time (no Node needed).
set(CUTMACHINE_SOUND_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/cutmachine-sound")
set(CUTMACHINE_SOUND_BUNDLE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/SoundRuntimeBundle.cc")
add_custom_command(
    OUTPUT "${CUTMACHINE_SOUND_BUNDLE}"
    COMMAND "${CMAKE_COMMAND}" "-DSOUND_DIR=${CUTMACHINE_SOUND_DIR}"
            "-DOUTPUT=${CUTMACHINE_SOUND_BUNDLE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SoundBundle.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SoundBundle.cmake"
            "${CUTMACHINE_SOUND_DIR}/time.mjs" "${CUTMACHINE_SOUND_DIR}/edl.mjs"
            "${CUTMACHINE_SOUND_DIR}/project.mjs"
            "${CUTMACHINE_SOUND_DIR}/partition.mjs"
            "${CUTMACHINE_SOUND_DIR}/runtime.mjs" "${CUTMACHINE_SOUND_DIR}/wav.mjs"
            "${CUTMACHINE_SOUND_DIR}/job.mjs"
    COMMENT "Joining the sound runtime for JavaScriptCore"
    VERBATIM
)
add_executable(cutmachine-sound-host src/SoundHost.mm "${CUTMACHINE_SOUND_BUNDLE}")
target_compile_options(cutmachine-sound-host PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(cutmachine-sound-host PRIVATE
    "-framework Foundation" "-framework JavaScriptCore")
add_custom_command(TARGET cutmachine-sound-host POST_BUILD
    COMMAND codesign --force --sign - --options runtime --entitlements
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cutmachine-sound-host.entitlements"
            "$<TARGET_FILE:cutmachine-sound-host>"
    COMMENT "Signing cutmachine-sound-host with allow-jit"
    VERBATIM
)
add_dependencies(cutmachine cutmachine-sound-host)
# Into the app beside its executable, where the engine looks for it; on every build, so that the
# app never keeps an older host than the one just built.
add_custom_target(cutmachine_sound_host_in_app ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:cutmachine-sound-host>"
            "$<TARGET_BUNDLE_CONTENT_DIR:cutmachine_app>/MacOS/cutmachine-sound-host"
    DEPENDS cutmachine-sound-host cutmachine_app
    VERBATIM
)

# DEV-2026-08 -- keep the build-and-launch path in CMake so every generator
# opens the exact bundle it just produced, including non-default build dirs.
add_custom_target(run-app
    COMMAND /usr/bin/open "$<TARGET_BUNDLE_DIR:cutmachine_app>"
    DEPENDS cutmachine_app
    COMMENT "Launching CUTMACHINE.app"
    VERBATIM
)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_ffmpeg_includes PkgConfig::FFMPEG
                        INTERFACE_INCLUDE_DIRECTORIES)
    list(GET _ffmpeg_includes 0 _ffmpeg_include)
    get_filename_component(_ffmpeg_prefix "${_ffmpeg_include}" DIRECTORY)
    find_program(FFMPEG_EXECUTABLE NAMES ffmpeg
                 HINTS "${_ffmpeg_prefix}/bin" REQUIRED)


    add_executable(cutmachine_tests tests/model_tests.cc)
    target_link_libraries(cutmachine_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_model_tests COMMAND cutmachine_tests)

    add_executable(cutmachine_edit_tests tests/edit_tests.cc)
    target_link_libraries(cutmachine_edit_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_edit_tests COMMAND cutmachine_edit_tests)

    # PL2 -- ROADMAP.md. Edit plans in the engine: prepared on a copy,
    # applied as previewed, refused when stale. Portable.
    add_executable(cutmachine_edit_plan_tests tests/edit_plan_tests.cc)
    target_link_libraries(cutmachine_edit_plan_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_edit_plan_tests COMMAND cutmachine_edit_plan_tests)
    set_tests_properties(cutmachine_edit_plan_tests PROPERTIES LABELS "portable")

    # MOTIF1, MOTIF4 -- docs/roadmap/MOTIF-2026-10.md. Why an edit was made,
    # in the journal: old journals keep their bytes, motifs round-trip,
    # folds keep them, list_motifs groups them. Portable.
    add_executable(cutmachine_edit_motif_tests tests/edit_motif_tests.cc)
    target_link_libraries(cutmachine_edit_motif_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_edit_motif_tests PRIVATE
        EDIT_LOG_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/edit_log")
    add_test(NAME cutmachine_edit_motif_tests COMMAND cutmachine_edit_motif_tests)
    set_tests_properties(cutmachine_edit_motif_tests PROPERTIES LABELS "portable")

    add_executable(cutmachine_run_step_tests tests/run_step_tests.cc)
    target_link_libraries(cutmachine_run_step_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_run_step_tests COMMAND cutmachine_run_step_tests)
    set_tests_properties(cutmachine_run_step_tests PROPERTIES LABELS "portable")
    add_executable(cutmachine_run_step_storage_tests tests/run_step_storage_tests.cc)
    target_link_libraries(cutmachine_run_step_storage_tests PRIVATE cutmachine_model cutmachine_sqlite)
    add_test(NAME cutmachine_run_step_storage_tests COMMAND cutmachine_run_step_storage_tests)
    set_tests_properties(cutmachine_run_step_storage_tests PROPERTIES LABELS "storage" TIMEOUT 120)
    # OR3 -- DONNEES-2026-10. The Agent panel's ACP trace report needs
    # neither a model nor AppKit; keep its failure cases portable.
    add_test(NAME cutmachine_acp_report_tests
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/acp-report/test_acp_report.py")
    set_tests_properties(cutmachine_acp_report_tests PROPERTIES LABELS "portable")

    # DEC44 -- docs/roadmap/DONNEES-2026-10.md. The cut of a capture into
    # chunks, its manifest and the chunk codec, without SQLite.
    add_executable(cutmachine_archive_manifest_tests
                   tests/archive_manifest_tests.cc)
    target_link_libraries(cutmachine_archive_manifest_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_archive_manifest_tests
             COMMAND cutmachine_archive_manifest_tests)
    set_tests_properties(cutmachine_archive_manifest_tests PROPERTIES
        LABELS "portable")

    add_executable(cutmachine_project_archive_tests tests/project_archive_tests.cc)
    target_link_libraries(cutmachine_project_archive_tests PRIVATE cutmachine_model cutmachine_sqlite)
    add_test(NAME cutmachine_project_archive_tests COMMAND cutmachine_project_archive_tests)
    set_tests_properties(cutmachine_project_archive_tests PROPERTIES LABELS "portable")

    add_executable(cutmachine_project_archive_delivery_tests
                   tests/project_archive_delivery_tests.cc)
    target_link_libraries(cutmachine_project_archive_delivery_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_archive_delivery_tests
             COMMAND cutmachine_project_archive_delivery_tests)
    set_tests_properties(cutmachine_project_archive_delivery_tests PROPERTIES
        LABELS "storage" TIMEOUT 120)

    # FORMAT-2026-09 -- the journal format of every operation, pinned by
    # fixtures the hand-written codec produced.
    add_executable(cutmachine_operation_format_tests
                   tests/operation_format_tests.cc)
    target_link_libraries(cutmachine_operation_format_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_operation_format_tests PRIVATE
        OPERATION_FORMAT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/operation_format")
    add_test(NAME cutmachine_operation_format_tests
             COMMAND cutmachine_operation_format_tests)

    # FORMAT-2026-09 F6 -- documents, project files and project deltas,
    # pinned by fixtures the hand-written codec produced.
    add_executable(cutmachine_document_format_tests
                   tests/document_format_tests.cc)
    target_link_libraries(cutmachine_document_format_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_document_format_tests PRIVATE
        DOCUMENT_FORMAT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/document_format")
    add_test(NAME cutmachine_document_format_tests
             COMMAND cutmachine_document_format_tests)

    add_executable(cutmachine_timeline_view_tests
                   tests/timeline_view_tests.cc)
    target_link_libraries(cutmachine_timeline_view_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_timeline_view_tests
             COMMAND cutmachine_timeline_view_tests)

    add_executable(cutmachine_timeline_stats_tests
                   tests/timeline_stats_tests.cc)
    target_link_libraries(cutmachine_timeline_stats_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_timeline_stats_tests
             COMMAND cutmachine_timeline_stats_tests)

    # OR4 -- docs/roadmap/DONNEES-2026-10.md. The policy, the end of the film
    # composed as the export composes it, and the verdict on measures handed
    # in. Portable: no AppKit, no Metal, and FFmpeg is never run.
    add_executable(cutmachine_timeline_check_tests
                   tests/timeline_check_tests.cc)
    target_link_libraries(cutmachine_timeline_check_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_timeline_check_tests
             COMMAND cutmachine_timeline_check_tests)
    set_tests_properties(cutmachine_timeline_check_tests
                         PROPERTIES LABELS "portable")

    # F1.5 -- ROADMAP.md. The multicam picture rule and the builders every
    # surface shares, with the consumers that must agree on them.
    add_executable(cutmachine_multicam_tests tests/multicam_tests.cc)
    target_link_libraries(cutmachine_multicam_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_multicam_tests COMMAND cutmachine_multicam_tests)

    # Out of lot -- the author, 29 October 2026. A disabled clip is neither seen
    # nor heard: the consumers that assemble the timeline, and the toggle of
    # the window's D (ClipEnabled.h). Portable: no AppKit, FFmpeg never run.
    add_executable(cutmachine_clip_enabled_tests tests/clip_enabled_tests.cc)
    target_link_libraries(cutmachine_clip_enabled_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_clip_enabled_tests
             COMMAND cutmachine_clip_enabled_tests)
    set_tests_properties(cutmachine_clip_enabled_tests
                         PROPERTIES LABELS "portable")

    # Out of lot -- the author, 4 November 2026. A link group that binds several
    # places of a timeline cut before #213, split one group per place, as
    # one undo step (LinkGroupRepair.h). Portable: no AppKit, no FFmpeg.
    add_executable(cutmachine_link_group_repair_tests
                   tests/link_group_repair_tests.cc)
    target_link_libraries(cutmachine_link_group_repair_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_link_group_repair_tests
             COMMAND cutmachine_link_group_repair_tests)
    set_tests_properties(cutmachine_link_group_repair_tests
                         PROPERTIES LABELS "portable")

    # R1 -- docs/roadmap/REEL-2026-09.md. The one rectangle: where a
    # clip's picture lands in the sequence frame, and which part of the
    # source fills it. Portable: no AppKit, no Metal, no FFmpeg.
    # PL1 to PL4 -- docs/roadmap/PLANS-2026-10.md. Described shots:
    # validation, the operation and its undo, the document version, the
    # merge, the search. Portable: no AppKit, no FFmpeg.
    add_executable(cutmachine_shot_descriptions_tests
                   tests/shot_descriptions_tests.cc)
    target_link_libraries(cutmachine_shot_descriptions_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_shot_descriptions_tests
             COMMAND cutmachine_shot_descriptions_tests)
    set_tests_properties(cutmachine_shot_descriptions_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_clip_framing_tests tests/clip_framing_tests.cc)
    target_link_libraries(cutmachine_clip_framing_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_clip_framing_tests
             COMMAND cutmachine_clip_framing_tests)
    set_tests_properties(cutmachine_clip_framing_tests
                         PROPERTIES LABELS "portable")

    # R2 -- docs/roadmap/REEL-2026-09.md. What an animated parameter is
    # worth at a frame, and what its keys become under a move, a head trim
    # and a cut. Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_clip_animation_tests
                   tests/clip_animation_tests.cc)
    target_link_libraries(cutmachine_clip_animation_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_clip_animation_tests
             COMMAND cutmachine_clip_animation_tests)
    set_tests_properties(cutmachine_clip_animation_tests
                         PROPERTIES LABELS "portable")

    # CA1, CA2 -- docs/roadmap/CADRAGE-2026-10.md. The operator's rule on
    # synthetic trajectories (dead zone, speed, gaps, several faces), and the
    # operations it lays on a clip, applied and undone byte for byte.
    # Portable: no AppKit, no Metal, no FFmpeg, no face finder.
    add_executable(cutmachine_subject_tracking_tests
                   tests/subject_tracking_tests.cc)
    target_link_libraries(cutmachine_subject_tracking_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_subject_tracking_tests
             COMMAND cutmachine_subject_tracking_tests)
    set_tests_properties(cutmachine_subject_tracking_tests
                         PROPERTIES LABELS "portable")

    # PA7 -- docs/roadmap/PARITE-2026-11.md. A handheld shot steadied: the
    # rule on synthetic paths, the operations applied and undone byte for
    # byte, and the movement read back from a rush FFmpeg makes with shifts
    # that are known. No AppKit, no Metal; FFmpeg for the last part.
    add_executable(cutmachine_stabilization_tests
                   tests/stabilization_tests.cc)
    target_link_libraries(cutmachine_stabilization_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_stabilization_tests
        PRIVATE FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_stabilization_tests
             COMMAND cutmachine_stabilization_tests)

    # PA8 -- docs/roadmap/PARITE-2026-11.md. A clip's mask: the rule, and
    # FFmpeg's filter held to it pixel by pixel. Header only, with FFmpeg.
    add_executable(cutmachine_clip_mask_tests tests/clip_mask_tests.cc)
    target_link_libraries(cutmachine_clip_mask_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_clip_mask_tests
        PRIVATE FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_clip_mask_tests
             COMMAND cutmachine_clip_mask_tests)

    # PA3 -- docs/roadmap/PARITE-2026-11.md. Who speaks when: the script's
    # report, the cache, the voice of a word, where the files are looked
    # for, and the tools on a stored project with a script standing for the
    # model. No AppKit; FFmpeg for the rush the tools read.
    add_executable(cutmachine_speaker_diarization_tests
                   tests/speaker_diarization_tests.cc)
    target_link_libraries(cutmachine_speaker_diarization_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_speaker_diarization_tests
        PRIVATE FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_speaker_diarization_tests
             COMMAND cutmachine_speaker_diarization_tests)

    # PRD24 -- ROADMAP.md. What a clip carries, as its rectangle on the
    # timeline says it: which badges, their text, their order, which of
    # them a narrow clip keeps, and where each one sits. The display list
    # only draws what this decides. Portable: no AppKit, no Metal.
    # LG1 -- docs/roadmap/LEGENDES-2026-10.md. Where a carton goes: the
    # rule the export, Record and the measuring tools share. Portable: the
    # font is described by its measures, no Core Text.
    # RM3 -- docs/roadmap/REGLES-2026-10.md. The learned carton boundary: a
    # model without weights is the rule, training is deterministic, the
    # forge admits only what beats the rule held out. Portable: no AppKit,
    # no renderer.
    add_executable(cutmachine_caption_break_model_tests
                   tests/caption_break_model_tests.cc)
    target_link_libraries(cutmachine_caption_break_model_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_caption_break_model_tests
             COMMAND cutmachine_caption_break_model_tests)
    set_tests_properties(cutmachine_caption_break_model_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_caption_layout_tests
                   tests/caption_layout_tests.cc)
    target_link_libraries(cutmachine_caption_layout_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_caption_layout_tests
             COMMAND cutmachine_caption_layout_tests)
    set_tests_properties(cutmachine_caption_layout_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_clip_badges_tests tests/clip_badges_tests.cc)
    target_link_libraries(cutmachine_clip_badges_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_clip_badges_tests
             COMMAND cutmachine_clip_badges_tests)
    set_tests_properties(cutmachine_clip_badges_tests
                         PROPERTIES LABELS "portable")

    # PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The journal of what an
    # agent relayed to the window did: which calls enter it, the French line
    # each becomes, the fold of an intervention, the bound, the clips the
    # timeline marks. Portable: no AppKit, no Metal, no socket.
    add_executable(cutmachine_agent_journal_tests
                   tests/agent_journal_tests.cc)
    target_link_libraries(cutmachine_agent_journal_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_agent_journal_tests
             COMMAND cutmachine_agent_journal_tests)
    set_tests_properties(cutmachine_agent_journal_tests
                         PROPERTIES LABELS "portable")

    # R5 -- docs/roadmap/REEL-2026-09.md. Where a transition falls and
    # what it is worth at every frame it covers: the one answer the
    # monitor, the export, Validate and verify_export all read.
    # Portable: no AppKit, no Metal, no FFmpeg.
    # IA4 -- docs/roadmap/IA-2026-09.md. Which window and which frames an
    # inspection of a cut renders, its level arithmetic and the comparison
    # of the words expected with the words heard.
    # Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_cut_inspection_tests
                   tests/cut_inspection_tests.cc)
    target_link_libraries(cutmachine_cut_inspection_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_cut_inspection_tests
             COMMAND cutmachine_cut_inspection_tests)
    set_tests_properties(cutmachine_cut_inspection_tests
                         PROPERTIES LABELS "portable")

    # RC1 -- docs/roadmap/RACCORDS-2026-10.md. The junctions of a timeline,
    # the frames of a band, the sound and picture measures of a junction and
    # the alerts their thresholds raise, on synthetic samples and planes.
    # Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_cut_review_tests tests/cut_review_tests.cc)
    target_link_libraries(cutmachine_cut_review_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_cut_review_tests
             COMMAND cutmachine_cut_review_tests)
    set_tests_properties(cutmachine_cut_review_tests
                         PROPERTIES LABELS "portable")

    # IA3 -- docs/roadmap/IA-2026-09.md. Blocks of speech, the revision of
    # the transcript view a selection is read from, and what a selection
    # leaves out around it. Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_speech_selection_tests
                   tests/speech_selection_tests.cc)
    target_link_libraries(cutmachine_speech_selection_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_speech_selection_tests
             COMMAND cutmachine_speech_selection_tests)
    set_tests_properties(cutmachine_speech_selection_tests
                         PROPERTIES LABELS "portable")

    # AGENT-2026-09 A4 -- docs/roadmap/AGENT-2026-09.md. Where to enter and
    # leave on a word, from the aligned words and the measured envelope.
    # Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_word_boundaries_tests
                   tests/word_boundaries_tests.cc)
    target_link_libraries(cutmachine_word_boundaries_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_word_boundaries_tests
             COMMAND cutmachine_word_boundaries_tests)
    set_tests_properties(cutmachine_word_boundaries_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_timeline_words_tests
                   tests/timeline_words_tests.cc)
    target_link_libraries(cutmachine_timeline_words_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_timeline_words_tests
             COMMAND cutmachine_timeline_words_tests)
    set_tests_properties(cutmachine_timeline_words_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_transition_render_tests
                   tests/transition_render_tests.cc)
    target_link_libraries(cutmachine_transition_render_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_transition_render_tests
             COMMAND cutmachine_transition_render_tests)
    set_tests_properties(cutmachine_transition_render_tests
                         PROPERTIES LABELS "portable")

    # ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. The window's pages: what
    # each opens and how wide (WorkspacePages.h). Portable.
    add_executable(cutmachine_workspace_pages_tests
                   tests/workspace_pages_tests.cc)
    target_link_libraries(cutmachine_workspace_pages_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_workspace_pages_tests
             COMMAND cutmachine_workspace_pages_tests)
    set_tests_properties(cutmachine_workspace_pages_tests
                         PROPERTIES LABELS "portable")

    # ON3 -- docs/roadmap/ONGLETS-2026-10.md. Where the Agent's floating
    # window opens (AgentWindow.h). Portable.
    add_executable(cutmachine_agent_window_tests
                   tests/agent_window_tests.cc)
    target_link_libraries(cutmachine_agent_window_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_agent_window_tests
             COMMAND cutmachine_agent_window_tests)
    set_tests_properties(cutmachine_agent_window_tests
                         PROPERTIES LABELS "portable")

    # R2b -- docs/roadmap/REEL-2026-09.md. A fade carried by a motion clip's
    # opacity keys, exported by FFmpeg and read back. The card comes from a
    # stand-in renderer, so the suite needs no Core Text.
    add_executable(cutmachine_motion_clip_opacity_export_tests
                   tests/motion_clip_opacity_export_tests.cc)
    target_link_libraries(cutmachine_motion_clip_opacity_export_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_motion_clip_opacity_export_tests
        PRIVATE FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_motion_clip_opacity_export_tests
             COMMAND cutmachine_motion_clip_opacity_export_tests)
    set_tests_properties(cutmachine_motion_clip_opacity_export_tests
                         PROPERTIES LABELS "portable")

    # EF1 -- docs/roadmap/EFFETS-2026-10.md. Where an effect dropped on the
    # timeline lands, and the operation it applies. Portable.
    add_executable(cutmachine_effects_library_tests
                   tests/effects_library_tests.cc)
    target_link_libraries(cutmachine_effects_library_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_effects_library_tests
             COMMAND cutmachine_effects_library_tests)
    set_tests_properties(cutmachine_effects_library_tests
                         PROPERTIES LABELS "portable")

    # R6 -- docs/roadmap/REEL-2026-09.md. What a .cube is read as, what
    # it is refused for, what it is worth at a colour, and where in the
    # chain the document says the look belongs.
    # Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_look_lut_tests tests/look_lut_tests.cc)
    target_link_libraries(cutmachine_look_lut_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_look_lut_tests COMMAND cutmachine_look_lut_tests)
    set_tests_properties(cutmachine_look_lut_tests
                         PROPERTIES LABELS "portable")

    # G1 -- docs/roadmap/GRADE-2026-09.md. The colour measure on pictures
    # whose numbers are known by hand. Portable: no AppKit, no Metal, no
    # FFmpeg.
    add_executable(cutmachine_color_measure_tests tests/color_measure_tests.cc)
    target_link_libraries(cutmachine_color_measure_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_color_measure_tests
             COMMAND cutmachine_color_measure_tests)
    set_tests_properties(cutmachine_color_measure_tests
                         PROPERTIES LABELS "portable")

    # G7 -- a Sony camera's white balance read out of an rtmd packet, on
    # packets built by hand. Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_camera_metadata_tests
                   tests/camera_metadata_tests.cc)
    target_link_libraries(cutmachine_camera_metadata_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_camera_metadata_tests
             COMMAND cutmachine_camera_metadata_tests)
    set_tests_properties(cutmachine_camera_metadata_tests
                         PROPERTIES LABELS "portable")

    # G6 -- the monitor's comparison: the split's geometry and the still's
    # planes. Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_monitor_compare_tests
                   tests/monitor_compare_tests.cc)
    target_link_libraries(cutmachine_monitor_compare_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_monitor_compare_tests
             COMMAND cutmachine_monitor_compare_tests)
    set_tests_properties(cutmachine_monitor_compare_tests
                         PROPERTIES LABELS "portable")

    # G4 -- the CDL checked against OpenColorIO's own processor, folded
    # from a stack, and through its files. Portable: no AppKit, no Metal, no
    # FFmpeg.
    add_executable(cutmachine_color_cdl_tests tests/color_cdl_tests.cc)
    target_link_libraries(cutmachine_color_cdl_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_color_cdl_tests COMMAND cutmachine_color_cdl_tests)
    set_tests_properties(cutmachine_color_cdl_tests
                         PROPERTIES LABELS "portable")

    # BH3 -- the HLG delivery held to Resolve's render of the S-Log3 chart,
    # and the OpenColorIO chain held to its CPU reference. Portable: no
    # AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_hlg_delivery_tests tests/hlg_delivery_tests.cc)
    target_link_libraries(cutmachine_hlg_delivery_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_hlg_delivery_tests
             COMMAND cutmachine_hlg_delivery_tests)
    set_tests_properties(cutmachine_hlg_delivery_tests
                         PROPERTIES LABELS "portable")

    # G2 -- the match on values known by hand, and the fold checked against
    # the renderer's arithmetic. Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_color_match_tests tests/color_match_tests.cc)
    target_link_libraries(cutmachine_color_match_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_color_match_tests
             COMMAND cutmachine_color_match_tests)
    set_tests_properties(cutmachine_color_match_tests
                         PROPERTIES LABELS "portable")

    # The same measure end to end: files decoded by FFmpeg, then the grade,
    # the framing, the colour management and the look of the document.
    add_executable(cutmachine_color_measure_render_tests
                   tests/color_measure_render_tests.cc)
    target_link_libraries(cutmachine_color_measure_render_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_color_measure_render_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_color_measure_render_tests
             COMMAND cutmachine_color_measure_render_tests)

    # FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. Slide and rate stretch: what
    # each applies, its byte-exact undo and redo, its canonical journal entry
    # and its refusals. Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_slide_rate_stretch_tests
                   tests/slide_rate_stretch_tests.cc)
    target_link_libraries(cutmachine_slide_rate_stretch_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_slide_rate_stretch_tests
             COMMAND cutmachine_slide_rate_stretch_tests)
    set_tests_properties(cutmachine_slide_rate_stretch_tests
                         PROPERTIES LABELS "portable")

    # R3 -- docs/roadmap/REEL-2026-09.md. The clip speed rule, frame by
    # frame, and the surfaces that must all name the same source frame.
    # Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_clip_speed_tests tests/clip_speed_tests.cc)
    target_link_libraries(cutmachine_clip_speed_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_clip_speed_tests
             COMMAND cutmachine_clip_speed_tests)
    set_tests_properties(cutmachine_clip_speed_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_timeline_sheets_tests
                   tests/timeline_sheets_tests.cc)
    target_link_libraries(cutmachine_timeline_sheets_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_timeline_sheets_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_timeline_sheets_tests
             COMMAND cutmachine_timeline_sheets_tests)

    # PRD19 -- ROADMAP.md. What the build says it is: a version of the
    # project's form, the commit of the checkout it was built in.
    # Portable: no AppKit, no Metal, no FFmpeg.
    add_executable(cutmachine_build_info_tests tests/build_info_tests.cc)
    target_link_libraries(cutmachine_build_info_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_build_info_tests PRIVATE
        CUTMACHINE_PROJECT_VERSION="${PROJECT_VERSION}")
    add_test(NAME cutmachine_build_info_tests
             COMMAND cutmachine_build_info_tests)
    set_tests_properties(cutmachine_build_info_tests
                         PROPERTIES LABELS "portable")

    # PRD35 -- ROADMAP.md. The engine log: a line's shape, the folder, the
    # rotation, writers in several processes, and the line a crash leaves --
    # played by forked children, never by this test's own process.
    # Portable: POSIX only, no AppKit, no FFmpeg.
    add_executable(cutmachine_engine_log_tests tests/engine_log_tests.cc)
    target_link_libraries(cutmachine_engine_log_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_engine_log_tests
             COMMAND cutmachine_engine_log_tests)
    set_tests_properties(cutmachine_engine_log_tests
                         PROPERTIES LABELS "portable")
    # The script that reads the commit, run as the build runs it against a
    # repository the test makes: the commit, its dirty flag, "unknown"
    # outside a checkout, and no rewrite when nothing changed.
    if(GIT_FOUND)
        add_test(NAME cutmachine_build_info_script_tests
                 COMMAND "${CMAKE_COMMAND}"
                         "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cmake"
                         "-DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/src/BuildInfo.cc.in"
                         "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/build-info-script-tests"
                         "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
                         -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_info_script_tests.cmake")
        set_tests_properties(cutmachine_build_info_script_tests
                             PROPERTIES LABELS "portable")
    endif()

    add_executable(cutmachine_cli_tests tests/cli_tests.cc)
    # LG2 -- generate_captions, called in this process, measures with the
    # caption renderer (Core Text).
    target_link_libraries(cutmachine_cli_tests PRIVATE
                          cutmachine_motion_render)
    target_compile_definitions(cutmachine_cli_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
        CUTMACHINE_EXECUTABLE="$<TARGET_FILE:cutmachine>")
    # PRD19 -- the command line is read in main.mm: tested through the
    # binary, as the CLI writer lock already is from the MCP suite.
    add_dependencies(cutmachine_cli_tests cutmachine)
    add_test(NAME cutmachine_cli_tests COMMAND cutmachine_cli_tests)

    # Pure-C++ MCP tool-dispatch/HTTP/JSON-RPC test (ROADMAP.md F1.1/F1.2):
    # no ProjectStorage, so it also builds and runs on a plain Linux host --
    # see tests/mcp_tools_tests.cc for details.
    add_executable(cutmachine_mcp_tools_tests tests/mcp_tools_tests.cc)
    target_link_libraries(cutmachine_mcp_tools_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_mcp_tools_tests COMMAND cutmachine_mcp_tools_tests)

    # Integration test against the real project-file backend
    # (McpProjectBackend/ProjectStorage) -- macOS only, like cutmachine_cli_tests.
    add_executable(cutmachine_mcp_tests tests/mcp_tests.cc)
    # MOTION-2026-09 M3 -- render_motion_frame draws with the renderer the
    # executable installs; the test installs it the same way. The renderer
    # links cutmachine_model publicly: naming the model here as well put the
    # archive twice on the link line.
    target_link_libraries(cutmachine_mcp_tests PRIVATE
                          cutmachine_motion_render)
    # F2.4 (R10) -- the Resolve database the test writes
    # (resolve_database_fixture.h): SQLite's header only, the library comes
    # with the model, and naming it again put it twice on the link line.
    target_include_directories(cutmachine_mcp_tests SYSTEM PRIVATE
                               "${sqlite_BINARY_DIR}")
    target_compile_definitions(cutmachine_mcp_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
        CUTMACHINE_EXECUTABLE="$<TARGET_FILE:cutmachine>"
        MOTION_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts"
        # F2.4 (R10) -- the Resolve grade bodies, for the import from the
        # window with its database (DEC15).
        RESOLVE_GRADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/resolve_grade")
    # IA5 -- the CLI writer lock is taken in main.mm: tested through it.
    add_dependencies(cutmachine_mcp_tests cutmachine)
    add_test(NAME cutmachine_mcp_tests COMMAND cutmachine_mcp_tests)

    # AX1 -- docs/roadmap/AX-2026-09.md. The app's endpoint and the stdio
    # relay to it; the binary is run to prove `--mcp-serve --stdio` relays.
    add_executable(cutmachine_mcp_endpoint_tests tests/mcp_endpoint_tests.cc)
    target_link_libraries(cutmachine_mcp_endpoint_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_mcp_endpoint_tests PRIVATE
        CUTMACHINE_EXECUTABLE="$<TARGET_FILE:cutmachine>")
    add_dependencies(cutmachine_mcp_endpoint_tests cutmachine)
    add_test(NAME cutmachine_mcp_endpoint_tests
             COMMAND cutmachine_mcp_endpoint_tests)

    # PRD13 -- ROADMAP.md. Who the loopback HTTP server answers: Host,
    # Origin, content type and bearer token, decided on a request's header
    # fields alone. The rule includes no socket header and the suite links
    # nothing but it, so it runs on a plain Linux host: portable.
    add_executable(cutmachine_http_request_policy_tests
                   tests/http_request_policy_tests.cc
                   src/HttpRequestPolicy.cc)
    target_include_directories(cutmachine_http_request_policy_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_http_request_policy_tests
             COMMAND cutmachine_http_request_policy_tests)
    set_tests_properties(cutmachine_http_request_policy_tests PROPERTIES
        LABELS "portable")

    # PRD18 -- ROADMAP.md (ACP4). The procedures the binary carries: each
    # embedded file identical to the repository's, byte for byte, none left
    # out, and the reading of a name, a path and a link. The binary reads no
    # skill directory; only this test is told where the repository's is.
    # Plain C++ (Procedures.h): portable.
    add_executable(cutmachine_procedures_tests tests/procedures_tests.cc)
    target_link_libraries(cutmachine_procedures_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_procedures_tests PRIVATE
        CUTMACHINE_SKILLS_DIR="${CUTMACHINE_SKILLS_DIR}")
    add_test(NAME cutmachine_procedures_tests
             COMMAND cutmachine_procedures_tests)
    set_tests_properties(cutmachine_procedures_tests PROPERTIES
        LABELS "portable")

    add_executable(cutmachine_after_bridge_tests
        tests/after_bridge_tests.cc)
    target_link_libraries(cutmachine_after_bridge_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_after_bridge_tests PRIVATE
        AFTER_BRIDGE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/after")
    add_test(NAME cutmachine_after_bridge_tests
             COMMAND cutmachine_after_bridge_tests)
    # Pont After -- the bridge's JSX handlers under JavaScriptCore, against a
    # stand-in for After's object model (tests/after_jsx_tests.js).
    add_test(NAME cutmachine_after_jsx_tests
             COMMAND osascript -l JavaScript
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/after_jsx_tests.js
                     ${CMAKE_CURRENT_SOURCE_DIR}/tools/after-bridge/scripts)

    add_test(
        NAME cutmachine_architecture_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/architecture_tests.py
                ${CMAKE_CURRENT_SOURCE_DIR}
    )
    # HEADLESS-2026-10 H3 -- a script that reads the tree: portable, so the
    # build without a window and its CI job keep the guard too.
    set_tests_properties(cutmachine_architecture_tests
                         PROPERTIES LABELS "portable")

    # FI1/FI5 -- ROADMAP.md. Packing is dependency-free; browser checks use
    # an explicitly supplied local Playwright/GSAP installation and report
    # SKIP (77) when it is unavailable, never a false pass.
    add_test(NAME cutmachine_motion_web_pack_tests
             COMMAND ${Python3_EXECUTABLE} -I
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/motion_web_pack_tests.py)
    set_tests_properties(cutmachine_motion_web_pack_tests PROPERTIES
                         LABELS "portable")
    find_program(CUTMACHINE_NODE_EXECUTABLE NAMES node)
    if(CUTMACHINE_NODE_EXECUTABLE)
        add_test(NAME cutmachine_motion_web_compare_tests
                 COMMAND ${CUTMACHINE_NODE_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/motion_web_compare_tests.mjs)
        set_tests_properties(cutmachine_motion_web_compare_tests PROPERTIES
                             LABELS "portable")
        add_test(NAME cutmachine_motion_web_tests
                 COMMAND ${CUTMACHINE_NODE_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/motion_web_tests.mjs
                         $<TARGET_FILE:cutmachine>
                         ${FFMPEG_EXECUTABLE})
        set_tests_properties(cutmachine_motion_web_tests PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 120 LABELS "integration"
                             RESOURCE_LOCK "cutmachine_desktop")
        # SN1 -- docs/roadmap/SON-2026-10.md. The sound renderer is plain
        # JavaScript without a dependency: its suite runs wherever Node is.
        # SN9 -- the fetched Strudel, for the patterns written as partitions.
        add_test(NAME cutmachine_sound_tests
                 COMMAND ${CUTMACHINE_NODE_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/cutmachine-sound/test/all.mjs
                         ${strudel_SOURCE_DIR})
        set_tests_properties(cutmachine_sound_tests PROPERTIES
                             TIMEOUT 300 LABELS "portable")
        # SN2 -- the same edit read from projects the engine writes and from
        # the EDL it exports from them, at 25 and 23.976 frames per second.
        add_test(NAME cutmachine_sound_project_tests
                 COMMAND ${CUTMACHINE_NODE_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/sound_project_tests.mjs
                         $<TARGET_FILE:cutmachine>
                         ${FFMPEG_EXECUTABLE})
        set_tests_properties(cutmachine_sound_project_tests PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 300
                             LABELS "integration")
    endif()

    # WORD-ALIGNMENT-2026-09 -- the benchmark stays dependency-free so its
    # timebase and scoring contract are checked on every CI host, even though
    # the aligners and models it measures run in their own environments.
    add_test(
        NAME cutmachine_word_alignment_benchmark_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/word_alignment_benchmark_tests.py
    )

    # ITW9 -- docs/roadmap/ITW-2026-09.md. What mfa_align.py hands MFA for
    # a French, English or Spanish rush, which models it runs, and how MFA's
    # words come back onto the transcript (ALIGNEMENT-2026-11), with MFA and
    # FFmpeg replaced: the real ones only run on a machine that has them.
    add_test(
        NAME cutmachine_mfa_align_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/mfa_align_tests.py
    )
    set_tests_properties(cutmachine_mfa_align_tests
                         PROPERTIES LABELS "portable")

    # GN6a -- docs/roadmap/GENERATIF-2026-10.md. What voice_over.py writes
    # (a 24-bit WAV and its report), the model revision it reads, and its
    # refusals, with Qwen3-TTS replaced: the real model only runs on a
    # machine that has it.
    add_test(
        NAME cutmachine_voice_over_script_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/voice_over_script_tests.py
    )
    set_tests_properties(cutmachine_voice_over_script_tests
                         PROPERTIES LABELS "portable")

    # PA3 -- docs/roadmap/PARITE-2026-11.md. What diarize.py refuses before
    # loading anything and how it reads the stretches of speech; its
    # grouping where numpy is installed. The speaker embedding model only
    # runs on a machine that has it.
    add_test(
        NAME cutmachine_diarize_script_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/diarize_script_tests.py
    )
    set_tests_properties(cutmachine_diarize_script_tests
                         PROPERTIES LABELS "portable")

    # GR12 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The Resolve grade probe's
    # cube reader, candidate CDL formulas and sweep, against a fake Resolve:
    # the real one only runs with the author at the screen.
    add_test(
        NAME cutmachine_resolve_grade_probe_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/resolve_grade_probe_tests.py
    )
    set_tests_properties(cutmachine_resolve_grade_probe_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_keyframe_tests tests/keyframe_tests.cc)
    target_link_libraries(cutmachine_keyframe_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_keyframe_tests COMMAND cutmachine_keyframe_tests)

    # MOTION-2026-09 M1 -- ROADMAP.md. Composition parsing, bindings, springs
    # and flexbox layout under fake text metrics: plain C++, no AppKit. The
    # suite also executes the complete examples of docs/MOTION_FORMAT.md so
    # the format document cannot drift from the engine.
    add_executable(cutmachine_motion_tests tests/motion_tests.cc)
    target_link_libraries(cutmachine_motion_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_motion_tests PRIVATE
        MOTION_FORMAT_DOC="${CMAKE_CURRENT_SOURCE_DIR}/docs/MOTION_FORMAT.md")
    add_test(NAME cutmachine_motion_tests COMMAND cutmachine_motion_tests)
    set_tests_properties(cutmachine_motion_tests PROPERTIES LABELS "portable")

    # MOTION-AFTER-2026-09 -- the description After builds a project from,
    # read without After.
    # MA2 -- verify_after_export, its pictures made by tests/golden_image.cc.
    add_executable(cutmachine_motion_after_export_tests
        tests/motion_after_export_tests.cc
        tests/golden_image.cc)
    target_link_libraries(cutmachine_motion_after_export_tests PRIVATE
        cutmachine_model)
    add_test(NAME cutmachine_motion_after_export_tests
        COMMAND cutmachine_motion_after_export_tests)
    set_tests_properties(cutmachine_motion_after_export_tests PROPERTIES
        LABELS "portable")

    # REMOTION-PARITY-2026-09 REM1 -- ROADMAP.md. Ten independent scenes
    # freeze the shared authoring contract as exact, deterministic evaluated
    # frames. The fake metrics keep this gate portable and hermetic.
    # FC9 -- the effects are also compared, whole, with the references of
    # tests/fixtures/golden/ (tests/golden_image.h).
    add_executable(cutmachine_motion_parity_tests
                   tests/motion_parity_tests.cc
                   tests/golden_image.cc)
    target_link_libraries(cutmachine_motion_parity_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_motion_parity_tests PRIVATE
        MOTION_PARITY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/motion/parity"
        GOLDEN_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/golden"
        GOLDEN_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-output")
    add_test(NAME cutmachine_motion_parity_tests
             COMMAND cutmachine_motion_parity_tests)
    set_tests_properties(cutmachine_motion_parity_tests
                         PROPERTIES LABELS "portable")

    # FC9 -- docs/roadmap/FILMCRAFT-2026-10.md. The comparison of a render
    # with a reference PNG (PSNR, largest difference, 99th percentile, the
    # files a failure writes, CUTMACHINE_BLESS), tested on itself.
    add_executable(cutmachine_golden_image_tests
                   tests/golden_image_tests.cc
                   tests/golden_image.cc)
    target_link_libraries(cutmachine_golden_image_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_golden_image_tests PRIVATE
        GOLDEN_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden-output")
    add_test(NAME cutmachine_golden_image_tests
             COMMAND cutmachine_golden_image_tests)
    set_tests_properties(cutmachine_golden_image_tests
                         PROPERTIES LABELS "portable")

    # REMOTION-PARITY-2026-09 REM5 -- the visual editor's policy: what a
    # click selects and what a gesture commits. Plain C++, fake metrics.
    add_executable(cutmachine_motion_canvas_tests
                   tests/motion_canvas_tests.cc)
    target_link_libraries(cutmachine_motion_canvas_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_motion_canvas_tests PRIVATE
        MOTION_PARITY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/motion/parity")
    add_test(NAME cutmachine_motion_canvas_tests
             COMMAND cutmachine_motion_canvas_tests)
    set_tests_properties(cutmachine_motion_canvas_tests
                         PROPERTIES LABELS "portable")

    # MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. The humanize pass's
    # rules on fake metrics; the proof by rendering runs in the executable.
    add_executable(cutmachine_motion_humanize_tests
                   tests/motion_humanize_tests.cc)
    target_link_libraries(cutmachine_motion_humanize_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_motion_humanize_tests
             COMMAND cutmachine_motion_humanize_tests)
    set_tests_properties(cutmachine_motion_humanize_tests
                         PROPERTIES LABELS "portable")

    # MOTION-2026-09 M3 -- ROADMAP.md. Compositions in the document: the
    # motion operations, their identities, refusals, journal and version.
    # Plain C++, no text measured.
    add_executable(cutmachine_motion_document_tests
                   tests/motion_document_tests.cc)
    target_link_libraries(cutmachine_motion_document_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_motion_document_tests
             COMMAND cutmachine_motion_document_tests)
    set_tests_properties(cutmachine_motion_document_tests
                         PROPERTIES LABELS "portable")

    # MOTION-2026-09 M4 -- ROADMAP.md. Motion clips on the timeline: schema,
    # version, operations, refusals and the frame Timeline resolves. Plain
    # C++, no text measured.
    # MOTION-2026-09 M5 -- the templates, the placement by anchor and the
    # safe-area check (MotionTemplates.h, MotionPlacement.h, MotionSafeArea.h).
    add_executable(cutmachine_motion_templates_tests
                   tests/motion_templates_tests.cc)
    target_link_libraries(cutmachine_motion_templates_tests PRIVATE
                          cutmachine_model cutmachine_sqlite)
    add_test(NAME cutmachine_motion_templates_tests
             COMMAND cutmachine_motion_templates_tests)
    set_tests_properties(cutmachine_motion_templates_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_motion_clip_tests tests/motion_clip_tests.cc)
    target_link_libraries(cutmachine_motion_clip_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_motion_clip_tests
             COMMAND cutmachine_motion_clip_tests)
    set_tests_properties(cutmachine_motion_clip_tests
                         PROPERTIES LABELS "portable")

    # MOTION-2026-09 M4 -- ROADMAP.md. A motion clip exported over a shot and
    # read back at three instants, with and without colour management: the
    # renderer draws the frames, FFmpeg encodes and decodes them.
    add_executable(cutmachine_motion_clip_export_tests
                   tests/motion_clip_export_tests.cc)
    target_link_libraries(cutmachine_motion_clip_export_tests PRIVATE
                          cutmachine_motion_render)
    target_compile_definitions(cutmachine_motion_clip_export_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
        CUTMACHINE_EXECUTABLE="$<TARGET_FILE:cutmachine>"
        MOTION_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts")
    # The CLI half runs the executable, which installs the renderer.
    add_dependencies(cutmachine_motion_clip_export_tests cutmachine)
    add_test(NAME cutmachine_motion_clip_export_tests
             COMMAND cutmachine_motion_clip_export_tests)

    # MOTION-2026-09 M2 -- ROADMAP.md. Core Text and Core Graphics: macOS only.
    # Fonts come from tests/fixtures/fonts (OFL), registered for the process,
    # so the result never depends on what is installed on the machine.
    # LG1 -- a carton drawn by Core Text: fill, outline, shadow, box and
    # opacity where the layout says, and the fallback for a missing family.
    add_executable(cutmachine_caption_render_tests
                   tests/caption_render_tests.cc)
    target_link_libraries(cutmachine_caption_render_tests PRIVATE
                          cutmachine_motion_render)
    # MOTION-WEB-2026-10 MW7 -- a variable font from google/fonts (Outfit,
    # OFL), in a directory of its own: only the suites that test the weight
    # of a variable font register it.
    target_compile_definitions(cutmachine_caption_render_tests PRIVATE
        MOTION_VARIABLE_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts/outfit")
    add_test(NAME cutmachine_caption_render_tests
             COMMAND cutmachine_caption_render_tests)

    add_executable(cutmachine_motion_render_tests tests/motion_render_tests.cc)
    target_link_libraries(cutmachine_motion_render_tests PRIVATE
                          cutmachine_motion_render)
    target_compile_definitions(cutmachine_motion_render_tests PRIVATE
        MOTION_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts"
        MOTION_VARIABLE_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts/outfit"
        MOTION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/motion"
        # M6 -- rendered videos are read back and probed.
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_motion_render_tests
             COMMAND cutmachine_motion_render_tests)

    # REMOTION-PARITY-2026-09 REM2 -- media and precompositions with real
    # files made by FFmpeg and the real painter (C12, C13, C14).
    add_executable(cutmachine_motion_media_tests tests/motion_media_tests.cc)
    target_link_libraries(cutmachine_motion_media_tests PRIVATE
                          cutmachine_motion_render)
    target_compile_definitions(cutmachine_motion_media_tests PRIVATE
        MOTION_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts"
        MOTION_PARITY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/motion/parity"
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_motion_media_tests
             COMMAND cutmachine_motion_media_tests)

    # REM3 -- paint, masks, effects and transitions drawn by the real
    # painter (C15-C18), and the italic face.
    add_executable(cutmachine_motion_paint_tests tests/motion_paint_tests.cc)
    # FI4 -- a private family containing only an upright face keeps the
    # missing-italic oracle independent of fonts installed on this Mac.
    set(_upright_font_dir "${CMAKE_CURRENT_BINARY_DIR}/test-fonts/upright")
    set(_upright_font "${_upright_font_dir}/CUTMACHINETestUpright.ttf")
    configure_file(tests/fixtures/fonts/OFL.txt "${_upright_font_dir}/OFL.txt"
                   COPYONLY NO_SOURCE_PERMISSIONS)
    add_custom_command(OUTPUT "${_upright_font}"
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/make_font_fixture.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts/FiraSansCondensed-SemiBold.ttf"
                "${_upright_font}"
        DEPENDS tests/make_font_fixture.py
                tests/fixtures/fonts/FiraSansCondensed-SemiBold.ttf
        VERBATIM)
    add_custom_target(cutmachine_upright_test_font DEPENDS "${_upright_font}")
    add_dependencies(cutmachine_motion_paint_tests cutmachine_upright_test_font)
    target_link_libraries(cutmachine_motion_paint_tests PRIVATE
                          cutmachine_motion_render)
    target_compile_definitions(cutmachine_motion_paint_tests PRIVATE
        MOTION_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fonts"
        MOTION_UPRIGHT_FONT_DIR="${_upright_font_dir}"
        MOTION_PARITY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/motion/parity")
    add_test(NAME cutmachine_motion_paint_tests
             COMMAND cutmachine_motion_paint_tests)

    add_executable(cutmachine_color_effects_tests tests/color_effects_tests.cc)
    target_link_libraries(cutmachine_color_effects_tests PRIVATE cutmachine_model)
    # PRD12 -- the eight scalar stages' cube, sampled, within a tolerance.
    # GR16 -- Resolve 21.1's own cubes, for color.resolve_primaries.
    target_compile_definitions(cutmachine_color_effects_tests PRIVATE
        COLOR_GRADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/color_grade"
        RESOLVE_PRIMARIES_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/resolve_primaries")
    add_test(NAME cutmachine_color_effects_tests
             COMMAND cutmachine_color_effects_tests)

    # F2.2 -- Inspector (ROADMAP.md). InspectorGradeControls.h is plain
    # C++, no AppKit dependency -- like UiTheme.h/PanelLayout.h above -- so
    # this builds and runs on a plain Linux host too.
    add_executable(cutmachine_inspector_grade_controls_tests
                   tests/inspector_grade_controls_tests.cc)
    target_link_libraries(cutmachine_inspector_grade_controls_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_inspector_grade_controls_tests
             COMMAND cutmachine_inspector_grade_controls_tests)

    # GR3 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The colour wheels and
    # the curve editor as policy (GradeWheelModel.h, GradeCurveModel.h):
    # plain C++, every gesture checked on the stack it gives.
    add_executable(cutmachine_grade_control_models_tests
                   tests/grade_control_models_tests.cc)
    target_link_libraries(cutmachine_grade_control_models_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_grade_control_models_tests
             COMMAND cutmachine_grade_control_models_tests)
    set_tests_properties(cutmachine_grade_control_models_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_project_recovery_tests
                   tests/project_recovery_tests.cc)
    target_link_libraries(cutmachine_project_recovery_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_recovery_tests
             COMMAND cutmachine_project_recovery_tests)

    add_executable(cutmachine_project_tests tests/project_tests.cc)
    target_link_libraries(cutmachine_project_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_tests COMMAND cutmachine_project_tests)

    add_executable(cutmachine_interview_short_tests
                   tests/interview_short_tests.cc)
    target_link_libraries(cutmachine_interview_short_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_interview_short_tests
             COMMAND cutmachine_interview_short_tests)

    add_executable(cutmachine_project_storage_tests
                   tests/project_storage_tests.cc)
    target_link_libraries(cutmachine_project_storage_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_storage_tests
             COMMAND cutmachine_project_storage_tests)

    add_executable(cutmachine_sequence_format_tests
        tests/sequence_format_tests.cc)
    target_link_libraries(cutmachine_sequence_format_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_sequence_format_tests
             COMMAND cutmachine_sequence_format_tests)

    add_executable(cutmachine_project_bin_tests tests/project_bin_tests.cc)
    target_link_libraries(cutmachine_project_bin_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_bin_tests COMMAND cutmachine_project_bin_tests)

    add_executable(cutmachine_export_tests tests/export_tests.cc)
    # LG3 -- captions are drawn by the caption renderer (Core Text).
    target_link_libraries(cutmachine_export_tests PRIVATE
                          cutmachine_motion_render)
    target_compile_definitions(cutmachine_export_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    add_test(NAME cutmachine_export_tests COMMAND cutmachine_export_tests)

    # IA4 -- docs/roadmap/IA-2026-09.md. An inspection rendered through the
    # export's graph: levels and the strip. Its spoken-interview part reads
    # real speech with the local Whisper model when CUTMACHINE_TEST_WHISPER
    # is set, and says it skipped otherwise.
    add_executable(cutmachine_cut_inspection_render_tests
                   tests/cut_inspection_render_tests.cc)
    target_link_libraries(cutmachine_cut_inspection_render_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_cut_inspection_render_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_cut_inspection_render_tests
             COMMAND cutmachine_cut_inspection_render_tests)

    # RC1, RC2 -- docs/roadmap/RACCORDS-2026-10.md. A review of the
    # junctions rendered for real, on rushes FFmpeg's lavfi sources make:
    # an ambience that jumps, a tone cut live, a jump cut, a through edit,
    # and the bands drawn and stacked.
    add_executable(cutmachine_cut_review_render_tests
                   tests/cut_review_render_tests.cc)
    target_link_libraries(cutmachine_cut_review_render_tests PRIVATE
                          cutmachine_model)
    target_compile_definitions(cutmachine_cut_review_render_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_cut_review_render_tests
             COMMAND cutmachine_cut_review_render_tests)

    add_executable(cutmachine_ingest_tests
        tests/ingest_tests.cc
        src/MediaSource.mm
    )
    target_link_libraries(cutmachine_ingest_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_ingest_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_ingest_tests COMMAND cutmachine_ingest_tests)

    # PRD7 -- docs/roadmap/PRODUIT-2026-10.md. What an ingest does to a
    # document once its files are read: the half the window and --ingest
    # share. The probed entries are built by the test, so no media is
    # decoded and no FFmpeg binary runs. Portable: no AppKit, no Metal.
    add_executable(cutmachine_ingest_document_tests
                   tests/ingest_document_tests.cc)
    target_link_libraries(cutmachine_ingest_document_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_ingest_document_tests
             COMMAND cutmachine_ingest_document_tests)
    set_tests_properties(cutmachine_ingest_document_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_resolve_export_tests
        tests/resolve_export_tests.cc)
    target_link_libraries(cutmachine_resolve_export_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_resolve_export_tests
             COMMAND cutmachine_resolve_export_tests)

    # R9 -- docs/roadmap/REEL-2026-09.md. The FCP7 XML writer is plain C++
    # with no AppKit, no FFmpeg and no Resolve, so it runs on a Linux host.
    add_executable(cutmachine_resolve_xml_export_tests
        tests/resolve_xml_export_tests.cc)
    target_link_libraries(cutmachine_resolve_xml_export_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_resolve_xml_export_tests
             COMMAND cutmachine_resolve_xml_export_tests)

    # DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Resolve's grades
    # read from a copy of a project database: bodies, database and match,
    # on fixtures written without a client's data.
    add_executable(cutmachine_resolve_grade_tests tests/resolve_grade_tests.cc)
    target_link_libraries(cutmachine_resolve_grade_tests
                          PRIVATE cutmachine_model cutmachine_sqlite)
    target_compile_definitions(cutmachine_resolve_grade_tests PRIVATE
        RESOLVE_GRADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/resolve_grade")
    add_test(NAME cutmachine_resolve_grade_tests
             COMMAND cutmachine_resolve_grade_tests)
    set_tests_properties(cutmachine_resolve_grade_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_resolve_import_tests
        tests/resolve_import_tests.cc
        src/MediaSource.mm
    )
    target_link_libraries(cutmachine_resolve_import_tests
                          PRIVATE cutmachine_model cutmachine_sqlite)
    target_compile_definitions(cutmachine_resolve_import_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
        RESOLVE_GRADE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/resolve_grade")
    add_test(NAME cutmachine_resolve_import_tests
             COMMAND cutmachine_resolve_import_tests)

    add_executable(cutmachine_media_task_manager_tests
        tests/media_task_manager_tests.cc
    )
    target_link_libraries(cutmachine_media_task_manager_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_media_task_manager_tests
             COMMAND cutmachine_media_task_manager_tests)

    add_executable(cutmachine_proxy_tests tests/proxy_tests.cc)
    target_link_libraries(cutmachine_proxy_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_proxy_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_proxy_tests COMMAND cutmachine_proxy_tests)

    add_executable(cutmachine_waveform_tests tests/waveform_tests.cc)
    target_link_libraries(cutmachine_waveform_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_waveform_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_waveform_tests COMMAND cutmachine_waveform_tests)

    add_executable(cutmachine_transcription_tests tests/transcription_tests.cc)
    target_link_libraries(cutmachine_transcription_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_transcription_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_transcription_tests
             COMMAND cutmachine_transcription_tests)

    add_executable(cutmachine_hesitation_tests tests/hesitation_tests.cc)
    target_link_libraries(cutmachine_hesitation_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_hesitation_tests
             COMMAND cutmachine_hesitation_tests)

    add_executable(cutmachine_timeline_audio_report_tests
        tests/timeline_audio_report_tests.cc)
    target_link_libraries(cutmachine_timeline_audio_report_tests
        PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_timeline_audio_report_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_timeline_audio_report_tests
             COMMAND cutmachine_timeline_audio_report_tests)

    add_executable(cutmachine_caption_generation_tests
        tests/caption_generation_tests.cc)
    # LG2 -- cartons are measured by the caption renderer (Core Text).
    target_link_libraries(cutmachine_caption_generation_tests
        PRIVATE cutmachine_motion_render)
    target_compile_definitions(cutmachine_caption_generation_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_caption_generation_tests
             COMMAND cutmachine_caption_generation_tests)

    add_executable(cutmachine_word_alignment_tests
        tests/word_alignment_tests.cc)
    target_link_libraries(cutmachine_word_alignment_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_word_alignment_tests
             COMMAND cutmachine_word_alignment_tests)

    add_executable(cutmachine_frame_capture_tests tests/frame_capture_tests.cc)
    target_link_libraries(cutmachine_frame_capture_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_frame_capture_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_frame_capture_tests
             COMMAND cutmachine_frame_capture_tests)

    # R8 -- docs/roadmap/REEL-2026-09.md. Isolated frames: the detection
    # rule and the range planning are plain C++ with no FFmpeg and no
    # AppKit, so they run on a plain Linux host and under the sanitizers.
    # The decode that feeds them is exercised separately, below.
    add_executable(cutmachine_isolated_frames_tests
                   tests/isolated_frames_tests.cc)
    target_link_libraries(cutmachine_isolated_frames_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_isolated_frames_tests
             COMMAND cutmachine_isolated_frames_tests)
    set_tests_properties(cutmachine_isolated_frames_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_isolated_frames_media_tests
                   tests/isolated_frames_media_tests.cc)
    target_link_libraries(cutmachine_isolated_frames_media_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_isolated_frames_media_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_isolated_frames_media_tests
             COMMAND cutmachine_isolated_frames_media_tests)

    add_executable(cutmachine_shot_quality_tests tests/shot_quality_tests.cc)
    target_link_libraries(cutmachine_shot_quality_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_shot_quality_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_shot_quality_tests
             COMMAND cutmachine_shot_quality_tests)

    add_executable(cutmachine_document_delta_tests
        tests/document_delta_tests.cc)
    target_link_libraries(cutmachine_document_delta_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_document_delta_tests
             COMMAND cutmachine_document_delta_tests)

    add_executable(cutmachine_transcript_alignment_tests
        tests/transcript_alignment_tests.cc)
    target_link_libraries(cutmachine_transcript_alignment_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_transcript_alignment_tests
             COMMAND cutmachine_transcript_alignment_tests)

    add_executable(cutmachine_source_address_tests
        tests/source_address_tests.cc)
    target_link_libraries(cutmachine_source_address_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_source_address_tests
             COMMAND cutmachine_source_address_tests)

    add_executable(cutmachine_pause_tightening_tests
        tests/pause_tightening_tests.cc)
    target_link_libraries(cutmachine_pause_tightening_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_pause_tightening_tests
             COMMAND cutmachine_pause_tightening_tests)

    add_executable(cutmachine_cut_words_tests tests/cut_words_tests.cc)
    target_link_libraries(cutmachine_cut_words_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_cut_words_tests
             COMMAND cutmachine_cut_words_tests)

    add_executable(cutmachine_boundary_air_tests
        tests/boundary_air_tests.cc)
    target_link_libraries(cutmachine_boundary_air_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_boundary_air_tests
             COMMAND cutmachine_boundary_air_tests)

    add_executable(cutmachine_speech_onset_tests tests/speech_onset_tests.cc)
    target_link_libraries(cutmachine_speech_onset_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_speech_onset_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_speech_onset_tests
             COMMAND cutmachine_speech_onset_tests)

    add_executable(cutmachine_subtitle_tests tests/subtitle_tests.cc)
    target_link_libraries(cutmachine_subtitle_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_subtitle_tests COMMAND cutmachine_subtitle_tests)

    # FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. The CMX 3600 writer and the
    # marker reader against lists written by hand (tests/fixtures/edl).
    # Portable: no AppKit, no FFmpeg, no Resolve.
    add_executable(cutmachine_edl_tests tests/edl_tests.cc)
    target_link_libraries(cutmachine_edl_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_edl_tests PRIVATE
        EDL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/edl")
    add_test(NAME cutmachine_edl_tests COMMAND cutmachine_edl_tests)
    set_tests_properties(cutmachine_edl_tests PROPERTIES LABELS "portable")

    # PA1 -- docs/roadmap/PARITE-2026-11.md. The OTIO and FCPXML writers
    # against what each format asks for, spelled by hand in the test.
    # Portable: no AppKit, no FFmpeg, no other editor.
    add_executable(cutmachine_interchange_tests tests/interchange_tests.cc)
    target_link_libraries(cutmachine_interchange_tests
        PRIVATE cutmachine_model)
    add_test(NAME cutmachine_interchange_tests
             COMMAND cutmachine_interchange_tests)
    set_tests_properties(cutmachine_interchange_tests
        PROPERTIES LABELS "portable")

    add_executable(cutmachine_thumbnail_tests tests/thumbnail_tests.cc)
    target_link_libraries(cutmachine_thumbnail_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_thumbnail_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_thumbnail_tests COMMAND cutmachine_thumbnail_tests)

    add_executable(cutmachine_fft_tests tests/fft_tests.cc)
    target_include_directories(cutmachine_fft_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_fft_tests COMMAND cutmachine_fft_tests)

    # F2.1 -- design system (ROADMAP.md). UiTheme.h and PanelLayout.h are
    # plain C++, no AppKit dependency, so -- like Fft.h above -- these build
    # and run on a plain Linux host.
    add_executable(cutmachine_ui_theme_tests tests/ui_theme_tests.cc)
    target_include_directories(cutmachine_ui_theme_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_ui_theme_tests COMMAND cutmachine_ui_theme_tests)

    # PERF-2026-09 -- the Record monitor's hold rule. PlaybackPresentation.h
    # is plain C++ with no AppKit dependency, like UiTheme.h/PanelLayout.h
    # around it, so this runs on a plain Linux host and under the sanitizers.
    add_executable(cutmachine_playback_presentation_tests
                   tests/playback_presentation_tests.cc)
    target_include_directories(cutmachine_playback_presentation_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_playback_presentation_tests
             COMMAND cutmachine_playback_presentation_tests)

    # PERF10 -- docs/roadmap/PERF-2026-09.md. Which decoders the editor keeps
    # open: DecoderPool.h is plain C++ with no AppKit or FFmpeg dependency,
    # main.mm only wires it, so this runs on a plain Linux host. PERF12 --
    # which ones it opens ahead of playback, read from a document: linked to
    # the model library like the other portable suites, still no AppKit.
    add_executable(cutmachine_decoder_pool_tests tests/decoder_pool_tests.cc)
    target_include_directories(cutmachine_decoder_pool_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cutmachine_decoder_pool_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_decoder_pool_tests
             COMMAND cutmachine_decoder_pool_tests)
    set_tests_properties(cutmachine_decoder_pool_tests
                         PROPERTIES LABELS "portable")

    # PERF13 -- which layers of Record an opaque shot above hides, so that
    # playback neither decodes nor asks for them: MonitorOcclusion.h is plain
    # C++ read from a document, main.mm only wires it.
    add_executable(cutmachine_monitor_occlusion_tests
                   tests/monitor_occlusion_tests.cc)
    target_include_directories(cutmachine_monitor_occlusion_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cutmachine_monitor_occlusion_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_monitor_occlusion_tests
             COMMAND cutmachine_monitor_occlusion_tests)
    set_tests_properties(cutmachine_monitor_occlusion_tests
                         PROPERTIES LABELS "portable")

    # PERF14 -- what playback reads ahead of the picture it shows: a shot up
    # to its last frame and no further, the decoded budget shared among the
    # sources read, the bytes of the next seconds asked for in pieces.
    # ReadAhead.h and PlaybackReads.h are plain C++, main.mm and
    # DecodeWorker.cc only wire them.
    add_executable(cutmachine_read_ahead_tests
        tests/read_ahead_tests.cc
        src/ReadAhead.cc
    )
    target_include_directories(cutmachine_read_ahead_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cutmachine_read_ahead_tests PRIVATE
                          cutmachine_model)
    add_test(NAME cutmachine_read_ahead_tests
             COMMAND cutmachine_read_ahead_tests)
    set_tests_properties(cutmachine_read_ahead_tests
                         PROPERTIES LABELS "portable")

    # PERF10 -- the worker's read-ahead at rest and in playback, against a
    # real decode of a generated clip.
    add_executable(cutmachine_decode_worker_tests
        tests/decode_worker_tests.cc
        src/DecodeWorker.cc
        src/FrameCache.cc
        src/ReadAhead.cc
        src/PerformanceMetrics.cc
        src/MediaSource.mm
        src/Ulid.cc
    )
    target_include_directories(cutmachine_decode_worker_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cutmachine_decode_worker_tests PRIVATE
                          PkgConfig::FFMPEG)
    target_compile_definitions(cutmachine_decode_worker_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_decode_worker_tests
             COMMAND cutmachine_decode_worker_tests)

    # PERF10 -- a cached picture owns its bytes. FrameCache.cc needs only
    # libavutil, so this runs on a plain Linux host too.
    add_executable(cutmachine_frame_cache_tests
        tests/frame_cache_tests.cc
        src/FrameCache.cc
        src/ReadAhead.cc
    )
    target_include_directories(cutmachine_frame_cache_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(cutmachine_frame_cache_tests PRIVATE
                          PkgConfig::FFMPEG)
    add_test(NAME cutmachine_frame_cache_tests
             COMMAND cutmachine_frame_cache_tests)
    set_tests_properties(cutmachine_frame_cache_tests
                         PROPERTIES LABELS "portable")

    # PERF13 -- the monitor's sound reads a rush's sound and not its
    # pictures, and decodes the same samples. AudioDecode.cc needs only
    # FFmpeg, so this runs on a plain Linux host too.
    add_executable(cutmachine_audio_decode_tests
        tests/audio_decode_tests.cc
        src/AudioDecode.cc
    )
    target_include_directories(cutmachine_audio_decode_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(cutmachine_audio_decode_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    target_link_libraries(cutmachine_audio_decode_tests PRIVATE
                          PkgConfig::FFMPEG)
    add_test(NAME cutmachine_audio_decode_tests
             COMMAND cutmachine_audio_decode_tests)
    set_tests_properties(cutmachine_audio_decode_tests
                         PROPERTIES LABELS "portable")

    # PERF7 -- ROADMAP.md. The timeline render cost report: the policy
    # (RenderCostMeter.h) is plain C++ with no AppKit dependency, main.mm
    # only times the two calls it measures, so this runs on a plain Linux
    # host and under the sanitizers.
    add_executable(cutmachine_render_cost_meter_tests
                   tests/render_cost_meter_tests.cc)
    target_include_directories(cutmachine_render_cost_meter_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_render_cost_meter_tests
             COMMAND cutmachine_render_cost_meter_tests)
    set_tests_properties(cutmachine_render_cost_meter_tests PROPERTIES
        LABELS "portable")

    add_executable(cutmachine_panel_layout_tests tests/panel_layout_tests.cc)
    target_include_directories(cutmachine_panel_layout_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_panel_layout_tests
             COMMAND cutmachine_panel_layout_tests)

    # F2.5 -- playback transport (ROADMAP.md). TransportBar.h and Timecode.h
    # are plain C++, no AppKit dependency, so -- like UiTheme.h/PanelLayout.h
    # above -- these build and run on a plain Linux host.
    add_executable(cutmachine_transport_bar_tests
                   tests/transport_bar_tests.cc)
    target_include_directories(cutmachine_transport_bar_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_transport_bar_tests
             COMMAND cutmachine_transport_bar_tests)

    # Visual QA of 19 September 2026 -- PlayheadFrameRate reads a Document,
    # whose members draw a ULID: the test links Ulid.cc, still without
    # AppKit or the model library.
    add_executable(cutmachine_timecode_tests tests/timecode_tests.cc
                   src/Ulid.cc)
    target_include_directories(cutmachine_timecode_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_timecode_tests COMMAND cutmachine_timecode_tests)

    # PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. What VoiceOver reads of the
    # Metal views (UiAccessibility.h): their names, and the timeline's value,
    # the playhead's timecode and the selected shots. Portable: header-only,
    # main.mm only hands it to AppKit.
    add_executable(cutmachine_ui_accessibility_tests
                   tests/ui_accessibility_tests.cc src/Ulid.cc)
    target_include_directories(cutmachine_ui_accessibility_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_ui_accessibility_tests
             COMMAND cutmachine_ui_accessibility_tests)
    set_tests_properties(cutmachine_ui_accessibility_tests
                         PROPERTIES LABELS "portable")

    # F2.3 -- Media panel view-model (ROADMAP.md). MediaPanelModel.h depends
    # on Document.h/Operations.h (GenerateUlid needs Ulid.cc), so unlike
    # UiTheme.h/PanelLayout.h above it links cutmachine_model, but it is
    # still plain C++ with no AppKit dependency and runs on a plain Linux
    # host the same way cutmachine_project_bin_tests does.
    add_executable(cutmachine_media_panel_model_tests
                   tests/media_panel_model_tests.cc)
    target_link_libraries(cutmachine_media_panel_model_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_media_panel_model_tests
             COMMAND cutmachine_media_panel_model_tests)

    add_executable(cutmachine_beat_detection_tests
                   tests/beat_detection_tests.cc)
    # The beat anchor of place_motion_clip checks the title's layout with
    # the motion renderer, as mcp_tests does; the renderer links the model.
    target_link_libraries(cutmachine_beat_detection_tests
                          PRIVATE cutmachine_motion_render)
    target_compile_definitions(cutmachine_beat_detection_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}")
    add_test(NAME cutmachine_beat_detection_tests
             COMMAND cutmachine_beat_detection_tests)

    # R7 -- docs/roadmap/REEL-2026-09.md. The tempo grid is derived from the
    # onsets BeatDetection already cached, so this suite decodes nothing and
    # needs neither FFmpeg nor AppKit: it builds its onset lists itself and
    # runs on a plain Linux host, like the other portable suites.
    add_executable(cutmachine_tempo_grid_tests tests/tempo_grid_tests.cc
                   src/TempoGrid.cc src/Ulid.cc)
    target_include_directories(cutmachine_tempo_grid_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_tempo_grid_tests
             COMMAND cutmachine_tempo_grid_tests)
    set_tests_properties(cutmachine_tempo_grid_tests PROPERTIES
        LABELS "portable")

    # Where a track's energy moves, beat by beat (MusicStructure.h): the
    # grid and the envelope are built by the test, so it needs neither
    # FFmpeg nor AppKit and runs on a plain Linux host.
    add_executable(cutmachine_music_structure_tests
                   tests/music_structure_tests.cc src/MusicStructure.cc)
    target_include_directories(cutmachine_music_structure_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_music_structure_tests
             COMMAND cutmachine_music_structure_tests)
    set_tests_properties(cutmachine_music_structure_tests PROPERTIES
        LABELS "portable")

    add_executable(cutmachine_relink_tests tests/relink_tests.cc)
    target_link_libraries(cutmachine_relink_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_relink_tests COMMAND cutmachine_relink_tests)

    # F2.4 -- chat panel (ROADMAP.md). Both pure C++ (Json/McpTools/
    # ChatLlmClient/ChatSession only), no AppKit -- see each file's own
    # header comment for what they prove, in particular that a chat-driven
    # tool call dispatches through the real McpToolRegistry::Call and
    # EditLog::Apply, not a parallel mutation path.
    add_executable(cutmachine_chat_llm_client_tests
                   tests/chat_llm_client_tests.cc)
    target_link_libraries(cutmachine_chat_llm_client_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_chat_llm_client_tests
             COMMAND cutmachine_chat_llm_client_tests)

    add_executable(cutmachine_chat_session_tests tests/chat_session_tests.cc)
    target_link_libraries(cutmachine_chat_session_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_chat_session_tests
             COMMAND cutmachine_chat_session_tests)

    # ACP8 -- the machine report cannot hide a human warning or disappear
    # when its fence is malformed. The policy has no AppKit dependency.
    add_executable(cutmachine_chat_presentation_tests
                   tests/chat_presentation_tests.cc)
    target_link_libraries(cutmachine_chat_presentation_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_chat_presentation_tests
             COMMAND cutmachine_chat_presentation_tests)
    set_tests_properties(cutmachine_chat_presentation_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_chat_message_view_tests
                   tests/chat_message_view_tests.mm
                   src/ChatMessageView.mm src/UiThemeAppKit.mm
                   src/UiComponents.mm)
    target_link_libraries(cutmachine_chat_message_view_tests
                          PRIVATE cutmachine_model
                          "-framework AppKit" "-framework QuartzCore")
    target_compile_options(cutmachine_chat_message_view_tests PRIVATE
                           $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    add_test(NAME cutmachine_chat_message_view_tests
             COMMAND cutmachine_chat_message_view_tests)

    # ACP1, ACP2 -- docs/roadmap/ACP-2026-09.md. The Agent panel's client of
    # an external coding agent, against a scripted agent, and its process
    # transport on /bin/sh. Portable: no AppKit, no model, no network.
    # GENERATIF-2026-10 -- paid generation against a fake fal.ai: the
    # canvas, the queue, the key kept out of every message, proposals spent
    # once, and propose_generation then generate_media on a real project.
    # Portable: no network, no AppKit.
    add_executable(cutmachine_generation_tests tests/generation_tests.cc)
    target_link_libraries(cutmachine_generation_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_generation_tests
             COMMAND cutmachine_generation_tests)
    set_tests_properties(cutmachine_generation_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_acp_client_tests tests/acp_client_tests.cc)
    target_link_libraries(cutmachine_acp_client_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_acp_client_tests
             COMMAND cutmachine_acp_client_tests)
    set_tests_properties(cutmachine_acp_client_tests
                         PROPERTIES LABELS "portable")

    # PRD22 -- ROADMAP.md. The Agent panel's "Moteur IA" dialog as policy
    # (AiEngineForm.h): the fields each protocol uses, their placeholders,
    # what the dialog opens with and the pinned ACP adapters. Portable: plain
    # C++, the AppKit form only applies it.
    add_executable(cutmachine_ai_engine_form_tests
                   tests/ai_engine_form_tests.cc)
    target_link_libraries(cutmachine_ai_engine_form_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_ai_engine_form_tests
             COMMAND cutmachine_ai_engine_form_tests)
    set_tests_properties(cutmachine_ai_engine_form_tests
                         PROPERTIES LABELS "portable")

    # PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The window's form panel as
    # policy (FormPanelLayout.h): the keys and roles of its buttons, its
    # frames, how a refusal grows it, its colours. Portable: header-only, no
    # AppKit.
    add_executable(cutmachine_form_panel_layout_tests
                   tests/form_panel_layout_tests.cc)
    target_include_directories(cutmachine_form_panel_layout_tests PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_form_panel_layout_tests
             COMMAND cutmachine_form_panel_layout_tests)
    set_tests_properties(cutmachine_form_panel_layout_tests
                         PROPERTIES LABELS "portable")

    # The window's keyboard shortcuts as policy (KeyboardShortcuts.h): the
    # defaults, held to DaVinci Resolve's keys, and how a combination reads
    # and shows. Portable: header-only, no AppKit.
    add_executable(cutmachine_keyboard_shortcuts_tests
                   tests/keyboard_shortcuts_tests.cc)
    target_include_directories(cutmachine_keyboard_shortcuts_tests PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_keyboard_shortcuts_tests
             COMMAND cutmachine_keyboard_shortcuts_tests)
    set_tests_properties(cutmachine_keyboard_shortcuts_tests
                         PROPERTIES LABELS "portable")

    # FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode (TrimSession.h):
    # edit point selection, nudges, extend to the playhead, the J/K/L dynamic
    # trim on an injected clock, the Trim Monitor, each through the existing
    # trim operations and EditLog::Apply. Portable: no AppKit, no FFmpeg.
    add_executable(cutmachine_trim_session_tests tests/trim_session_tests.cc)
    target_link_libraries(cutmachine_trim_session_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_trim_session_tests
             COMMAND cutmachine_trim_session_tests)
    set_tests_properties(cutmachine_trim_session_tests
                         PROPERTIES LABELS "portable")

    # PRD1 -- docs/roadmap/PRODUIT-2026-10.md. The one launcher of other
    # programs, on /bin/sh, and the one resolution of the FFmpeg binary, on
    # shell scripts named ffmpeg. Portable: no AppKit, no FFmpeg, no network.
    add_executable(cutmachine_process_launcher_tests
                   tests/process_launcher_tests.cc)
    target_link_libraries(cutmachine_process_launcher_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_process_launcher_tests
             COMMAND cutmachine_process_launcher_tests)
    set_tests_properties(cutmachine_process_launcher_tests
                         PROPERTIES LABELS "portable")

    # GN6a -- docs/roadmap/GENERATIF-2026-10.md. Where the local voice-over's
    # Python, script and models are looked for, what the engine hands the
    # script, and a run against a shell standing in for Python: the report
    # read back, a failure's error output kept, cancellation. Portable: no
    # AppKit, no FFmpeg, no model.
    add_executable(cutmachine_voice_over_tests tests/voice_over_tests.cc)
    target_link_libraries(cutmachine_voice_over_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_voice_over_tests
             COMMAND cutmachine_voice_over_tests)
    set_tests_properties(cutmachine_voice_over_tests
                         PROPERTIES LABELS "portable")

    # GN6b -- docs/roadmap/GENERATIF-2026-10.md. design_voice and
    # generate_voice_over on a project package, against a shell standing in
    # for Python that hands back a real WAV: ingested, marked, undone; the
    # refusal of any reference that is not a voice the engine made (DEC49).
    # Portable: no AppKit, no model; FFmpeg reads the WAV at ingest.
    add_executable(cutmachine_voice_over_tools_tests
                   tests/voice_over_tools_tests.cc)
    target_link_libraries(cutmachine_voice_over_tools_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_voice_over_tools_tests
             COMMAND cutmachine_voice_over_tools_tests)
    set_tests_properties(cutmachine_voice_over_tools_tests
                         PROPERTIES LABELS "portable")

    # DEC39 -- docs/roadmap/PHASES-2026-08.md. Fichier › Ouvrir un projet…
    # and Nouveau projet… in the editor: the checks made before it reopens
    # on another project, on packages made in a temporary folder. Portable:
    # no AppKit, no FFmpeg, no network.
    add_executable(cutmachine_project_switch_tests
                   tests/project_switch_tests.cc)
    target_link_libraries(cutmachine_project_switch_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_switch_tests
             COMMAND cutmachine_project_switch_tests)
    set_tests_properties(cutmachine_project_switch_tests
                         PROPERTIES LABELS "portable")

    # PRD2 -- docs/roadmap/PRODUIT-2026-10.md. FFmpeg checked before it
    # serves, against a script named ffmpeg that lists fewer filters and
    # encoders than the engine uses and logs every call it receives: the
    # refusal is named and nothing but the listings is launched. Portable:
    # no AppKit, no FFmpeg, no network.
    add_executable(cutmachine_ffmpeg_capabilities_tests
                   tests/ffmpeg_capabilities_tests.cc)
    target_link_libraries(cutmachine_ffmpeg_capabilities_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_ffmpeg_capabilities_tests
             COMMAND cutmachine_ffmpeg_capabilities_tests)
    set_tests_properties(cutmachine_ffmpeg_capabilities_tests
                         PROPERTIES LABELS "portable")

    # PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. The setup report, each item
    # in each state, on a machine the test makes up: no $HOME, no real key,
    # no Keychain, no FFmpeg launched. And the order MFA is looked for in.
    # Portable: no AppKit, no network.
    add_executable(cutmachine_setup_check_tests tests/setup_check_tests.cc)
    target_link_libraries(cutmachine_setup_check_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_setup_check_tests
             COMMAND cutmachine_setup_check_tests)
    set_tests_properties(cutmachine_setup_check_tests
                         PROPERTIES LABELS "portable")

    # AX-2026-09 AX6 -- how fc-list is read, how fontconfig compares a
    # family, which families a caption would export in another font, and
    # the Core Text seam filled by the suite itself (FontCatalog.h).
    # Portable: no Core Text, and fontconfig is optional.
    # SYNCHRO-2026-10 SY2 -- a recorder's sound laid under its picture at a
    # given offset, linked, in one undo; then carried by a word cut. Portable.
    add_executable(cutmachine_separate_sound_tests
                   tests/separate_sound_tests.cc)
    target_link_libraries(cutmachine_separate_sound_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_separate_sound_tests
             COMMAND cutmachine_separate_sound_tests)
    set_tests_properties(cutmachine_separate_sound_tests
                         PROPERTIES LABELS "portable")

    # SN4 -- docs/roadmap/SON-2026-10.md. A stem laid at its BWF timecode:
    # the header read, the renderer's rounding inverted, one undo. Portable.
    add_executable(cutmachine_sound_stem_tests tests/sound_stem_tests.cc)
    target_link_libraries(cutmachine_sound_stem_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_sound_stem_tests
             COMMAND cutmachine_sound_stem_tests)
    set_tests_properties(cutmachine_sound_stem_tests
                         PROPERTIES LABELS "portable")

    # SYNCHRO-2026-10 SY1 -- the offset between two sounds of one take, on
    # synthetic signals: exact to the sample, or refused by name. Portable.
    add_executable(cutmachine_audio_offset_tests tests/audio_offset_tests.cc)
    target_link_libraries(cutmachine_audio_offset_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_audio_offset_tests
             COMMAND cutmachine_audio_offset_tests)
    set_tests_properties(cutmachine_audio_offset_tests
                         PROPERTIES LABELS "portable")

    # MOTION-MARQUE-2026-10 MM1 -- the loudness meter and the limiter on
    # EBU Tech 3341's signals. Portable.
    add_executable(cutmachine_loudness_tests tests/loudness_tests.cc)
    target_link_libraries(cutmachine_loudness_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_loudness_tests COMMAND cutmachine_loudness_tests)
    set_tests_properties(cutmachine_loudness_tests
                         PROPERTIES LABELS "portable")

    # FC5 -- FILMCRAFT-2026-10. The sound-processing library against its
    # contract (blocks, allocations, latency, identity) and known answers.
    # Portable.
    add_executable(cutmachine_audio_dsp_tests tests/audio_dsp_tests.cc)
    target_link_libraries(cutmachine_audio_dsp_tests PRIVATE cutmachine_model)
    add_test(NAME cutmachine_audio_dsp_tests COMMAND cutmachine_audio_dsp_tests)
    set_tests_properties(cutmachine_audio_dsp_tests
                         PROPERTIES LABELS "portable")

    # FC5 -- FFmpeg's RBJ biquads as an oracle for ours. Skipped (77), and
    # said so, without an FFmpeg that has them.
    add_executable(cutmachine_audio_dsp_ffmpeg_tests
                   tests/audio_dsp_ffmpeg_tests.cc)
    target_link_libraries(cutmachine_audio_dsp_ffmpeg_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_audio_dsp_ffmpeg_tests
             COMMAND cutmachine_audio_dsp_ffmpeg_tests)
    set_tests_properties(cutmachine_audio_dsp_ffmpeg_tests
                         PROPERTIES LABELS "portable" SKIP_RETURN_CODE 77)

    # FC6 -- FILMCRAFT-2026-10. The proposed ducking on synthetic voices,
    # to the sample. Portable.
    add_executable(cutmachine_audio_dsp_ducking_tests
                   tests/audio_dsp_ducking_tests.cc)
    target_link_libraries(cutmachine_audio_dsp_ducking_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_audio_dsp_ducking_tests
             COMMAND cutmachine_audio_dsp_ducking_tests)
    set_tests_properties(cutmachine_audio_dsp_ducking_tests
                         PROPERTIES LABELS "portable")

    add_executable(cutmachine_font_catalog_tests tests/font_catalog_tests.cc)
    target_link_libraries(cutmachine_font_catalog_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_font_catalog_tests
             COMMAND cutmachine_font_catalog_tests)
    set_tests_properties(cutmachine_font_catalog_tests
                         PROPERTIES LABELS "portable")

    # PRD21, the window's part. What the window says of that report
    # (SetupNotice.h): the welcome screen's line when something required is
    # missing, the diagnosis in French, the window's own Whisper model and
    # Keychain key counted. Portable: plain C++, main.mm only shows it.
    add_executable(cutmachine_setup_notice_tests
                   tests/setup_notice_tests.cc)
    target_link_libraries(cutmachine_setup_notice_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_setup_notice_tests
             COMMAND cutmachine_setup_notice_tests)
    set_tests_properties(cutmachine_setup_notice_tests
                         PROPERTIES LABELS "portable")

    # DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. The window's Whisper
    # model written as the engine's setting, and the old preference migrated
    # once, on a settings file of the suite's own. Portable: plain C++.
    add_executable(cutmachine_whisper_model_setting_tests
                   tests/whisper_model_setting_tests.cc)
    target_link_libraries(cutmachine_whisper_model_setting_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_whisper_model_setting_tests
             COMMAND cutmachine_whisper_model_setting_tests)
    set_tests_properties(cutmachine_whisper_model_setting_tests
                         PROPERTIES LABELS "portable")

    # HEADLESS-2026-10 H4 -- docs/roadmap/HEADLESS-2026-10.md. Where this
    # user's data and log go (UserDirectories.h), macOS and Linux from one
    # host. Portable: plain C++.
    add_executable(cutmachine_user_directories_tests
                   tests/user_directories_tests.cc)
    target_link_libraries(cutmachine_user_directories_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_user_directories_tests
             COMMAND cutmachine_user_directories_tests)
    set_tests_properties(cutmachine_user_directories_tests
                         PROPERTIES LABELS "portable")

    # The whole chain with real processes: a fake ACP agent in Python starts
    # the real `--mcp-serve --stdio` binary the session hands it, and its
    # marker lands in the project through the engine. macOS only, like
    # cutmachine_mcp_endpoint_tests (ProjectStorage, the binary).
    add_executable(cutmachine_acp_agent_tests tests/acp_agent_tests.cc)
    target_link_libraries(cutmachine_acp_agent_tests PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_acp_agent_tests PRIVATE
        CUTMACHINE_EXECUTABLE="$<TARGET_FILE:cutmachine>"
        CUTMACHINE_PYTHON="${Python3_EXECUTABLE}"
        CUTMACHINE_FAKE_ACP_AGENT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_acp_agent.py")
    add_dependencies(cutmachine_acp_agent_tests cutmachine)
    add_test(NAME cutmachine_acp_agent_tests
             COMMAND cutmachine_acp_agent_tests)

    # F1.3 -- wheels and curves (ROADMAP.md). Compiles src/shader.metal as
    # Renderer.mm does and compares its applyColorGrade, on the GPU, with
    # ColorEffects.cc's. Needs a Metal device, not a window; skipped (77)
    # without one.
    add_executable(cutmachine_color_grade_shader_tests
                   tests/color_grade_shader_tests.mm)
    target_compile_options(cutmachine_color_grade_shader_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_color_grade_shader_tests PRIVATE
        CUTMACHINE_SHADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/src/shader.metal"
    )
    target_link_libraries(cutmachine_color_grade_shader_tests PRIVATE
        cutmachine_model
        "-framework Foundation"
        "-framework Metal"
    )
    add_test(NAME cutmachine_color_grade_shader_tests
             COMMAND cutmachine_color_grade_shader_tests)
    set_tests_properties(cutmachine_color_grade_shader_tests PROPERTIES
                         SKIP_RETURN_CODE 77)

    # Visual QA of 19 September 2026 -- a motion clip on a colour-managed
    # monitor is mixed in the SDR display view. Compiles the real shader and
    # compares its compositeDisplayReferred with ColorManagement.cc's rule,
    # on the GPU. Skipped (77) without a Metal device.
    add_executable(cutmachine_display_referred_shader_tests
                   tests/display_referred_shader_tests.mm)
    target_compile_options(cutmachine_display_referred_shader_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_display_referred_shader_tests
        PRIVATE
        CUTMACHINE_SHADER_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/src/shader.metal"
    )
    target_link_libraries(cutmachine_display_referred_shader_tests PRIVATE
        cutmachine_model
        "-framework Foundation"
        "-framework Metal"
    )
    add_test(NAME cutmachine_display_referred_shader_tests
             COMMAND cutmachine_display_referred_shader_tests)
    set_tests_properties(cutmachine_display_referred_shader_tests PROPERTIES
                         SKIP_RETURN_CODE 77)

    # Visual QA of 19 September 2026 -- the Record monitor's picture
    # (Renderer.mm, frames from the app's DecodeWorker) beside the exported
    # file, patch by patch, graded and with motion, with and without colour
    # management. Skipped (77) without a Metal device.
    add_executable(cutmachine_monitor_export_parity_tests
        tests/monitor_export_parity_tests.mm
        src/Renderer.mm
        src/MetalUiAtlas.mm
        src/DecodeWorker.cc
        src/FrameCache.cc
        src/ReadAhead.cc
        src/PerformanceMetrics.cc
        src/MediaSource.mm
    )
    add_dependencies(cutmachine_monitor_export_parity_tests
                     cutmachine_ui_icons)
    target_include_directories(cutmachine_monitor_export_parity_tests
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(cutmachine_monitor_export_parity_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_monitor_export_parity_tests PRIVATE
        CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
        CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    target_link_libraries(cutmachine_monitor_export_parity_tests PRIVATE
        cutmachine_motion_render
        PkgConfig::FFMPEG
        "-framework AppKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework QuartzCore"
    )
    add_test(NAME cutmachine_monitor_export_parity_tests
             COMMAND cutmachine_monitor_export_parity_tests)
    set_tests_properties(cutmachine_monitor_export_parity_tests PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 300)

    # G5 -- docs/roadmap/GRADE-2026-09.md. The scope graticule and false
    # colour read back from the monitor's own passes: the marks land where
    # VideoScopes.h places them, and each level is painted its band.
    # Skipped (77) without a Metal device.
    add_executable(cutmachine_video_scope_render_tests
        tests/video_scope_render_tests.mm
        src/Renderer.mm
        src/MetalUiAtlas.mm
    )
    add_dependencies(cutmachine_video_scope_render_tests cutmachine_ui_icons)
    target_include_directories(cutmachine_video_scope_render_tests
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(cutmachine_video_scope_render_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_video_scope_render_tests PRIVATE
        CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
        CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
    )
    target_link_libraries(cutmachine_video_scope_render_tests PRIVATE
        cutmachine_model
        PkgConfig::FFMPEG
        "-framework AppKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework QuartzCore"
    )
    add_test(NAME cutmachine_video_scope_render_tests
             COMMAND cutmachine_video_scope_render_tests)
    set_tests_properties(cutmachine_video_scope_render_tests PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    # G6 -- docs/roadmap/GRADE-2026-09.md. The reference still's whole
    # loop through the monitor's passes: grabbed, then shown beside the
    # live picture, with and without colour management. Skipped (77)
    # without a Metal device.
    add_executable(cutmachine_monitor_compare_render_tests
        tests/monitor_compare_render_tests.mm
        src/Renderer.mm
        src/MetalUiAtlas.mm
    )
    add_dependencies(cutmachine_monitor_compare_render_tests
                     cutmachine_ui_icons)
    target_include_directories(cutmachine_monitor_compare_render_tests
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(cutmachine_monitor_compare_render_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_monitor_compare_render_tests PRIVATE
        CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
        CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
    )
    target_link_libraries(cutmachine_monitor_compare_render_tests PRIVATE
        cutmachine_model
        PkgConfig::FFMPEG
        "-framework AppKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework QuartzCore"
    )
    add_test(NAME cutmachine_monitor_compare_render_tests
             COMMAND cutmachine_monitor_compare_render_tests)
    set_tests_properties(cutmachine_monitor_compare_render_tests PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    # PERF13 -- a layer MonitorOcclusion.h hides contributes nothing to
    # Record: the monitor's passes render the same values with it and
    # without it, with and without colour management. Skipped (77) without
    # a Metal device.
    add_executable(cutmachine_monitor_occlusion_render_tests
        tests/monitor_occlusion_render_tests.mm
        src/Renderer.mm
        src/MetalUiAtlas.mm
    )
    add_dependencies(cutmachine_monitor_occlusion_render_tests
                     cutmachine_ui_icons)
    target_include_directories(cutmachine_monitor_occlusion_render_tests
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(cutmachine_monitor_occlusion_render_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_monitor_occlusion_render_tests
                               PRIVATE
        CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
        CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
    )
    target_link_libraries(cutmachine_monitor_occlusion_render_tests PRIVATE
        cutmachine_model
        PkgConfig::FFMPEG
        "-framework AppKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework QuartzCore"
    )
    add_test(NAME cutmachine_monitor_occlusion_render_tests
             COMMAND cutmachine_monitor_occlusion_render_tests)
    set_tests_properties(cutmachine_monitor_occlusion_render_tests PROPERTIES
                         SKIP_RETURN_CODE 77 TIMEOUT 120)

    add_executable(cutmachine_audio_playback_tests
        tests/audio_playback_tests.mm
        src/AudioPlayback.mm
        src/AudioDecode.cc
    )
    target_compile_options(cutmachine_audio_playback_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_audio_playback_tests PRIVATE
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    target_link_libraries(cutmachine_audio_playback_tests PRIVATE
        cutmachine_model
        PkgConfig::FFMPEG
        "-framework AVFAudio"
        "-framework Foundation"
    )
    add_test(NAME cutmachine_audio_playback_tests
             COMMAND cutmachine_audio_playback_tests)

    # Native AppKit/Metal smoke test. It launches the real AppDelegate against
    # a disposable project and exercises target/action wiring in-process, so
    # it needs a macOS WindowServer but no Accessibility permission.
    # PRD8 -- docs/roadmap/PRODUIT-2026-10.md. The harness is a category of
    # AppDelegate in its own file, compiled into this target alone.
    add_executable(cutmachine_ui_smoke_tests ${CUTMACHINE_APP_SOURCES}
                   src/UiSmokeHarness.mm)
    add_dependencies(cutmachine_ui_smoke_tests cutmachine_ui_icons)
    target_include_directories(cutmachine_ui_smoke_tests
                               PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(cutmachine_ui_smoke_tests PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    )
    target_compile_definitions(cutmachine_ui_smoke_tests PRIVATE
        CUTMACHINE_UI_SMOKE_TEST=1
        CUTMACHINE_SHADER_PATH="${SHADER_RUNTIME_PATH}"
        CUTMACHINE_ICON_DIR="${CUTMACHINE_ICON_OUTPUT_DIR}"
        # PERF-2026-09. The smoke fixture's media used to be a text file, so
        # every source in it was offline and no decoder ever opened. Real
        # media is what lets the smoke run assert anything about decoders at
        # all -- and it puts actual frames through the monitors besides.
        FFMPEG_EXECUTABLE="${FFMPEG_EXECUTABLE}"
    )
    # cutmachine_motion_render links cutmachine_model publicly; naming the
    # model first as well put the archive twice on the link line.
    target_link_libraries(cutmachine_ui_smoke_tests PRIVATE
        cutmachine_motion_render
        PkgConfig::FFMPEG
        "-framework AppKit"
        "-framework AVFAudio"
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework QuartzCore"
        "-framework ScreenCaptureKit"
        "-framework Security"
        "-framework UniformTypeIdentifiers"
    )
    add_test(NAME cutmachine_ui_smoke_tests
             COMMAND cutmachine_ui_smoke_tests --ui-smoke)
    # PERF7 -- ROADMAP.md. The smoke run keeps the timeline render cost
    # report on, so the path main.mm takes when the editor is measured is the
    # one this test exercises; the report lands in the build tree, never in a
    # project. VQA2 -- with CUTMACHINE_UI_QA_DIR set, the run also exports and
    # captures every REEL scenario state, which takes minutes, not seconds.
    # PRD28 -- a run that could not play every check (its window never key,
    # or hidden) and failed none exits 77: Skipped, not Passed. Its last line
    # says how many were not run and why (SmokeReport.h).
    set_tests_properties(cutmachine_ui_smoke_tests PROPERTIES
        TIMEOUT 300 SKIP_RETURN_CODE 77 ENVIRONMENT
        "CUTMACHINE_RENDER_COST=${CMAKE_CURRENT_BINARY_DIR}/ui_smoke_render_cost.tsv")
    # FI5 -- Chromium startup and AppKit key events share the desktop even
    # when Chrome has no visible window. Do not run them at the same time.
    set_tests_properties(cutmachine_ui_smoke_tests PROPERTIES
                         RESOURCE_LOCK "cutmachine_desktop")

    # PRD28 -- how the window test reports a check it could not play: what
    # each need asks of the screen, its last line, the status CTest reads.
    # Portable: header-only, no AppKit, no window.
    add_executable(cutmachine_smoke_report_tests
                   tests/smoke_report_tests.cc)
    target_include_directories(cutmachine_smoke_report_tests PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME cutmachine_smoke_report_tests
             COMMAND cutmachine_smoke_report_tests)
    set_tests_properties(cutmachine_smoke_report_tests
                         PROPERTIES LABELS "portable")

    add_test(
        NAME cutmachine_sidecar_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest sidecar.test_sidecar
    )
    set_tests_properties(cutmachine_sidecar_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    # SN3 -- docs/roadmap/SON-2026-10.md. The sidecar's boundary to the
    # sound renderer: Python and Node only, skipped where Node is missing.
    add_test(
        NAME cutmachine_sidecar_sound_tests
        COMMAND "${Python3_EXECUTABLE}" -m unittest sidecar.test_sound
    )
    set_tests_properties(cutmachine_sidecar_sound_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        LABELS "portable"
    )

    # QC-2026-08 -- these regression suites were delivered as detached
    # artifacts under "Test copilot/". Register them here so exact-time,
    # stateful edit-log and storage-failure properties run with the normal
    # local CTest gate instead of existing only as unaudited source files.
    add_executable(cutmachine_rational_time_property_tests
                   tests/rational_time_property_tests.cc)
    target_link_libraries(cutmachine_rational_time_property_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_rational_time_property_tests
             COMMAND cutmachine_rational_time_property_tests)

    add_executable(cutmachine_edit_sequence_tests
                   tests/edit_sequence_tests.cc)
    target_link_libraries(cutmachine_edit_sequence_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_edit_sequence_tests
             COMMAND cutmachine_edit_sequence_tests)

    add_executable(cutmachine_storage_fault_injection_tests
                   tests/storage_fault_injection_tests.cc)
    target_link_libraries(cutmachine_storage_fault_injection_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_storage_fault_injection_tests
             COMMAND cutmachine_storage_fault_injection_tests)

    set_tests_properties(
        cutmachine_rational_time_property_tests
        cutmachine_edit_sequence_tests
        cutmachine_storage_fault_injection_tests
        PROPERTIES LABELS "portable;regression"
    )

    # FC8 -- docs/roadmap/FILMCRAFT-2026-10.md. Random sequences of timeline
    # operations from fixed seeds, on an edit with linked pairs, gaps and
    # markers: every operation is refused without a trace, or validates and
    # undoes and redoes byte for byte, and the whole sequence undoes to the
    # start. CUTMACHINE_OP_SEED replays the seed a failure prints.
    add_executable(cutmachine_operation_property_tests
                   tests/operation_property_tests.cc)
    target_link_libraries(cutmachine_operation_property_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_operation_property_tests
             COMMAND cutmachine_operation_property_tests)
    set_tests_properties(cutmachine_operation_property_tests PROPERTIES
        LABELS "portable;regression"
    )

    # S6 -- SAVING_ROADMAP.md. Project history recorded as deltas: every
    # project operation round-trips byte for byte on an interview-sized
    # fixture with a bounded journal entry, and a pre-S6 journal still
    # replays and shrinks once read.
    add_executable(cutmachine_project_history_tests
                   tests/project_history_tests.cc)
    target_link_libraries(cutmachine_project_history_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_history_tests
             COMMAND cutmachine_project_history_tests)

    # DEC24 -- SAVING_ROADMAP.md. A project journal written before S6,
    # frozen in tests/fixtures/project_log, migrated at the opening under
    # the session lock: written back as deltas, the rest of the package byte
    # for byte, every undo and redo where it was. Plain C++.
    add_executable(cutmachine_project_log_migration_tests
                   tests/project_log_migration_tests.cc)
    target_link_libraries(cutmachine_project_log_migration_tests
                          PRIVATE cutmachine_model)
    target_compile_definitions(cutmachine_project_log_migration_tests PRIVATE
        PROJECT_LOG_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/project_log")
    add_test(NAME cutmachine_project_log_migration_tests
             COMMAND cutmachine_project_log_migration_tests)

    # S7 -- SAVING_ROADMAP.md. Project file version 3: canonical round trip,
    # version-2 files migrated to the same project, and a file that grows
    # with each timeline's content rather than with copies of the library.
    add_executable(cutmachine_project_format_tests
                   tests/project_format_tests.cc)
    target_link_libraries(cutmachine_project_format_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_project_format_tests
             COMMAND cutmachine_project_format_tests)

    # S8 -- SAVING_ROADMAP.md. The reproducible save benchmark PERF-2026-09
    # lacked: an interview-shaped package built in memory, driven through
    # the CLI and the editor's commit path, with the bytes written held to
    # bounds and every timing printed.
    add_executable(cutmachine_save_benchmark_tests
                   tests/save_benchmark_tests.cc)
    target_link_libraries(cutmachine_save_benchmark_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_save_benchmark_tests
             COMMAND cutmachine_save_benchmark_tests)

    set_tests_properties(
        cutmachine_storage_fault_injection_tests
        cutmachine_project_recovery_tests
        cutmachine_project_storage_tests
        cutmachine_project_history_tests
        cutmachine_project_format_tests
        PROPERTIES LABELS "storage"
    )
    set_tests_properties(cutmachine_project_log_migration_tests PROPERTIES
        LABELS "storage;portable")
    set_tests_properties(cutmachine_save_benchmark_tests PROPERTIES
        LABELS "storage;perf"
        TIMEOUT 300
    )

    # PERF8 -- ROADMAP.md. The load side of the same reference: opening a
    # project and an MCP server's tool calls, on S8's fixture, with the work
    # each step does (package parses, media probes, media hashes, package
    # files committed -- src/WorkCounters.h) held to bounds and every timing
    # printed. scripts/perf_bench.sh runs it on the external drive.
    add_executable(cutmachine_load_benchmark_tests
                   tests/load_benchmark_tests.cc)
    target_link_libraries(cutmachine_load_benchmark_tests
                          PRIVATE cutmachine_model)
    add_test(NAME cutmachine_load_benchmark_tests
             COMMAND cutmachine_load_benchmark_tests)
    set_tests_properties(cutmachine_load_benchmark_tests PROPERTIES
        LABELS "storage;perf"
        TIMEOUT 300
    )

    # The Resolve plugin's pure core is Lua, so its tests run under fuscript,
    # the Lua 5.1 interpreter DaVinci Resolve ships. Registered only where
    # Resolve is installed: this is a tool test, not a reason to fail a build
    # on a machine without it.
    #
    # fuscript ALWAYS exits 0 -- on os.exit(1) and on an uncaught Lua error
    # alike -- so the verdict has to come from its output. Without
    # PASS_REGULAR_EXPRESSION this test would pass while asserting nothing.
    set(RESOLVE_FUSCRIPT "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fuscript")
    if(EXISTS "${RESOLVE_FUSCRIPT}")
        add_test(
            NAME cutmachine_resolve_plugin_tests
            COMMAND "${RESOLVE_FUSCRIPT}" -l lua
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/resolve-plugin/tests/test_cutmachine_lua.lua"
        )
        set_tests_properties(cutmachine_resolve_plugin_tests PROPERTIES
            PASS_REGULAR_EXPRESSION "plugin resolve : tests passés"
            TIMEOUT 30
        )
    endif()

    # PRODUIT-2026-10 PRD6 -- a sanitizer build gives the ASan flags to C
    # and C++ only. A suite built from a .mm links Objective-C++
    # that is not, and its copies of libc++'s container annotations
    # (std::vector<float>::__annotate_new...) are empty functions. The linker
    # keeps one copy of each for the whole program, OpenColorIO included: a
    # vector annotated by one copy and grown by another reads as a container
    # overflow, the false positive the ASan documentation names. These
    # suites run without that one check; every other one stays on.
    # cutmachine_motion_media_tests joined them on 11 November 2026: it
    # links CaptionRender.mm and MotionRender.mm, and its binary holds an
    # empty std::vector<unsigned char>::__annotate_new (20 bytes) beside an
    # instrumented __annotate_increase, so the bytes ReadFfmpegRawFrames
    # appends to a vector read as an overflow.
    foreach(_cutmachine_test IN ITEMS
            cutmachine_motion_media_tests
            cutmachine_color_grade_shader_tests
            cutmachine_display_referred_shader_tests
            cutmachine_monitor_export_parity_tests
            cutmachine_video_scope_render_tests
            cutmachine_monitor_compare_render_tests
            cutmachine_monitor_occlusion_render_tests
            cutmachine_audio_playback_tests)
        if(TEST ${_cutmachine_test})
            set_property(TEST ${_cutmachine_test} APPEND PROPERTY
                ENVIRONMENT_MODIFICATION
                "ASAN_OPTIONS=string_append::detect_container_overflow=0")
        endif()
    endforeach()

    get_property(_cutmachine_all_tests DIRECTORY PROPERTY TESTS)
    foreach(_cutmachine_test IN LISTS _cutmachine_all_tests)
        set_property(TEST ${_cutmachine_test} APPEND PROPERTY ENVIRONMENT
            # DB2 -- suites opt into automatic archiving in temporary files.
            "CUTMACHINE_PROJECT_ARCHIVE=off"
            # DEC44 -- nor copy one into the folder the user's settings
            # file names for the archive's daily copy.
            "CUTMACHINE_ARCHIVE_BACKUP_DIR=off"
            # PRD35 -- the servers a suite starts keep their engine log in
            # the build tree, never in the user's ~/Library/Logs.
            "CUTMACHINE_LOG_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-logs"
            # MB10 -- no suite reads or writes the user's motion library
            # (its personal base and its inbox); a suite that checks the
            # library sets its own.
            "CUTMACHINE_MOTION_LIBRARY=${CMAKE_CURRENT_BINARY_DIR}/test-motion-library"
            # RM3 -- no suite cuts cartons with a model installed on the
            # machine it runs on; a suite that checks the models sets its own.
            "CUTMACHINE_MODELS=off"
            # PA16 -- no suite sends a timeline to the Resolve open on the
            # machine: the bridge named here does not exist, and the
            # descriptors stay in the build tree.
            "CUTMACHINE_RESOLVE_BRIDGE=${CMAKE_CURRENT_BINARY_DIR}/test-no-resolve-bridge.py"
            "CUTMACHINE_RESOLVE_SEND_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-resolve-send")
    endforeach()

    # Test hygiene -- the same variables for a suite run by hand, outside
    # CTest: tests/test_environment.cc, linked into every test executable,
    # sets each one before main() when it is unset. Keep the two lists
    # alike. On 8 October 2026, suites run by hand had filled the user's
    # project archive with 1 177 captures of their fixtures.
    set_source_files_properties(tests/test_environment.cc PROPERTIES
        COMPILE_DEFINITIONS
        "CUTMACHINE_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
    get_property(_cutmachine_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_cutmachine_target IN LISTS _cutmachine_targets)
        if(_cutmachine_target MATCHES "^cutmachine_.*_tests$")
            get_target_property(_cutmachine_type ${_cutmachine_target} TYPE)
            if(_cutmachine_type STREQUAL "EXECUTABLE")
                target_sources(${_cutmachine_target} PRIVATE
                               tests/test_environment.cc)
            endif()
        endif()
    endforeach()
endif()
