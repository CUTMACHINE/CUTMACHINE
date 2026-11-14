#!/usr/bin/env python3
# SAVING_ROADMAP.md -- the forced-stop validation, for the editor itself.
# scripts/forced_stop_bench.py kills CLI writers; this kills the app in the
# middle of edits and checks what its own save path -- the autosave written
# before each commit (S1) and the commit of a candidate state (S3,
# -commitProjectCandidate:) -- leaves for the next launch:
#
#   - the editor opens again, without waiting on a dialog;
#   - an autosave it offers is recovered (or declined) whole, and what it
#     holds is the state before or after the killed edit;
#   - each edit is whole or absent, and an edit the editor answered is
#     never lost;
#   - what the editor shows after reopening is what the files say;
#   - no transaction marker, no .tmp/.bak and no autosave temporary
#     survive a reopening (an editor killed inside WriteAutosave leaves
#     one; the next editor removes it, ProjectRecovery::
#     DiscardAutosaveLeftovers);
#   - undo and redo still work, through the editor.
#
#   scripts/editor_forced_stop_bench.py <folder> [--iterations N]
#       [--window LOW HIGH] [--seed S] [--app build/CUTMACHINE.app]
#       [--binary build/cutmachine] [--keep]
#
# The editor needs no key or visible window for any of this: it is launched
# on the project file (its only argument) and driven through the socket it
# announces in <project>.lock/mcp-endpoint (AX1), the same path an agent
# relayed to the window takes. It runs with the screen locked. One step
# would wait for a person: the recovery alert. The editor answers it from
# CUTMACHINE_RECOVERY_ANSWER (recover|decline), set per launch here;
# "decline" is used for one offer in three.
#
# Each iteration launches the editor, checks the previous kill, sends one
# edit (alternating, as the CLI bench does, a motion clip, a composition and
# a timeline) and kills the editor with SIGKILL: at a time drawn in
# --window after the request, when its autosave appears, or when the
# transaction marker appears -- one in two, one in four, one in four.
#
# The editor records recent projects in its defaults domain
# (com.cutmachine.editor): the domain is exported before the first launch
# and imported back at the end. The bench refuses to start while an editor
# runs. The project is created under <folder> and removed at the end
# unless --keep.
import argparse
import glob
import json
import os
import random
import select
import shutil
import signal
import socket
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("folder")
parser.add_argument("--iterations", type=int, default=150)
parser.add_argument("--window", type=float, nargs=2, default=(0.0, 0.15))
parser.add_argument("--seed", type=int, default=20261024)
here = os.path.dirname(os.path.abspath(__file__))
parser.add_argument("--app", default=os.path.join(here, "..", "build",
                                                  "CUTMACHINE.app"))
parser.add_argument("--binary", default=os.path.join(here, "..", "build",
                                                     "cutmachine"))
parser.add_argument("--keep", action="store_true")
options = parser.parse_args()
random.seed(options.seed)
binary = os.path.abspath(options.binary)
app_binary = os.path.join(os.path.abspath(options.app), "Contents", "MacOS",
                          "CUTMACHINE")
if subprocess.run(["pgrep", "-x", "CUTMACHINE"],
                  capture_output=True).returncode == 0:
    sys.exit("an editor is running: quit it first (its defaults are saved "
             "and restored around the bench)")
folder = os.path.join(os.path.abspath(options.folder),
                      f"cutmachine-editor-forced-stop-{os.getpid()}")
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
           CUTMACHINE_MODELS="off")
created = subprocess.run(
    [binary, "--create-project",
     os.path.join(folder, "Arret.cutmachine-project"), "Arret", "--format",
     "1080x1920@25"], stdin=subprocess.DEVNULL, capture_output=True,
    text=True, check=True, env=env)
project = json.loads(created.stdout)
P = project["path"]
V1 = next(track["id"] for track in project["tracks"]
          if track["kind"] == "video")
PKG = os.path.dirname(P)
AUTOSAVE = P + ".autosave"
MARKER = os.path.join(PKG, ".cutmachine-transaction")
ENDPOINT = os.path.join(P + ".lock", "mcp-endpoint")
app_log = open(os.path.join(folder, "editor.log"), "a")


def cli(tool, args):
    """Only while no editor holds the project."""
    out = subprocess.run([binary, "--call-tool", P, tool, json.dumps(args)],
                         stdin=subprocess.DEVNULL, capture_output=True,
                         text=True, timeout=120, env=env)
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return {"ok": False, "error": "unparsable",
                "detail": out.stdout[:300] + out.stderr[:300]}


class Editor:
    """One launch of the editor and its MCP socket."""

    def __init__(self, answer):
        launch_env = dict(env, CUTMACHINE_RECOVERY_ANSWER=answer)
        # A killed editor leaves its announcement behind: the new one is
        # ready when it has written its own.
        try:
            with open(ENDPOINT) as handle:
                self.stale_endpoint = handle.read().strip()
        except OSError:
            self.stale_endpoint = None
        app_log.write(f"--- launch, recovery answer {answer}\n")
        app_log.flush()
        self.started = time.monotonic()
        self.process = subprocess.Popen(
            [app_binary, P], env=launch_env, stdin=subprocess.DEVNULL,
            stdout=app_log, stderr=app_log)
        self.socket = None
        self.buffer = b""
        self.next_id = 1
        self.ready_after = None

    def announced(self):
        try:
            with open(ENDPOINT) as handle:
                path = handle.read().strip()
        except OSError:
            return None
        return path if path and path != self.stale_endpoint else None

    def wait_ready(self, timeout=30.0):
        while self.announced() is None:
            if self.process.poll() is not None:
                return f"the editor exited ({self.process.returncode})"
            if time.monotonic() - self.started > timeout:
                return "no endpoint: the editor waits on something"
            time.sleep(0.005)
        self.ready_after = time.monotonic() - self.started
        for _ in range(200):
            try:
                self.socket = socket.socket(socket.AF_UNIX,
                                            socket.SOCK_STREAM)
                self.socket.connect(self.announced())
                break
            except (OSError, TypeError):
                self.socket = None
                time.sleep(0.01)
        if self.socket is None:
            return "the announced socket does not answer"
        answer = self.request("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "editor-forced-stop", "version": "1"}})
        if answer is None or "result" not in answer:
            return f"initialize failed: {answer}"
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return None

    def send(self, message):
        self.socket.sendall((json.dumps(message) + "\n").encode())

    def pump(self, timeout):
        """Reads what has arrived; returns the complete messages."""
        messages = []
        ready, _, _ = select.select([self.socket], [], [], timeout)
        if ready:
            try:
                chunk = self.socket.recv(1 << 20)
            except OSError:
                chunk = b""
            if not chunk:
                raise ConnectionError("the editor closed its socket")
            self.buffer += chunk
        while b"\n" in self.buffer:
            line, self.buffer = self.buffer.split(b"\n", 1)
            if line.strip():
                messages.append(json.loads(line))
        return messages

    def start(self, method, params):
        identifier = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": identifier, "method": method,
                   "params": params})
        return identifier

    def request(self, method, params, timeout=60.0):
        identifier = self.start(method, params)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for message in self.pump(0.05):
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

    def kill(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL)
        self.process.wait()
        if self.socket is not None:
            self.socket.close()


def editor_state(editor):
    described = editor.call("describe", {})
    compositions = editor.call("list_motion_compositions", {})
    timelines = editor.call("list_timelines", {})
    if not (described.get("ok") and compositions.get("ok")
            and timelines.get("ok")):
        return None, (described, compositions, timelines)
    clips = sum(1 for track in described["result"]["timeline"]["tracks"]
                if track["id"] == V1 for item in track["items"]
                if item.get("type") == "clip")
    return (clips, len(compositions["result"]["compositions"]),
            len(timelines["result"]["timelines"])), None


def count_clips(timeline):
    tracks = timeline.get("tracks")
    if tracks is None:
        tracks = timeline.get("sequence", {}).get("tracks", [])
    return sum(len(track.get("clips", [])) for track in tracks
               if track.get("id") == V1)


def disk_state():
    """The state the files hold, read without the session lock and
    without writing: what the editor must show after reopening."""
    with open(P) as handle:
        stored = json.load(handle)
    active = stored["active_timeline_id"]
    with open(os.path.join(PKG, "Timelines", active + ".json")) as handle:
        timeline = json.load(handle)
    return (count_clips(timeline), len(stored["compositions"]),
            len(stored["timelines"]))


def autosave_verdict():
    """ProjectRecovery::Inspect, as the editor will classify the autosave
    at its next launch, and the state it would recover."""
    if not os.path.exists(AUTOSAVE):
        return "none", None
    try:
        with open(AUTOSAVE) as handle:
            envelope = json.load(handle)
        recovered = json.loads(envelope["project"])
        active = recovered["active_timeline_id"]
        timeline = next(item for item in recovered["timelines"]
                        if item["id"] == active)
        state = (count_clips(timeline), len(recovered["compositions"]),
                 len(recovered["timelines"]))
    except (OSError, ValueError, KeyError, StopIteration) as error:
        return "invalid", str(error)[:200]
    try:
        with open(P) as handle:
            json.load(handle)
        primary_readable = True
    except (OSError, ValueError):
        primary_readable = False
    if not primary_readable or (os.stat(AUTOSAVE).st_mtime_ns >
                                os.stat(P).st_mtime_ns):
        return ("available" if primary_readable else "available-unreadable",
                state)
    return "stale", state


def leftovers():
    found = []
    for pattern in ("*.cutmachine-*.tmp", "*.cutmachine-*.bak",
                    "Timelines/*.cutmachine-*.tmp",
                    "Timelines/*.cutmachine-*.bak", "*.lock-*"):
        found += [os.path.relpath(path, PKG)
                  for path in glob.glob(os.path.join(PKG, pattern))]
    if os.path.exists(MARKER):
        found.append(".cutmachine-transaction")
    return found


def autosave_temporaries():
    return [os.path.basename(path)
            for path in glob.glob(AUTOSAVE + ".*.tmp")]


def composition_args(name):
    return {"name": name, "definition": {
        "width": 1080, "height": 1920, "fps": 25, "duration": 75,
        "root": {"type": "fill", "style": {"backgroundColor": "#000000"}}}}


added = cli("add_motion_composition", composition_args("base"))
if not added.get("ok"):
    sys.exit(f"setup failed: {added}")
composition = added["result"]["composition"]["id"]
sockets_before = set(glob.glob(f"/tmp/cutmachine-{os.getuid()}/*.sock"))

counts = {"killed": 0, "answered_before_kill": 0, "autosave_at_kill": 0,
          "in_transaction": 0, "applied": 0, "rolled_back": 0,
          "offered": 0, "recovered": 0, "declined": 0, "stale_kept": 0,
          "autosave_tmp_after_kill": 0, "kills_random": 0, "kills_on_autosave": 0,
          "kills_on_transaction": 0, "marker_seen": 0}
failures = []
launch_seconds = []
previous = None  # what the killed edit was: before, after, answered
offers = 0
editor = None
try:
    for iteration in range(options.iterations + 1):
        # What the editor will find, and the answer it is given.
        verdict, recoverable = autosave_verdict()
        answer = "recover"
        if verdict.startswith("available"):
            offers += 1
            counts["offered"] += 1
            answer = "decline" if offers % 3 == 0 else "recover"
        editor = Editor(answer)
        problem = editor.wait_ready()
        if problem:
            failures.append((iteration, "launch", verdict, problem))
            editor.kill()
            break
        launch_seconds.append(editor.ready_after)
        current, error = editor_state(editor)
        if current is None:
            failures.append((iteration, "editor state unreadable",
                             str(error)[:300]))
            editor.kill()
            break
        on_disk = disk_state()
        if on_disk != current:
            failures.append((iteration, "editor and files differ", current,
                             on_disk))
        if previous is not None:
            before, after, answered, kind = previous
            if verdict.startswith("available"):
                if answer == "recover":
                    counts["recovered"] += 1
                    if current != recoverable:
                        failures.append((iteration, "recovery is not the "
                                         "autosave", recoverable, current))
                else:
                    counts["declined"] += 1
            if verdict == "invalid":
                failures.append((iteration, "invalid autosave", recoverable))
            if current == after:
                counts["applied"] += 1
            elif current == before:
                counts["rolled_back"] += 1
                if answered:
                    failures.append((iteration, "answered edit lost", kind))
            else:
                failures.append((iteration, "partial state", kind, before,
                                 after, current))
            if verdict == "stale" and os.path.exists(AUTOSAVE):
                counts["stale_kept"] += 1
        residue = leftovers()
        if residue:
            failures.append((iteration, "leftovers after reopening",
                             residue))
        if autosave_temporaries():
            failures.append((iteration, "autosave temporary survives "
                             "reopening", autosave_temporaries()))
        if verdict.startswith("available") and os.path.exists(AUTOSAVE) \
                and verdict != "available-unreadable":
            failures.append((iteration, "offered autosave not discarded",
                             answer))
        if iteration == options.iterations:
            break

        if iteration % 10 == 9:
            edited = editor.call("insert_motion_clip", {
                "track_id": V1, "composition_id": composition,
                "timeline_in": {"value": current[0] * 75, "rate": 25}})
            after_edit, _ = editor_state(editor)
            undone = editor.call("undo", {})
            after_undo, _ = editor_state(editor)
            redone = editor.call("redo", {})
            after_redo, _ = editor_state(editor)
            if not (edited.get("ok") and undone.get("ok")
                    and redone.get("ok")
                    and after_edit == (current[0] + 1, current[1],
                                       current[2])
                    and after_undo == current and after_redo == after_edit):
                failures.append((iteration, "undo/redo", current, after_edit,
                                 after_undo, after_redo))
            current = after_redo if after_redo is not None else current

        kind = random.choice(["clip", "clip", "composition", "timeline"])
        if kind == "clip":
            tool, delta = "insert_motion_clip", (1, 0, 0)
            args = {"track_id": V1, "composition_id": composition,
                    "timeline_in": {"value": current[0] * 75, "rate": 25}}
        elif kind == "composition":
            tool, delta = "add_motion_composition", (0, 1, 0)
            args = composition_args(f"C{iteration}")
        else:
            tool, delta = "add_timeline", (0, 0, 1)
            args = {"name": f"T{iteration}", "width": 1080, "height": 1920,
                    "frame_rate": {"num": 25, "den": 1}}
        mode = random.choice(["random", "random", "autosave", "transaction"])
        counts["kills_" + {"random": "random", "autosave": "on_autosave",
                           "transaction": "on_transaction"}[mode]] += 1
        identifier = editor.start("tools/call", {"name": tool,
                                                 "arguments": args})
        sent = time.monotonic()
        deadline = sent + random.uniform(*options.window)
        answered = False
        trigger = None
        marker_seen = False
        while True:
            now = time.monotonic()
            if mode == "random" and now >= deadline:
                break
            if not marker_seen and os.path.exists(MARKER):
                marker_seen = True
            if mode != "random" and trigger is None and (
                    os.path.exists(AUTOSAVE) if mode == "autosave"
                    else marker_seen):
                # The transaction lasts a few milliseconds on an internal
                # SSD: a kill drawn later than that lands after it.
                trigger = now + random.uniform(
                    0.0, 0.03 if mode == "autosave" else 0.002)
            if trigger is not None and now >= trigger:
                break
            if now - sent > 2.0:  # the edit finished without the trigger
                break
            try:
                for message in editor.pump(0.0005):
                    if message.get("id") == identifier:
                        answered = True
            except ConnectionError:
                break
        at_kill_autosave = os.path.exists(AUTOSAVE)
        at_kill_marker = os.path.exists(MARKER)
        editor.kill()
        editor = None
        counts["killed"] += 1
        counts["answered_before_kill"] += answered
        counts["autosave_at_kill"] += at_kill_autosave
        counts["in_transaction"] += at_kill_marker
        counts["marker_seen"] += marker_seen or at_kill_marker
        counts["autosave_tmp_after_kill"] += bool(autosave_temporaries())
        previous = (current, tuple(a + b for a, b in zip(current, delta)),
                    answered, kind)
finally:
    if editor is not None:
        editor.kill()
    # The project as the CLI reopens it, once no editor holds it.
    final = cli("describe", {})
    if not final.get("ok"):
        failures.append(("end", "the CLI cannot reopen the project",
                         str(final)[:300]))
    if had_defaults:
        subprocess.run(["defaults", "import", DOMAIN, defaults_backup])
    else:
        subprocess.run(["defaults", "delete", DOMAIN], capture_output=True)

sockets_left = sorted(set(glob.glob(f"/tmp/cutmachine-{os.getuid()}/*.sock"))
                      - sockets_before)
launch_seconds.sort()
summary = {
    "counts": counts,
    "failures": len(failures),
    "launch_seconds": {
        "median": launch_seconds[len(launch_seconds) // 2]
        if launch_seconds else None,
        "max": launch_seconds[-1] if launch_seconds else None},
    "autosave_temporaries_at_end": autosave_temporaries(),
    "stale_autosave_at_end": os.path.exists(AUTOSAVE),
    "sockets_left": len(sockets_left),
}
print(json.dumps(summary))
for failure in failures[:20]:
    print("  ", failure)
if not options.keep:
    shutil.rmtree(folder, ignore_errors=True)
    for path in sockets_left:
        try:
            os.remove(path)
        except OSError:
            pass
sys.exit(1 if failures else 0)
