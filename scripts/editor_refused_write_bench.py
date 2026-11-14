#!/usr/bin/env python3
# SAVING_ROADMAP.md -- a write refused under the editor (S3, 24 October 2026).
# scripts/editor_forced_stop_bench.py kills the editor in the middle of its
# edits; this one leaves it alive and makes its commits fail, then checks
# what an agent relayed to the window is told and what the window, the
# package and the journal hold afterwards:
#
#   - an edit, an undo, a redo, the fold of an intervention and a project
#     edit sent through the editor's socket while its package refuses
#     writes are refused by name (IoError), never answered `ok`;
#   - what the editor shows (`describe`, its history included) is what it
#     showed before the call, and is what the files say;
#   - the package's files and journals are byte for byte what they were;
#   - no transaction marker, .tmp or .bak is left behind;
#   - once the storage accepts writes again, the same call succeeds.
#
# Three ways to refuse a write: the package folder made read-only (the
# autosave, written first, fails), its Timelines/ folder made read-only
# (the autosave is written and the commit fails after it), and a full disk
# -- the project on a small disk image (hdiutil, HFS+) filled to the last
# byte once the editor runs. --no-disk-image skips the last one.
#
#   scripts/editor_refused_write_bench.py <folder> [--app build/CUTMACHINE.app]
#       [--binary build/cutmachine] [--no-disk-image] [--keep]
#
# The editor is launched and driven as editor_forced_stop_bench.py does it:
# the project file as its only argument, then the socket it announces in
# <project>.lock/mcp-endpoint (AX1); no window needs to be seen. Its
# defaults domain (com.cutmachine.editor, where it notes recent projects)
# is exported before the first launch and imported back at the end, and the
# script refuses to start while an editor runs. Everything is created under
# <folder> and removed at the end unless --keep; the disk image is detached.
import argparse
import glob
import hashlib
import json
import os
import select
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("folder")
here = os.path.dirname(os.path.abspath(__file__))
parser.add_argument("--app", default=os.path.join(here, "..", "build",
                                                  "CUTMACHINE.app"))
parser.add_argument("--binary", default=os.path.join(here, "..", "build",
                                                     "cutmachine"))
parser.add_argument("--no-disk-image", action="store_true")
parser.add_argument("--keep", action="store_true")
options = parser.parse_args()
binary = os.path.abspath(options.binary)
app_binary = os.path.join(os.path.abspath(options.app), "Contents", "MacOS",
                          "CUTMACHINE")
if subprocess.run(["pgrep", "-x", "CUTMACHINE"],
                  capture_output=True).returncode == 0:
    sys.exit("an editor is running: quit it first (its defaults are saved "
             "and restored around this script)")
folder = os.path.join(os.path.abspath(options.folder),
                      f"cutmachine-editor-refused-write-{os.getpid()}")
os.makedirs(folder)
DOMAIN = "com.cutmachine.editor"
defaults_backup = os.path.join(folder, "defaults.plist")
had_defaults = subprocess.run(
    ["defaults", "export", DOMAIN, defaults_backup],
    capture_output=True).returncode == 0

env = dict(os.environ,
           CUTMACHINE_PROJECT_ARCHIVE="off",
           CUTMACHINE_LOG_DIR=os.path.join(folder, "logs"),
           CUTMACHINE_MOTION_LIBRARY=os.path.join(folder, "motion"),
           CUTMACHINE_MODELS="off",
           CUTMACHINE_RECOVERY_ANSWER="decline")
app_log = open(os.path.join(folder, "editor.log"), "a")
sockets_before = set(glob.glob(f"/tmp/cutmachine-{os.getuid()}/*.sock"))


class Package:
    """One throwaway project, its files and the CLI that sets it up."""

    def __init__(self, parent, name):
        created = subprocess.run(
            [binary, "--create-project",
             os.path.join(parent, name + ".cutmachine-project"), name,
             "--format", "1080x1920@25"], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, check=True, env=env)
        project = json.loads(created.stdout)
        self.path = project["path"]
        self.video = next(track["id"] for track in project["tracks"]
                          if track["kind"] == "video")
        self.folder = os.path.dirname(self.path)
        self.endpoint = os.path.join(self.path + ".lock", "mcp-endpoint")
        added = self.cli("add_motion_composition", {
            "name": "base", "definition": {
                "width": 1080, "height": 1920, "fps": 25, "duration": 75,
                "root": {"type": "fill",
                         "style": {"backgroundColor": "#000000"}}}})
        if not added.get("ok"):
            sys.exit(f"setup failed: {added}")
        self.composition = added["result"]["composition"]["id"]

    def cli(self, tool, args):
        """Only while no editor holds the project."""
        out = subprocess.run(
            [binary, "--call-tool", self.path, tool, json.dumps(args)],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            timeout=120, env=env)
        try:
            return json.loads(out.stdout)
        except json.JSONDecodeError:
            return {"ok": False, "error": "unparsable",
                    "detail": out.stdout[:300] + out.stderr[:300]}

    def files(self):
        """Every file of the package but the session lock, by content."""
        found = {}
        for root, folders, names in os.walk(self.folder):
            folders[:] = [name for name in folders
                          if not name.endswith(".lock")]
            for name in names:
                path = os.path.join(root, name)
                with open(path, "rb") as handle:
                    found[os.path.relpath(path, self.folder)] = (
                        hashlib.sha256(handle.read()).hexdigest())
        return found

    def disk_state(self):
        with open(self.path) as handle:
            stored = json.load(handle)
        active = stored["active_timeline_id"]
        with open(os.path.join(self.folder, "Timelines",
                               active + ".json")) as handle:
            timeline = json.load(handle)
        tracks = timeline.get("tracks")
        if tracks is None:
            tracks = timeline.get("sequence", {}).get("tracks", [])
        clips = sum(len(track.get("clips", [])) for track in tracks
                    if track.get("id") == self.video)
        return (clips, len(stored["compositions"]), len(stored["timelines"]))

    def leftovers(self):
        found = []
        for pattern in ("*.cutmachine-*.tmp", "*.cutmachine-*.bak",
                        "Timelines/*.cutmachine-*.tmp",
                        "Timelines/*.cutmachine-*.bak",
                        "*.autosave.*.tmp"):
            found += [os.path.relpath(path, self.folder)
                      for path in glob.glob(os.path.join(self.folder,
                                                         pattern))]
        if os.path.exists(os.path.join(self.folder,
                                       ".cutmachine-transaction")):
            found.append(".cutmachine-transaction")
        return found

    def autosave(self):
        return os.path.exists(self.path + ".autosave")


class Editor:
    """One launch of the editor and its MCP socket."""

    def __init__(self, package):
        self.package = package
        app_log.write(f"--- launch on {package.path}\n")
        app_log.flush()
        self.started = time.monotonic()
        self.process = subprocess.Popen(
            [app_binary, package.path], env=env, stdin=subprocess.DEVNULL,
            stdout=app_log, stderr=app_log)
        self.socket = None
        self.buffer = b""
        self.next_id = 1

    def wait_ready(self, timeout=30.0):
        path = None
        while not path:
            if self.process.poll() is not None:
                return f"the editor exited ({self.process.returncode})"
            if time.monotonic() - self.started > timeout:
                return "no endpoint: the editor waits on something"
            try:
                with open(self.package.endpoint) as handle:
                    path = handle.read().strip()
            except OSError:
                time.sleep(0.01)
        for _ in range(200):
            try:
                self.socket = socket.socket(socket.AF_UNIX,
                                            socket.SOCK_STREAM)
                self.socket.connect(path)
                break
            except OSError:
                self.socket = None
                time.sleep(0.01)
        if self.socket is None:
            return "the announced socket does not answer"
        answer = self.request("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "editor-refused-write", "version": "1"}})
        if answer is None or "result" not in answer:
            return f"initialize failed: {answer}"
        self.socket.sendall((json.dumps(
            {"jsonrpc": "2.0", "method": "notifications/initialized"}) +
            "\n").encode())
        return None

    def request(self, method, params, timeout=60.0):
        identifier = self.next_id
        self.next_id += 1
        self.socket.sendall((json.dumps(
            {"jsonrpc": "2.0", "id": identifier, "method": method,
             "params": params}) + "\n").encode())
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.socket], [], [], 0.05)
            if ready:
                chunk = self.socket.recv(1 << 20)
                if not chunk:
                    return None
                self.buffer += chunk
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if line.strip():
                    message = json.loads(line)
                    if message.get("id") == identifier:
                        return message
        return None

    def call(self, tool, args):
        answer = self.request("tools/call", {"name": tool,
                                             "arguments": args})
        if answer is None or "result" not in answer:
            return {"ok": False, "error": str(answer)[:300]}
        result = answer["result"]
        text = result["content"][0]["text"] if result.get("content") else ""
        try:
            body = json.loads(text)
        except ValueError:
            body = {"text": text[:300]}
        if result.get("isError"):
            return {"ok": False, "error": body}
        return {"ok": True, "result": body}

    def state(self):
        """What the editor shows: the clips of V1, the compositions, the
        timelines, and its journal's cursor."""
        described = self.call("describe", {})
        history = self.call("describe", {"only": ["history"]})
        compositions = self.call("list_motion_compositions", {})
        timelines = self.call("list_timelines", {})
        if not all(answer.get("ok") for answer in
                   (described, history, compositions, timelines)):
            return None
        clips = sum(1 for track in described["result"]["timeline"]["tracks"]
                    if track["id"] == self.package.video
                    for item in track["items"] if item.get("type") == "clip")
        journal = history["result"].get("history", {})
        return {"shown": (clips, len(compositions["result"]["compositions"]),
                          len(timelines["result"]["timelines"])),
                "journal": (journal.get("total"), journal.get("applied"))}

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.send_signal(signal.SIGKILL)
                self.process.wait()
        if self.socket is not None:
            self.socket.close()


def error_name(answer):
    error = answer.get("error")
    if isinstance(error, dict):
        return error.get("error") or error.get("code") or str(error)[:120]
    return str(error)[:120]


results = []
failures = []


def refused(label, editor, call, refuse, allow, expect_change=True):
    """Runs `call` while `refuse()` holds, then again after `allow()`."""
    package = editor.package
    before = editor.state()
    files_before = package.files()
    if before is None or before["shown"] != package.disk_state():
        failures.append((label, "editor and files differ before", before,
                         package.disk_state()))
        return
    refuse()
    try:
        answer = call()
        after = editor.state()
        files_after = package.files()
        leftovers = package.leftovers()
        autosave = package.autosave()
    finally:
        allow()
    on_disk = package.disk_state()
    row = {"case": label,
           "answer": "ok" if answer.get("ok") else error_name(answer),
           "detail": (json.dumps(answer.get("error"))[:240]
                      if not answer.get("ok") else ""),
           "before": before, "after": after, "on_disk": on_disk,
           "files_unchanged": files_after == files_before,
           "autosave_left": autosave, "leftovers": leftovers}
    if answer.get("ok"):
        failures.append((label, "answered ok while the write was refused",
                         before, after, on_disk))
    elif error_name(answer) != "IoError":
        failures.append((label, "refused, but not as IoError",
                         answer.get("error")))
    if after != before:
        failures.append((label, "the editor's state moved", before, after))
    if after is not None and after["shown"] != on_disk:
        failures.append((label, "editor and files differ after", after,
                         on_disk))
    if files_after != files_before:
        changed = sorted(name for name in set(files_before) |
                         set(files_after)
                         if files_before.get(name) != files_after.get(name))
        if changed != ["project.cutmachine.json.autosave"]:
            failures.append((label, "package files changed", changed))
    if leftovers:
        failures.append((label, "leftovers", leftovers))
    # The storage accepts writes again: the same call goes through.
    retried = call()
    again = editor.state()
    row["retry"] = "ok" if retried.get("ok") else error_name(retried)
    row["retry_state"] = again
    row["autosave_after_retry"] = package.autosave()
    if not retried.get("ok"):
        failures.append((label, "the call fails once writes are accepted",
                         retried.get("error")))
    elif expect_change and again == before:
        failures.append((label, "the retried call changed nothing"))
    if again is not None and again["shown"] != package.disk_state():
        failures.append((label, "editor and files differ after the retry",
                         again, package.disk_state()))
    results.append(row)


def chmod_refuse(path):
    mode = stat.S_IMODE(os.stat(path).st_mode)
    return (lambda: os.chmod(path, mode & ~0o222),
            lambda: os.chmod(path, mode))


def scenario_read_only(package):
    editor = Editor(package)
    problem = editor.wait_ready()
    if problem:
        failures.append(("read-only", "launch", problem))
        editor.stop()
        return
    def insert():
        clips = editor.state()["shown"][0]
        return editor.call("insert_motion_clip", {
            "track_id": package.video,
            "composition_id": package.composition,
            "timeline_in": {"value": clips * 75, "rate": 25}})

    targets = ((package.folder, "package"),
               (os.path.join(package.folder, "Timelines"), "Timelines"))
    try:
        for target, where in targets:
            refuse, allow = chmod_refuse(target)
            refused(f"edit, {where} read-only", editor, insert, refuse,
                    allow)
            refused(f"undo, {where} read-only", editor,
                    lambda: editor.call("undo", {}), refuse, allow)
            refused(f"redo, {where} read-only", editor,
                    lambda: editor.call("redo", {}), refuse, allow)
            # Two edits in an intervention, folded while writes are refused.
            # A fold writes the journal alone, beside Timelines/: only the
            # package folder refuses it.
            if where != "package":
                continue
            begun = editor.call("begin_intervention", {})
            mark = begun.get("result", {}).get("intervention")
            insert()
            insert()
            refused(f"end_intervention, {where} read-only", editor,
                    lambda: editor.call("end_intervention", {
                        "intervention": mark,
                        "label": "Agent : deux plans"}),
                    refuse, allow)
        # Last: a created timeline becomes the one the window edits.
        for target, where in targets:
            refuse, allow = chmod_refuse(target)
            refused(f"add_timeline, {where} read-only", editor,
                    lambda: editor.call("add_timeline", {
                        "name": "T" + where, "width": 1080,
                        "height": 1920,
                        "frame_rate": {"num": 25, "den": 1}}),
                    refuse, allow)
    finally:
        editor.stop()


def scenario_disk_full():
    image = os.path.join(folder, "full.dmg")
    volume = os.path.join(folder, "volume")
    os.makedirs(volume)
    subprocess.run(["hdiutil", "create", "-size", "24m", "-fs", "HFS+",
                    "-volname", "CMFULL", image], check=True,
                   capture_output=True)
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-noverify",
                    "-mountpoint", volume, image], check=True,
                   capture_output=True)
    # Spotlight would hold the filler open, and its space with it.
    open(os.path.join(volume, ".metadata_never_index"), "w").close()
    filler = os.path.join(volume, "filler")
    try:
        package = Package(volume, "Plein")
        editor = Editor(package)
        problem = editor.wait_ready()
        if problem:
            failures.append(("disk full", "launch", problem))
            editor.stop()
            return

        def fill():
            # Unbuffered, down to single bytes: nothing is left to write.
            descriptor = os.open(filler, os.O_WRONLY | os.O_CREAT, 0o644)
            try:
                size = 1 << 20
                while size:
                    try:
                        os.write(descriptor, b"\0" * size)
                    except OSError:
                        size //= 2
            finally:
                os.close(descriptor)

        def free():
            try:
                os.remove(filler)
            except OSError:
                pass
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                usage = os.statvfs(volume)
                if usage.f_bavail * usage.f_frsize > (8 << 20):
                    break
                time.sleep(0.1)

        def insert():
            clips = editor.state()["shown"][0]
            return editor.call("insert_motion_clip", {
                "track_id": package.video,
                "composition_id": package.composition,
                "timeline_in": {"value": clips * 75, "rate": 25}})

        try:
            refused("edit, disk full", editor, insert, fill, free)
            refused("undo, disk full", editor,
                    lambda: editor.call("undo", {}), fill, free)
        finally:
            editor.stop()
    finally:
        try:
            os.remove(filler)
        except OSError:
            pass
        for attempt in range(10):
            if subprocess.run(["hdiutil", "detach", volume] +
                              (["-force"] if attempt == 9 else []),
                              capture_output=True).returncode == 0:
                break
            time.sleep(0.5)


try:
    scenario_read_only(Package(folder, "Refus"))
    if not options.no_disk_image:
        scenario_disk_full()
finally:
    if had_defaults:
        subprocess.run(["defaults", "import", DOMAIN, defaults_backup])
    else:
        subprocess.run(["defaults", "delete", DOMAIN], capture_output=True)

for row in results:
    print(json.dumps(row))
sockets_left = sorted(set(glob.glob(f"/tmp/cutmachine-{os.getuid()}/*.sock"))
                      - sockets_before)
print(json.dumps({"cases": len(results), "failures": len(failures),
                  "sockets_left": len(sockets_left)}))
for failure in failures:
    print("  ", failure)
if not options.keep:
    shutil.rmtree(folder, ignore_errors=True)
    for path in sockets_left:
        try:
            os.remove(path)
        except OSError:
            pass
sys.exit(1 if failures else 0)
