#!/usr/bin/env python3
"""Executable architecture guard for PHILOSOPHY.md sections 2 and 3."""

from __future__ import annotations

import re
import sys
from pathlib import Path


failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def variant_members(header: str, alias: str) -> set[str]:
    match = re.search(
        rf"using\s+{re.escape(alias)}\s*=\s*std::variant<(.*?)>;",
        header,
        re.DOTALL,
    )
    if not match:
        failures.append(f"Operations.h no longer declares {alias}")
        return set()
    return set(re.findall(r"\b[A-Z][A-Za-z0-9_]*Operation\b", match.group(1)))


def between(text: str, start: str, end: str | None) -> str:
    begin = text.find(start)
    if begin < 0:
        failures.append(f"missing architecture marker: {start}")
        return ""
    if end is None:
        return text[begin:]
    finish = text.find(end, begin + len(start))
    if finish < 0:
        failures.append(f"missing architecture marker: {end}")
        return ""
    return text[begin:finish]


def matching_close(text: str, start: int, opening: str, closing: str) -> int:
    """Index of the bracket closing the one at `start`, or -1."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opening:
            depth += 1
        elif text[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    return -1


def backend_refusing_defaults(header: str) -> set[str]:
    """McpBackend virtuals whose default body refuses ("this backend ...")."""
    names: set[str] = set()
    for match in re.finditer(r"\bvirtual\b[^;{()]*?\b([A-Z]\w*)\s*\(", header):
        close = matching_close(header, match.end() - 1, "(", ")")
        if close < 0:
            continue
        after = re.match(r"\s*(?:const\s*)?\{", header[close + 1 :])
        if not after:
            continue  # pure virtual: every backend has to define it anyway
        body_start = close + 1 + after.end() - 1
        body_end = matching_close(header, body_start, "{", "}")
        if body_end >= 0 and '"this backend' in header[body_start:body_end]:
            names.add(match.group(1))
    return names


def adapter_method_body(adapter: str, method: str) -> str:
    match = re.search(rf"\bbool\s+{re.escape(method)}\s*\(", adapter)
    if not match:
        failures.append(f"the chat adapter no longer defines {method}")
        return ""
    close = matching_close(adapter, match.end() - 1, "(", ")")
    body_start = adapter.find("{", close)
    body_end = matching_close(adapter, body_start, "{", "}")
    return adapter[body_start:body_end]


def class_public_methods(header: str, class_name: str) -> list[tuple[str, bool, bool]]:
    match = re.search(
        rf"class\s+{re.escape(class_name)}\s*\{{\s*public:(.*?)\n\}};",
        header,
        re.DOTALL,
    )
    if not match:
        failures.append(f"unable to inspect public API of {class_name}")
        return []
    body = re.sub(r"//.*", "", match.group(1))
    methods: list[tuple[str, bool, bool]] = []
    for declaration in body.split(";"):
        compact = " ".join(declaration.split())
        method = re.search(r"(?:^|\s)(~?[A-Za-z_]\w*)\s*\([^()]*\)\s*(const)?$", compact)
        if not method:
            continue
        methods.append(
            (method.group(1), "static " in compact, method.group(2) == "const")
        )
    return methods


def third_party_dependencies(root: Path) -> int:
    """PRD23 -- every FetchContent dependency has its notice and its licence.

    Lives here because, like the rest of this guard, it reads the sources
    rather than a build. Returns how many dependencies it checked.
    """
    # No '#' appears inside a string in CMakeLists.txt, so cutting each line
    # at the first one drops the comments, parentheses included.
    cmake = "\n".join(
        line.split("#", 1)[0]
        for line in read(root, "CMakeLists.txt").splitlines()
    )
    notices_path = root / "THIRD_PARTY_NOTICES.md"
    if not notices_path.is_file():
        failures.append("THIRD_PARTY_NOTICES.md is missing")
        return 0
    notices = notices_path.read_text(encoding="utf-8")

    copies = re.findall(
        r"\bcutmachine_bundle_license\(\s*(\S+)\s+\"([^\"]+)\"", cmake
    )
    check(
        any(name == "THIRD_PARTY_NOTICES.md" for name, _ in copies),
        "the bundle no longer carries THIRD_PARTY_NOTICES.md",
    )
    check(
        re.search(
            r"add_executable\(cutmachine_app\s+MACOSX_BUNDLE[^)]*"
            r"\$\{CUTMACHINE_LICENSE_RESOURCES\}",
            cmake,
        )
        is not None
        and re.search(
            r"set_source_files_properties\(\$\{CUTMACHINE_LICENSE_RESOURCES\}"
            r"\s+PROPERTIES\s+MACOSX_PACKAGE_LOCATION\s+Resources/licenses\b",
            cmake,
        )
        is not None,
        "cutmachine_app no longer ships CUTMACHINE_LICENSE_RESOURCES in "
        "Contents/Resources/licenses",
    )
    # Every file the bundle's licences directory receives is named in the
    # notices: the ones copied by the helper, the ones copied once
    # OpenColorIO has built its own libraries ("target:file:name"), and
    # those written there directly.
    bundled = {name for name, _ in copies}
    bundled.update(
        re.findall(r"\"[\w.-]+_install:[\w.-]+:([\w.+-]+)\"", cmake)
    )
    bundled.update(
        re.findall(r"\$\{CUTMACHINE_LICENSE_DIR\}/([\w.+-]+)", cmake)
    )
    bundled.discard("THIRD_PARTY_NOTICES.md")
    for name in sorted(bundled):
        check(
            f"`{name}`" in notices,
            f"THIRD_PARTY_NOTICES.md does not name the bundled file {name}",
        )

    count = 0
    for match in re.finditer(r"\bFetchContent_Declare\s*\(", cmake):
        close = matching_close(cmake, match.end() - 1, "(", ")")
        words = cmake[match.end() : close].split()
        if close < 0 or not words:
            failures.append("unreadable FetchContent_Declare in CMakeLists.txt")
            continue
        name = words[0]
        count += 1
        location = ""
        for key in ("URL", "GIT_REPOSITORY"):
            if key in words[1:-1]:
                location = words[words.index(key, 1) + 1]
        # The notices name the project by its repository: org/repo on GitHub,
        # the whole location anywhere else.
        github = re.search(
            r"github\.com/([\w.-]+)/([\w.-]+?)(?:\.git)?(?:/|$)", location
        )
        reference = (
            f"{github.group(1)}/{github.group(2)}" if github else location
        )
        check(
            bool(reference) and reference in notices,
            f"FetchContent dependency {name} has no row in "
            "THIRD_PARTY_NOTICES.md",
        )
        # Pinned by content: an archive and its digest, never a git ref that
        # can move. The commit of the archive is the version the notices
        # give, so bumping one without the other fails here.
        check(
            "GIT_REPOSITORY" not in words
            and "URL" in words
            and re.search(
                r"\bURL_HASH\s+SHA256=[0-9a-f]{64}\b",
                cmake[match.end() : close],
            )
            is not None,
            f"FetchContent dependency {name} is not pinned by URL and "
            "URL_HASH SHA256",
        )
        commit = re.search(
            r"/archive/([0-9a-f]{40})\.(?:tar\.gz|zip)$", location
        )
        check(
            commit is None or commit.group(1) in notices,
            f"THIRD_PARTY_NOTICES.md does not give the commit {name} is "
            "built from",
        )
        check(
            any(
                source.startswith(f"${{{name}_SOURCE_DIR}}/")
                for _, source in copies
            ),
            f"FetchContent dependency {name} has no licence copied into the "
            "bundle (cutmachine_bundle_license)",
        )
    check(count > 0, "CMakeLists.txt declares no FetchContent dependency")
    return count


# PHILOSOPHY.md, principle 2 -- nothing writes a Document but an operation.
# Document keeps its members public (117 files read them), so the compiler
# cannot hold the line; this does, for the two ways a write is spelled: a
# member of a document assigned or edited in place, and a non-const pointer
# taken into one through Find*. Ingest's repairs and the window's proxies
# were both written that way, outside any journal, until they became
# operations (IngestMediaOperation's `completed` and `mounted`,
# SetMediaProxyOperation).
DOCUMENT_WRITERS = {
    # The algebra itself, and the document's own codec and validation.
    "Operations.cc",
    "MotionOperations.cc",
    "Document.cc",
    "DocumentFormat.cc",
    # A Project assembles one timeline's Document as a view and takes an
    # edited one back (MakeDocument, CommitDocument); storage builds the
    # standalone document of a timeline the same way. Neither edits.
    "Project.cc",
    "ProjectStorage.cc",
}
# The window test builds its fixtures before any journal exists; its harness
# lives under tests/ui_smoke/ (STRUCTURE-2026-11 ST1), out of this scan.
# A line that builds a throwaway document for display or measurement, never
# written back, says so with this marker.
DOCUMENT_VIEW_MARKER = "// document view, not an edit"
_DOCUMENT_MEMBERS = (
    "library|bins|sources|compositions|sequence|color_management|version"
)
_DOCUMENT_WRITE = re.compile(
    r"\b\w*(?:[dD]ocument|candidate|staged)\w*(?:\.|->)"
    rf"(?:{_DOCUMENT_MEMBERS})\b(?:[.\[][^;=]*?)?\s*"
    r"(?:(?<![=!<>])=(?!=)|\.(?:push_back|emplace_back|erase|insert|clear|"
    r"resize|assign|swap)\()"
)
_DOCUMENT_POINTER = re.compile(
    r"^\s*(?:(?:if|while)\s*\(\s*)?(?:LibraryMedia|DocumentSource|DocumentClip|DocumentTrack|"
    r"DocumentBin|DocumentComposition|DocumentMarker|DocumentTransition)"
    r"\s*\*\s*\w+\s*=\s*[^;]*\bFind\w*\("
)


def document_writes(root: Path) -> list[str]:
    found: list[str] = []
    for source in src_files(root):
        if source.suffix not in {".cc", ".mm", ".h"}:
            continue
        if source.name in DOCUMENT_WRITERS:
            continue
        lines = source.read_text(encoding="utf-8").splitlines()
        for number, line in enumerate(lines, start=1):
            stripped = line.strip()
            if stripped.startswith("//") or DOCUMENT_VIEW_MARKER in line:
                continue
            # A declaration clang-format broke after its `=` reads on.
            joined = line
            if stripped.endswith("=") and number < len(lines):
                joined = line + " " + lines[number].strip()
            if _DOCUMENT_WRITE.search(line) or _DOCUMENT_POINTER.search(joined):
                found.append(f"{source.name}:{number}: {stripped}")
    return found


# PHILOSOPHY.md, principle 3 -- the command line is a surface over the model,
# not under it. Cli.h declares the commands; what the model needs from below
# them (the answer's envelope, a package's files) is in CommandEnvelope.h and
# PackageFiles.h. Only the surfaces, and the files that define commands of
# their own on top of ApplyOperationCommand, include Cli.h. The list can
# shrink; a file added to it says why.
CLI_HEADER_INCLUDERS = {
    "Cli.cc",
    "CliMain.cc",
    # Commands of their own, which apply an operation the way --apply-op
    # does (ApplyOperationCommand, ApplyProjectOperationCommand).
    "Edl.cc",
    "MotionCli.cc",
    "MulticamCli.cc",
    # The MCP backends run the commands and read their answers back.
    "McpLiveBackend.cc",
    "McpProjectBackend.cc",
    # The window, and its adapter between the chat panel and the backend.
    "AppDelegatePrivate.h",
    "ChatPanelView.mm",
    "MainThreadBackend.h",
}


def cli_header_includers(root: Path) -> set[str]:
    return {
        source.name
        for source in src_files(root)
        if source.suffix in {".cc", ".mm", ".h"}
        and '#include "Cli.h"' in source.read_text(encoding="utf-8")
    }


# STRUCTURE-2026-11 ST3 -- docs/ARCHITECTURE.md. src/ is in its modules, one
# directory each (ST3b), and a file includes only headers of its module or
# of one before it in MODULE_ORDER. The portable modules name no Apple
# framework; CommonCrypto is answered under Linux by headless/compat. The
# includes that go against the order today are listed: the list can only
# shrink, and an entry no longer true has to go. Headers are included by
# their bare name from every module, so two files may not share one.
MODULE_ORDER = ("engine", "media", "motion", "ui", "agent", "platform", "app")
KNOWN_UPWARD_INCLUDES = {
    ("Edl.cc", "Cli.h"),
    ("Edl.cc", "TimelineView.h"),
    ("TimelineCheck.h", "MotionTemplates.h"),
    ("AgentJournal.h", "McpEndpoint.h"),
}
PORTABLE_MODULES = {"engine", "media", "motion", "ui", "agent"}
_MODULE_CODE = {".h", ".cc", ".mm", ".metal", ".in"}
_QUOTED_INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s*"([^"]+)"', re.M)
_APPLE_FRAMEWORK = re.compile(
    r'^\s*#\s*(?:include|import)\s*<((?:AppKit|Foundation|Metal|MetalKit|'
    r"MetalPerformanceShaders|QuartzCore|CoreText|CoreGraphics|CoreVideo|"
    r"CoreMedia|AVFoundation|AVFAudio|Vision|ImageIO|ScreenCaptureKit|"
    r"Security|UniformTypeIdentifiers|JavaScriptCore|VideoToolbox|"
    r"CoreFoundation|objc|dispatch|os|mach|Accelerate|IOKit|CoreServices)/"
    r"[^>]+)>",
    re.M,
)


def src_files(root: Path) -> list[Path]:
    """Every file of src/, in its module directory or not."""
    return sorted(path for path in (root / "src").rglob("*") if path.is_file())


def module_layering(root: Path) -> None:
    order = list(MODULE_ORDER)
    paths: dict[str, Path] = {}
    table: dict[str, str] = {}
    for path in src_files(root):
        if path.suffix not in _MODULE_CODE:
            continue
        relative = path.relative_to(root / "src")
        module = relative.parts[0] if len(relative.parts) == 2 else None
        check(
            module in order,
            f"src/{relative} is not in a module's directory "
            f"({', '.join(order)}): see docs/ARCHITECTURE.md",
        )
        check(
            path.name not in paths,
            f"src/{relative} and "
            f"{paths[path.name].relative_to(root) if path.name in paths else ''} "
            "share a name: "
            "headers are included by their bare name",
        )
        paths[path.name] = path
        if module in order:
            table[path.name] = module
    seen_upward = set()
    for name, module in sorted(table.items()):
        text = paths[name].read_text(encoding="utf-8")
        for target in _QUOTED_INCLUDE.findall(text):
            target_module = table.get(target)
            if target_module not in order:
                continue
            if order.index(target_module) <= order.index(module):
                continue
            if (name, target) in KNOWN_UPWARD_INCLUDES:
                seen_upward.add((name, target))
                continue
            check(
                False,
                f"src/{module}/{name} includes {target} ({target_module}): a "
                "module includes only its own headers or those of a module "
                f"before it ({' < '.join(order)}); see docs/ARCHITECTURE.md",
            )
        if module in PORTABLE_MODULES:
            for framework in _APPLE_FRAMEWORK.findall(text):
                check(
                    False,
                    f"src/{module}/{name} includes <{framework}>: only "
                    "platform and app name an Apple framework",
                )
    for name, target in sorted(KNOWN_UPWARD_INCLUDES - seen_upward):
        if target in paths:
            check(
                False,
                f"{name} no longer includes {target}: take the pair out of "
                "KNOWN_UPWARD_INCLUDES",
            )

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: architecture_tests.py SOURCE_ROOT", file=sys.stderr)
        return 2
    root = Path(sys.argv[1]).resolve()
    operations_h = read(root, "src/engine/Operations.h")
    operations_cc = read(root, "src/engine/Operations.cc")
    # FORMAT-2026-09 -- the journal codec lives beside the engine, not in it.
    format_cc = read(root, "src/engine/OperationFormat.cc")
    document_h = read(root, "src/engine/Document.h")
    project_h = read(root, "src/engine/Project.h")
    cli_h = read(root, "src/agent/Cli.h")
    cli_cc = read(root, "src/agent/Cli.cc")
    # STRUCTURE-2026-11 ST4 -- AppDelegate is implemented by main.mm and by
    # its categories (AppDelegate+*.mm), with the helpers they share
    # (WindowSupport.mm): the window's rules read them all.
    gui = "\n".join(
        [read(root, "src/app/main.mm"), read(root, "src/app/WindowSupport.mm")]
        + [
            category.read_text(encoding="utf-8")
            for category in sorted((root / "src").rglob("AppDelegate+*.mm"))
        ]
    )
    # HEADLESS-2026-10 H1 -- the command line is read once, in CliMain.cc,
    # for the app (main.mm) and for the build without a window
    # (HeadlessMain.cc).
    cli_main = read(root, "src/agent/CliMain.cc")
    headless_main = read(root, "src/agent/HeadlessMain.cc")

    # ADOBE-CLI-2026-09 -- a live backend capability must not disappear in
    # the chat worker's adapter by inheriting an unsupported base default.
    live_header = read(root, "src/agent/McpLiveBackend.h")
    # AX1 -- the adapter left the chat panel for its own header: the agent
    # endpoint runs the catalog through it too (McpEndpoint.h).
    chat_source = read(root, "src/app/MainThreadBackend.h")
    override_pattern = re.compile(
        r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?override"
    )
    # Comments are dropped first: a parenthesis in one used to read as the
    # start of a parameter list running into the next declaration.
    live_overrides = set(
        override_pattern.findall(re.sub(r"//.*", "", live_header))
    )
    chat_adapter = between(
        chat_source,
        "class MainThreadBackend : public McpBackend",
        "McpLiveBackend& live_;",
    )
    chat_overrides = set(
        override_pattern.findall(re.sub(r"//.*", "", chat_adapter))
    )
    # The calls that need this path (export, for one) freeze it on
    # the main thread themselves; returning the live path directly from the
    # worker would be a data race.
    required_overrides = (live_overrides - {"ProjectPath"}) | {
        "EndTimelineEdit",
        "ApplyPreparedOperation",
    }
    check(
        required_overrides <= chat_overrides,
        "chat backend drops live capabilities: "
        + ", ".join(sorted(required_overrides - chat_overrides)),
    )

    # CHAT-PARITE -- ROADMAP.md F2.4. The chat panel offers the whole shared
    # catalog, so a capability some tool dispatches to and the live backend
    # leaves to McpBackend's refusal ("this backend cannot ...") is a tool
    # that fails from the panel while it works over MCP. The adapter check
    # above could not see it: it only compares the adapter with the live
    # backend, and both lacked transcription alike.
    backend_header = read(root, "src/agent/McpBackend.h")
    # DECOUPAGE-2026-10 DC2 -- the dispatchers McpTools.cc held now live in
    # the files of the catalog that share McpToolsInternal.h: all are read,
    # or this check would pass on what it no longer sees.
    catalog_files = ["src/agent/McpTools.cc"] + [
        str(path.relative_to(root))
        for path in sorted((root / "src").rglob("Mcp*Tools.cc"))
        if '#include "McpToolsInternal.h"' in path.read_text(encoding="utf-8")
        and path.name != "McpTools.cc"
    ]
    check(
        len(catalog_files) >= 12,
        "the catalog's files were not found: " + ", ".join(catalog_files),
    )
    tools_source = "".join(read(root, name) for name in catalog_files)
    refusing_defaults = backend_refusing_defaults(backend_header)
    check(
        {"TranscribeSources", "CreateProject", "RunAfterRequest"}
        <= refusing_defaults,
        "unable to find McpBackend's refusing defaults",
    )
    dispatched = set(re.findall(r"\bbackend\.([A-Z]\w*)\(", tools_source))
    # Tools of the catalog that still fail from the chat panel. Named here so
    # the list cannot grow unseen; implementing one fails the check below
    # until it is removed. Each is a decision about the app, not plumbing.
    # F2.4 -- create_project is no gap but a refusal of McpLiveBackend's
    # own: the window holds one project, its session lock and its journals
    # for its whole life (IA7), and says how to reach another; open_project
    # answers for that one project (AX1). ingest_media runs the engine's
    # ingest on the window's document since 19 October 2026, and
    # import_resolve_timeline (R10) the import's steps (ResolveImport.h)
    # since 24 October 2026. None is left: a tool that fails from the panel
    # again is named here, with why.
    live_backend_gaps = set()
    missing_live = (refusing_defaults & dispatched) - live_overrides
    check(
        missing_live <= live_backend_gaps,
        "tools of the shared catalog fail from the chat panel, "
        "McpLiveBackend does not implement: "
        + ", ".join(sorted(missing_live - live_backend_gaps)),
    )
    check(
        not (live_backend_gaps & live_overrides),
        "McpLiveBackend now implements a listed gap, remove it from "
        "live_backend_gaps: "
        + ", ".join(sorted(live_backend_gaps & live_overrides)),
    )
    # Transcription lasts minutes: the adapter may read what it needs on the
    # main queue, never run the command there.
    for method in (
        "TranscribeSources",
        "TranscribeTimeline",
        "AlignSourceTranscripts",
        # AX2 -- word alignment: minutes of MFA over the rushes.
        "AlignSourceWords",
        # CHAT-ANALYSES -- decoding a whole rush, or running FFmpeg over up
        # to 64 positions of a timeline, froze the editor just as long.
        "AnalyzeSourceShotQuality",
        "AnalyzeSourceSpeechOnset",
        "AnalyzeSourceAudioBeats",
        # PA3 -- the speaker embedding model over a whole rush's sound.
        "AnalyzeSourceSpeakers",
        "CaptureSourceFrame",
        "CaptureTimelineSheet",
        # IA4 -- an inspection renders a window through the export's graph
        # and may run Whisper over its mix.
        "InspectCut",
        # F2.4 -- an ingest decodes the sound of every rush it reads.
        "IngestMedia",
        # F2.4 (R10) -- the import reads its manifest and copies the Resolve
        # database, several megabytes, before the main queue plans it.
        "ImportResolveTimeline",
    ):
        body = adapter_method_body(chat_adapter, method)
        check(
            "dispatch_sync" not in body and "McpLiveBackend::Run" in body,
            f"the chat adapter must run {method} from its worker, not on "
            "the main queue",
        )
    # CHAT-ANALYSES -- and whatever the form of a method, nothing that decodes
    # or encodes media runs inside a block the adapter sends to the main
    # queue: neither the live backend's own long capabilities nor the
    # functions the older methods (hesitations, timeline audio, exports)
    # call on the worker after their snapshot. VQA5 -- the one capture that
    # belongs to the main queue is the window's own picture
    # (CaptureEditorWindow): AppKit draws only there, and it decodes
    # nothing, redrawing the frames the monitors already hold.
    long_live_call = re.compile(
        r"\blive_\.(Analyze\w+|Transcribe\w+|AlignSourceTranscripts|"
        r"Capture(?!EditorWindow\b)\w+|Export\w+|VerifyExportedFile|"
        r"InspectCut)\s*\("
    )
    long_free_call = re.compile(
        r"\b(McpLiveBackend::Run\w+|Exporter::\w+|VerifyExport|"
        r"ProbeIngest\w*|"
        r"AnalyzeProjectSourceHesitations|AnalyzeTimelineAudio|"
        r"RenderTimelineSheet|RenderCutInspection|CaptureSourceFrame|"
        r"\w+Command)\s*\("
    )
    for match in re.finditer(r"\bdispatch_sync\s*\(", chat_adapter):
        close = matching_close(chat_adapter, match.end() - 1, "(", ")")
        block = chat_adapter[match.end() : close]
        on_main = {m.group(1) for m in long_live_call.finditer(block)} | {
            m.group(1) for m in long_free_call.finditer(block)
        }
        check(
            not on_main,
            "the chat adapter decodes media on the main queue: "
            + ", ".join(sorted(on_main)),
        )

    # PRD16 -- docs/roadmap/PRODUIT-2026-10.md. One default export, decided
    # by the engine (Exporter::DefaultSettings). A surface that picked its own
    # encoder when its caller named none is how `--export` came to write HEVC
    # Main10 and export_timeline H.264 High from the same project. The
    # window's export dialog opens on Exporter::DefaultPreset (checked below).
    check(
        "Exporter::DefaultSettings(" in cli_cc,
        "--export no longer takes its default from the engine",
    )
    check(
        "Exporter::DefaultSettings(" in tools_source,
        "the catalog's exports no longer take their default from the engine",
    )
    export_block = between(
        cli_main,
        'std::string(argv[1]) == "--export")',
        "if (argc > 2 || (argc == 2 && argv[1][0] == '-'))",
    )
    check(
        re.search(r'std::string\s+codec\s*=\s*"', export_block) is None
        and "HevcMain10Preset" not in export_block
        and "H264Preset" not in export_block,
        "--export chooses a default encoder of its own again",
    )
    check(
        re.search(
            r'ReadString\(\s*\w+,\s*"codec",[^;]*?,\s*false,\s*"(?:h264|hevc)"',
            tools_source,
        )
        is None,
        "an export tool of the catalog chooses a default codec of its own "
        "again",
    )
    check(
        "Exporter::PresetIndex(" in gui and "Exporter::DefaultPreset(" in gui,
        "the window's export dialog no longer opens on the engine's default "
        "preset",
    )

    # HEADLESS-2026-10 H1 -- neither executable reads a command of its own:
    # a command dispatched in one only is a command the other lacks. The
    # window test's two flags are read by its own entry
    # (tests/ui_smoke/UiSmokeMain.mm, STRUCTURE-2026-11 ST1).
    command_test = re.compile(r'std::string\(argv\[1\]\)\s*==\s*"(--[a-z0-9-]+)"')
    gui_commands = set(command_test.findall(gui))
    check(
        not gui_commands,
        "main.mm reads commands of its own again: "
        + ", ".join(sorted(gui_commands)),
    )
    check(
        "RunCliCommand(" in gui and "RunCliCommand(" in headless_main,
        "an executable no longer hands its command line to RunCliCommand",
    )
    check(
        re.search(r'==\s*"--', headless_main) is None,
        "HeadlessMain.cc reads commands of its own again",
    )

    timeline_operations = variant_members(operations_h, "Operation")
    project_operations = variant_members(operations_h, "ProjectOperation")
    all_operations = timeline_operations | project_operations
    check(bool(timeline_operations), "timeline Operation variant is empty")
    check(bool(project_operations), "ProjectOperation variant is empty")

    # FORMAT-2026-09 -- the codec is derived from the field tables of
    # OperationFormat.h, so an operation is covered by having one. The
    # dispatch itself is checked by the compiler: an alternative of the
    # variant without a table does not build.
    format_h = read(root, "src/engine/OperationFormat.h")
    tabled = set(
        re.findall(
            r"struct\s+FieldTable<([A-Za-z0-9_:]+)>\s*\{\s*"
            r"static\s+constexpr\s+std::string_view\s+kType",
            format_h,
        )
    )
    for operation in sorted(all_operations):
        check(
            operation in tabled,
            f"{operation} is registered but has no FieldTable",
        )
    check(
        tabled <= all_operations,
        "a FieldTable carries a type tag for something that is not a "
        "registered operation: " + ", ".join(sorted(tabled - all_operations)),
    )
    for name in (
        "std::string SerializeOperation(",
        "bool DeserializeOperation(",
        "std::string SerializeProjectOperation(",
        "bool DeserializeProjectOperation(",
    ):
        check(name in format_cc, f"{name} no longer lives in OperationFormat.cc")
    check(
        "std::visit" in format_cc and "ReadOperation(" in format_cc,
        "the journal codec no longer dispatches over the whole variant",
    )

    # FORMAT-2026-09 F6 -- the document format is derived from the tables of
    # DocumentFormat.h. Document.cc keeps what a document is -- validation
    # and lookups -- and no longer spells a single JSON key.
    document_cc = read(root, "src/engine/Document.cc")
    document_format_cc = read(root, "src/engine/DocumentFormat.cc")
    for name in (
        "std::string Document::SaveToString(",
        "bool Document::LoadFromString(",
        "std::string Project::SaveToString(",
        "bool Project::LoadFromString(",
    ):
        check(
            name in document_format_cc,
            f"{name} no longer lives in DocumentFormat.cc",
        )
    check(
        '\\"' not in document_cc,
        "Document.cc spells JSON again: the document format belongs to "
        "DocumentFormat.cc",
    )
    # Both format headers are internal to their codec and its tests: the
    # public API stays Operations.h, Document.h, Project.h and DocumentJson.h.
    for header, codec in (
        ("OperationFormat.h", "OperationFormat.cc"),
        ("DocumentFormat.h", "DocumentFormat.cc"),
    ):
        for source in src_files(root):
            if source.name in (header, codec) or source.suffix not in (
                ".h",
                ".cc",
                ".mm",
            ):
                continue
            check(
                f'#include "{header}"' not in source.read_text(),
                f"src/{source.name} includes the internal {header}",
            )

    # Public non-const lookup methods are used by the operation engine. CommitDocument
    # is the explicit boundary that reintegrates an already journaled timeline
    # snapshot. Every other new public mutator must name a registered counterpart.
    query_accessors = {
        "Document": {
            "FindSource",
            "FindLibraryMedia",
            "FindBin",
        "FindMarker",
        "FindTransition",
        "FindCaptionStyle",
        "FindMulticamGroup",
            "FindTrack",
            "FindClip",
            "FindTrackForClip",
            # MOTION-2026-09 M3 -- read by the motion operations.
            "FindComposition",
        },
        "Project": {"ActiveTimeline", "FindTimeline"},
    }
    commit_boundaries = {"Project": {"CommitDocument", "CommitActiveDocument"}}
    public_mutators: dict[str, set[str]] = {"Document": set(), "Project": set()}
    mutator_name = re.compile(
        r"^(Add|Remove|Set|Update|Rename|Move|Activate|Relink|Delete|Clear|"
        r"Insert|Trim|Split|Join|Detach)(.*)$"
    )
    for class_name, header in (("Document", document_h), ("Project", project_h)):
        for method, is_static, is_const in class_public_methods(header, class_name):
            if (
                is_static
                or is_const
                or method in {class_name, f"~{class_name}"}
                or method in query_accessors[class_name]
                or method in commit_boundaries.get(class_name, set())
            ):
                continue
            public_mutators[class_name].add(method)
            candidates = {
                f"{method}Operation",
                f"{method.removeprefix(class_name)}Operation",
                f"{class_name}{method}Operation",
            }
            parts = mutator_name.match(method)
            if parts:
                candidates.add(
                    f"{parts.group(1)}{class_name}{parts.group(2)}Operation"
                )
            check(
                bool(candidates & all_operations),
                f"public mutator {class_name}::{method} has no registered Operation counterpart",
            )

    for method in sorted(public_mutators["Document"] | public_mutators["Project"]):
        check(
            re.search(rf"\.{re.escape(method)}\s*\(", gui) is None,
            f"GUI calls public mutator {method} directly instead of applying its Operation",
        )
    direct_project_writes = re.findall(
        r"self\.state->project\.([A-Za-z_]\w*)\s*(?:"
        r"(?:\+|-|\*|/)?=(?!=)|\.push_back\s*\(|\.erase\s*\(|\.clear\s*\()",
        gui,
    )
    check(
        not direct_project_writes,
        "GUI writes Project fields directly: "
        + ", ".join(sorted(set(direct_project_writes))),
    )

    gui_operation_tokens = set(
        re.findall(r"\b[A-Z][A-Za-z0-9_]*Operation\b", gui)
    )
    # Cocoa types and operation-building helper function names are not model
    # operation structs. Keeping this list narrow makes a new GUI-only model
    # operation fail until it joins one of the variants.
    gui_non_model_tokens = {
        "ApplyOperation",
        "GapDeleteOperation",
        "NSCollectionViewDropOperation",
        "NSDragOperation",
        "ProjectOperation",
        "RedoProjectOperation",
        "TimelineCutOperation",
        "TimelineRangeDeleteOperation",
        "TimelineSourceEditOperation",
        "UndoProjectOperation",
    }
    unregistered_gui_operations = (
        gui_operation_tokens - all_operations - gui_non_model_tokens
    )
    check(
        not unregistered_gui_operations,
        "GUI references operation-like types outside the model variants: "
        + ", ".join(sorted(unregistered_gui_operations)),
    )

    gui_timeline_operations = gui_operation_tokens & timeline_operations
    gui_project_operations = gui_operation_tokens & project_operations
    if gui_timeline_operations:
        check(
            "ApplyOperationCommand" in cli_h
            and "int ApplyOperationCommand(" in cli_cc
            and "DeserializeOperation(" in cli_cc
            and '"--apply-op"' in cli_main,
            "GUI timeline operations have no generic headless CLI surface",
        )
    if gui_project_operations:
        check(
            "ApplyProjectOperationCommand" in cli_h
            and "int ApplyProjectOperationCommand(" in cli_cc
            and "DeserializeProjectOperation(" in cli_cc
            and '"--apply-project-op"' in cli_main,
            "GUI project operations have no generic headless CLI surface",
        )
    project_log_variables = set(
        re.findall(r"\bProjectEditLog\s+([A-Za-z_]\w*)", gui)
    )
    check(
        any(re.search(rf"\b{re.escape(name)}\.Apply\s*\(", gui)
            for name in project_log_variables),
        "GUI project mutations no longer visibly pass through ProjectEditLog::Apply",
    )

    # VQA1 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. NSString reads `%s` in
    # the process C-string encoding, while every engine string is UTF-8: an
    # accented timeline name showed as "renomm√©e". Text shown by AppKit goes
    # through `%@` and an explicit UTF-8 conversion.
    ns_format = re.compile(
        r'(?:stringWithFormat|initWithFormat|appendFormat|'
        r'localizedStringWithFormat):\s*@"(?:[^"\\]|\\.)*"'
        r'(?:\s*@?"(?:[^"\\]|\\.)*")*'
    )
    for source in sorted((root / "src").rglob("*.mm")):
        text = source.read_text(encoding="utf-8")
        for match in ns_format.finditer(text):
            if "%s" in match.group(0):
                line = text.count("\n", 0, match.start()) + 1
                check(
                    False,
                    f"{source.name}:{line}: NSString format uses %s, which "
                    "does not read UTF-8",
                )

    # PRD1 -- docs/roadmap/PRODUIT-2026-10.md. One launcher, one path to
    # FFmpeg. A thirteenth copy of the spawn loop, or a new default that
    # calls the binary by its name, is how the twelve copies and the
    # eighteen defaults came to be.
    spawners = {
        "ProcessLauncher.cc",
        # Out of PRD1's scope: a login shell, After Effects, Python.
        "AcpProcess.cc",
        "AfterBridge.cc",
        "MfaAlignment.cc",
    }
    spawn_call = re.compile(r"\bposix_spawnp?\s*\(")
    for source in src_files(root):
        if source.suffix not in {".cc", ".h", ".mm"}:
            continue
        code = "\n".join(
            line
            for line in source.read_text(encoding="utf-8").splitlines()
            if not line.lstrip().startswith("//")
        )
        check(
            source.name in spawners or not spawn_call.search(code),
            f"{source.name} starts a process itself instead of going "
            "through ProcessLauncher.h",
        )
        # FfmpegCapabilities.h holds the word once too: the key an answer
        # carries the diagnosis under (PRD2), which is not a binary's name.
        check(
            source.name in {"FfmpegBinary.cc", "FfmpegCapabilities.h"}
            or '"ffmpeg"' not in code,
            f'{source.name} calls FFmpeg by the bare name "ffmpeg" instead '
            "of leaving the binary to FfmpegBinary.h",
        )

    for write in document_writes(root):
        check(
            False,
            f"{write} writes a Document outside an operation (PHILOSOPHY.md, "
            "principle 2): express it as an operation of Operations.h, or "
            f"mark a throwaway view with '{DOCUMENT_VIEW_MARKER}'",
        )

    for name in sorted(cli_header_includers(root) - CLI_HEADER_INCLUDERS):
        check(
            False,
            f"{name} includes Cli.h: the model takes the envelope from "
            "CommandEnvelope.h and a package's files from PackageFiles.h; a "
            "new command file is added to CLI_HEADER_INCLUDERS with why",
        )

    # ARCHI-2026-11 AR3 -- a block under CUTMACHINE_UI_SMOKE_TEST in a .cc
    # file was dead code: the display list's PRD24 switch moved out of
    # main.mm that way and stopped switching anything. STRUCTURE-2026-11 ST1
    # -- and the window test no longer compiles the app's sources at all: it
    # links the product's objects, and observes or answers through the hooks
    # its entry installs (AppTestHooks.h). Nothing under src/ names the
    # macro, or the run's state (gUiSmoke..., gUiPlayProbe...), which lives
    # in tests/ui_smoke/.
    test_state = re.compile(r"\bCUTMACHINE_UI_SMOKE_TEST\b|\bgUi(?:Smoke|PlayProbe)\w*")
    for source in src_files(root):
        if source.suffix not in {".cc", ".mm", ".h", ".metal"}:
            continue
        found = test_state.search(source.read_text(encoding="utf-8"))
        check(
            found is None,
            f"{source.name} names {found.group(0) if found else ''}, the "
            "window test's: the product's objects are the test's, and what "
            "the run observes or answers goes through AppTestHooks.h",
        )
    check(
        not any((root / "src").rglob("UiSmokeHarness.mm"))
        and (root / "tests" / "ui_smoke" / "UiSmokeHarness.mm").exists(),
        "the window test's harness is test code: tests/ui_smoke/, not src/",
    )

    module_layering(root)

    # PRD23 -- a fourth dependency added without its notice does not pass.
    third_party_count = third_party_dependencies(root)

    if failures:
        for failure in failures:
            print(f"ARCHITECTURE FAILURE: {failure}", file=sys.stderr)
        return 1
    print(
        "architecture guard passed: "
        f"{len(timeline_operations)} timeline operations, "
        f"{len(project_operations)} project operations, "
        f"{len(gui_timeline_operations | gui_project_operations)} GUI operation types, "
        f"{third_party_count} third-party dependencies"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
