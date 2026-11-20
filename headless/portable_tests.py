"""HEADLESS-2026-10 (spike) -- the portable suites of the main build, for the
headless build.

Reads CMakeLists.txt, keeps the tests labelled "portable" whose command is
one of its own test executables built from .cc sources, and writes a CMake
fragment that declares them again against the headless cutmachine_model.
What it cannot carry over (a .mm source, an Apple-only library, a command
that is not a plain target) is listed as skipped, with the reason, so that
nothing is dropped silently.

    python3 portable_tests.py <CMakeLists.txt> <output.cmake>
"""
import re
import sys

source_path, output_path = sys.argv[1], sys.argv[2]
every_suite = len(sys.argv) > 3 and sys.argv[3] == "--all"
text = open(source_path, encoding="utf-8").read()
# Comments out, so a commented example never counts as a declaration.
text = re.sub(r"#[^\n]*", "", text)


def calls(name):
    """Every name( ... ) call, with its arguments as one string."""
    for match in re.finditer(rf"\b{name}\s*\(", text):
        depth, index = 1, match.end()
        while depth and index < len(text):
            depth += {"(": 1, ")": -1}.get(text[index], 0)
            index += 1
        yield text[match.end():index - 1]


def words(arguments):
    # NAME="${VALUE}" is one argument, as CMake reads it.
    return re.findall(r'(?:[^\s"]|"[^"]*")+', arguments)


# STRUCTURE-2026-11 ST2 -- a suite declared in one call
# (cmake/CutmachineAddTest.cmake) is read as its plain calls would be: its
# executable and sources, its libraries and definitions, its command, its
# labels.
SUITE_KEYWORDS = {"SOURCES", "INCLUDES", "OPTIONS", "DEFINITIONS", "LIBS",
                  "DEPENDS", "ARGS", "LABELS"}
suites = {}
for arguments in calls("cutmachine_add_test"):
    tokens = words(arguments)
    if not tokens:
        continue
    sections, current = {}, None
    for token in tokens[1:]:
        if token in SUITE_KEYWORDS:
            current = sections.setdefault(token, [])
        elif current is not None:
            current.append(token)
    suites[tokens[0]] = sections

# A test's LABELS are the last set_tests_properties that sets them, as
# CMake reads them: cutmachine_storage_fault_injection_tests is given
# "portable;regression", then "storage" alone.
labels_of = {name: [label.strip('"') for label in sections["LABELS"]]
             for name, sections in suites.items() if "LABELS" in sections}
# A suite that exits 77 when its tool is missing (FFmpeg, Node) declares
# SKIP_RETURN_CODE in the main build; without it here, a skip reads as a
# failure on a machine that lacks the tool (FILMCRAFT-2026-10).
skip_code_of = {}
for arguments in calls("set_tests_properties"):
    tokens = words(arguments)
    if "PROPERTIES" not in tokens:
        continue
    names = tokens[:tokens.index("PROPERTIES")]
    properties = tokens[tokens.index("PROPERTIES") + 1:]
    for key, value in zip(properties[::2], properties[1::2]):
        if key == "LABELS":
            for name in names:
                labels_of[name] = value.strip('"').split(";")
        if key == "SKIP_RETURN_CODE":
            for name in names:
                skip_code_of[name] = value


def skip_property(test):
    if test not in skip_code_of:
        return ""
    return f" SKIP_RETURN_CODE {skip_code_of[test]}"


portable = [name for name, labels in labels_of.items() if "portable" in labels]

commands = {}
for arguments in calls("add_test"):
    tokens = words(arguments)
    if len(tokens) >= 4 and tokens[0] == "NAME" and tokens[2] == "COMMAND":
        commands[tokens[1]] = tokens[3:]

executables = {}
for arguments in calls("add_executable"):
    tokens = words(arguments)
    if tokens:
        executables[tokens[0]] = tokens[1:]

links = {}
for arguments in calls("target_link_libraries"):
    tokens = words(arguments)
    if tokens:
        links.setdefault(tokens[0], []).extend(
            token for token in tokens[1:]
            if token not in ("PRIVATE", "PUBLIC", "INTERFACE"))

definitions = {}
for arguments in calls("target_compile_definitions"):
    tokens = words(arguments)
    if tokens:
        definitions.setdefault(tokens[0], []).extend(
            token for token in tokens[1:]
            if token not in ("PRIVATE", "PUBLIC", "INTERFACE"))

for name, sections in suites.items():
    executables[name] = sections.get("SOURCES", [])
    commands[name] = [name] + sections.get("ARGS", [])
    if "LIBS" in sections:
        links.setdefault(name, []).extend(sections["LIBS"])
    if "DEFINITIONS" in sections:
        definitions.setdefault(name, []).extend(sections["DEFINITIONS"])

LIBRARY_MAP = {
    "cutmachine_model": "cutmachine_model",
    "cutmachine_sqlite": "SQLite::SQLite3",
    "yogacore": "yogacore",
    "PkgConfig::FFMPEG": "PkgConfig::FFMPEG",
    "Threads::Threads": "Threads::Threads",
    "OpenColorIO": "OpenColorIO",
    "whisper": "whisper",
}

# HEADLESS-2026-10 H3 -- a suite the main build runs through Python or
# CMake's script mode is declared again with the same command: the guard of
# the architecture (architecture_tests.py) runs here too. FI5 -- a Node
# suite as well, declared only where Node was found, as in the main build.
SCRIPT_RUNNERS = {"${Python3_EXECUTABLE}", "${CMAKE_COMMAND}",
                  "${CUTMACHINE_NODE_EXECUTABLE}"}


def unquoted(token):
    return token[1:-1] if len(token) >= 2 and token[0] == token[-1] == '"' \
        else token


def carried(argument):
    return argument.replace("${CMAKE_CURRENT_SOURCE_DIR}", "${CUTMACHINE_ROOT}")


lines, skipped, kept = [], [], []
candidates = sorted(commands) if every_suite else sorted(set(portable))
for test in candidates:
    command = commands.get(test)
    if not command:
        skipped.append((test, "no add_test(NAME ... COMMAND ...) found"))
        continue
    executable, arguments = command[0], command[1:]
    if unquoted(executable) in SCRIPT_RUNNERS:
        if any("$<" in argument for argument in arguments):
            skipped.append((test, "its command uses a generator expression"))
            continue
        kept.append(test)
        # BuildInfo's test needs git, as in the main build; a Node suite
        # needs Node.
        needs_git = any("${GIT_EXECUTABLE}" in a for a in arguments)
        needs_node = unquoted(executable) == "${CUTMACHINE_NODE_EXECUTABLE}"
        if needs_git:
            lines.append("if(GIT_FOUND)")
        if needs_node:
            lines.append("if(CUTMACHINE_NODE_EXECUTABLE)")
        argument_text = " ".join(carried(a) for a in arguments)
        lines.append(f"add_test(NAME {test} COMMAND {executable} {argument_text})")
        label = "portable" if test in portable else "engine"
        lines.append(f'set_tests_properties({test} PROPERTIES LABELS "{label}"'
                     ' TIMEOUT 300 WORKING_DIRECTORY "${CUTMACHINE_ROOT}"'
                 + skip_property(test) + ")")
        if needs_git or needs_node:
            lines.append("endif()")
        lines.append("")
        continue
    if executable not in executables:
        skipped.append((test, f"command '{executable}' is not a test target"))
        continue
    sources = executables[executable]
    if any(not source.endswith(".cc") for source in sources):
        skipped.append((test, "a source is not plain C++: " + " ".join(sources)))
        continue
    libraries = links.get(executable, [])
    unmapped = [library for library in libraries if library not in LIBRARY_MAP]
    if unmapped:
        skipped.append((test, "links " + ", ".join(unmapped)))
        continue
    if any("$<" in argument for argument in arguments):
        skipped.append((test, "its command uses a generator expression"))
        continue
    target = executable
    kept.append(test)
    lines.append(f"add_executable({target}")
    lines.extend(f'    "${{CUTMACHINE_ROOT}}/{source}"' for source in sources)
    lines.append('    "${CUTMACHINE_ROOT}/tests/test_environment.cc")')
    mapped = sorted({LIBRARY_MAP[library] for library in libraries} |
                    {"cutmachine_model"})
    lines.append(f"target_link_libraries({target} PRIVATE {' '.join(mapped)})")
    lines.append(f"target_include_directories({target} PRIVATE "
                 '"${CUTMACHINE_ROOT}/tests")')
    if executable in definitions:
        values = " ".join(definitions[executable]).replace(
            "${CMAKE_CURRENT_SOURCE_DIR}", "${CUTMACHINE_ROOT}")
        lines.append(f"target_compile_definitions({target} PRIVATE {values})")
    argument_text = " ".join(carried(a) for a in arguments)
    lines.append(f"add_test(NAME {test} COMMAND {target} {argument_text})")
    label = "portable" if test in portable else "engine"
    lines.append(f'set_tests_properties({test} PROPERTIES LABELS "{label}"'
                 ' TIMEOUT 300 WORKING_DIRECTORY "${CUTMACHINE_ROOT}"'
                 + skip_property(test) + ")")
    lines.append("")

with open(output_path, "w", encoding="utf-8") as output:
    output.write("# Generated by headless/portable_tests.py -- do not edit.\n")
    output.write(f"# {len(kept)} portable suites kept, {len(skipped)} skipped.\n")
    for test, reason in skipped:
        output.write(f"# skipped {test}: {reason}\n")
    output.write("\n".join(lines) + "\n")
print(f"portable suites: {len(kept)} kept, {len(skipped)} skipped")
for test, reason in skipped:
    print(f"  skipped {test}: {reason}")
