# SN9 -- docs/roadmap/SON-2026-10.md. Where Node finds the imports of the fetched Strudel.
#
# Strudel's modules import "@strudel/core" and "fraction.js" by name, as its npm workspace
# resolves them. Node looks for such a name in the node_modules folders above the importing file,
# so links in the folder that holds the fetched sources answer them: no npm, no copy, the sources
# as fetched. "@kabelsalat/web" is imported by Strudel's REPL alone (repl.mjs), which a pattern
# compiled by tools/cutmachine-sound never starts: an empty stand-in answers that import.
#
# A checkout given by FETCHCONTENT_SOURCE_DIR_STRUDEL keeps its own node_modules (pnpm install):
# nothing is written beside it.
#
#   include(cmake/StrudelModules.cmake) after FetchContent_MakeAvailable(strudel fractionjs)

get_filename_component(_strudel_parent "${strudel_SOURCE_DIR}" DIRECTORY)
file(RELATIVE_PATH _strudel_inside "${CMAKE_BINARY_DIR}" "${_strudel_parent}")
if(_strudel_inside MATCHES "^\\.\\.")
    message(STATUS "Strudel from ${strudel_SOURCE_DIR}: its own node_modules resolve its imports")
else()
    set(_strudel_modules "${_strudel_parent}/node_modules")
    file(MAKE_DIRECTORY "${_strudel_modules}/@strudel" "${_strudel_modules}/@kabelsalat/web")
    file(CREATE_LINK "${strudel_SOURCE_DIR}/packages/core" "${_strudel_modules}/@strudel/core"
         SYMBOLIC)
    file(CREATE_LINK "${strudel_SOURCE_DIR}/packages/mini" "${_strudel_modules}/@strudel/mini"
         SYMBOLIC)
    file(CREATE_LINK "${fractionjs_SOURCE_DIR}" "${_strudel_modules}/fraction.js" SYMBOLIC)
    file(WRITE "${_strudel_modules}/@kabelsalat/web/package.json"
         "{ \"name\": \"@kabelsalat/web\", \"type\": \"module\", \"main\": \"index.mjs\" }\n")
    file(WRITE "${_strudel_modules}/@kabelsalat/web/index.mjs"
         "// Stand-in written by cmake/StrudelModules.cmake: only Strudel's REPL uses it.\n"
         "export class SalatRepl {}\n")
endif()
