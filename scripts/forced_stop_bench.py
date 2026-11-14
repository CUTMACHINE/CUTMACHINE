#!/usr/bin/env python3
# SAVING_ROADMAP.md -- the forced-stop validation. Kills CLI edits with
# SIGKILL at random instants on a throwaway project, relaunches, and checks
# what a crash must leave behind:
#
#   - the project opens again (recovery runs on load);
#   - each edit is whole or absent, never half applied;
#   - no transaction marker and no .tmp/.bak leftover survive a reload;
#   - the session lock of a killed writer does not refuse the next one, and
#     what it leaves is removed by an opening 30 s later;
#   - every JSON of the package parses, and the manifest names files that
#     exist;
#   - undo and redo still work after the interruptions.
#
#   scripts/forced_stop_bench.py <folder on the volume> [--iterations N]
#       [--window LOW HIGH] [--seed S] [--binary build/cutmachine] [--keep]
#
# The project is created under <folder>, so its reads, fsyncs and renames
# hit that volume, and removed at the end unless --keep. Edits alternate a
# timeline edit (insert_motion_clip), a project edit
# (add_motion_composition) and a new timeline (add_timeline). --window is
# the range, in seconds after launch, the kill is drawn from: the default
# covers the whole command; narrow it on the commit (on an exFAT volume, 0.17
# to 0.32 s) to land most kills inside a transaction.
import argparse
import glob
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("folder")
parser.add_argument("--iterations", type=int, default=150)
parser.add_argument("--window", type=float, nargs=2, default=(0.0, 0.3))
parser.add_argument("--seed", type=int, default=20260924)
parser.add_argument("--binary", default=os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "build", "cutmachine"))
parser.add_argument("--keep", action="store_true")
options = parser.parse_args()
random.seed(options.seed)
binary = os.path.abspath(options.binary)
folder = os.path.join(os.path.abspath(options.folder),
                      f"cutmachine-forced-stop-{os.getpid()}")
os.makedirs(folder)
created = subprocess.run(
    [binary, "--create-project",
     os.path.join(folder, "Arret.cutmachine-project"), "Arret", "--format",
     "1080x1920@25"], stdin=subprocess.DEVNULL, capture_output=True,
    text=True, check=True)
project = json.loads(created.stdout)
P = project["path"]
V1 = next(track["id"] for track in project["tracks"]
          if track["kind"] == "video")
PKG = os.path.dirname(P)


def call(tool, args):
    out = subprocess.run([binary, "--call-tool", P, tool, json.dumps(args)],
                         stdin=subprocess.DEVNULL, capture_output=True,
                         text=True, timeout=120)
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return {"ok": False, "error": "unparsable",
                "detail": out.stdout[:300] + out.stderr[:300]}


def composition_args(name):
    return {"name": name, "definition": {
        "width": 1080, "height": 1920, "fps": 25, "duration": 75,
        "root": {"type": "fill", "style": {"backgroundColor": "#000000"}}}}


def state():
    described = call("describe", {})
    compositions = call("list_motion_compositions", {})
    timelines = call("list_timelines", {})
    if not (described.get("ok") and compositions.get("ok")
            and timelines.get("ok")):
        return None, (described, compositions, timelines)
    clips = sum(1 for track in described["result"]["timeline"]["tracks"]
                if track["id"] == V1 for item in track["items"]
                if item.get("type") == "clip")
    return (clips, len(compositions["result"]["compositions"]),
            len(timelines["result"]["timelines"])), None


def leftovers(patterns=("*.cutmachine-*.tmp", "*.cutmachine-*.bak",
                         "Timelines/*.cutmachine-*.tmp",
                         "Timelines/*.cutmachine-*.bak")):
    found = []
    for pattern in patterns:
        found += glob.glob(os.path.join(PKG, pattern))
    if os.path.exists(os.path.join(PKG, ".cutmachine-transaction")):
        found.append(".cutmachine-transaction")
    return found


def package_problems():
    problems = []
    for path in glob.glob(os.path.join(PKG, "*.json")) + glob.glob(
            os.path.join(PKG, "Timelines", "*.json")):
        if os.path.basename(path).startswith("._"):
            continue
        try:
            with open(path) as handle:
                json.load(handle)
        except (OSError, ValueError) as error:
            problems.append(f"{path}: {error}")
    with open(os.path.join(PKG, "manifest.json")) as handle:
        manifest = json.load(handle)
    for timeline in manifest["timelines"]:
        if not os.path.exists(os.path.join(PKG, timeline["path"])):
            problems.append("manifest names a missing " + timeline["path"])
    return problems


added = call("add_motion_composition", composition_args("base"))
if not added.get("ok"):
    sys.exit(f"setup failed: {added}")
composition = added["result"]["composition"]["id"]
counts = {"killed": 0, "completed": 0, "in_transaction": 0, "applied": 0,
          "rolled_back": 0}
failures = []
current, error = state()
if current is None:
    sys.exit(f"initial load failed: {error}")

for iteration in range(options.iterations):
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
    process = subprocess.Popen([binary, "--call-tool", P, tool,
                                json.dumps(args)], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    time.sleep(random.uniform(*options.window))
    killed = process.poll() is None
    if killed:
        process.send_signal(signal.SIGKILL)
    stdout, _ = process.communicate()
    if killed:
        counts["killed"] += 1
        counts["in_transaction"] += os.path.exists(
            os.path.join(PKG, ".cutmachine-transaction"))
    else:
        counts["completed"] += 1
        try:
            result = json.loads(stdout)
        except ValueError:
            result = {"ok": False, "detail": stdout[:200]}
        if not result.get("ok"):
            failures.append((iteration, "completed edit refused", kind,
                             str(result)[:300]))
    after, error = state()
    if after is None:
        failures.append((iteration, "reload failed", kind, str(error)[:300]))
        break
    if after == tuple(a + b for a, b in zip(current, delta)):
        counts["applied"] += 1
    elif after == current:
        counts["rolled_back"] += 1
        if not killed:
            failures.append((iteration, "completed edit lost", kind))
    else:
        failures.append((iteration, "partial state", kind, current, after))
    residue = leftovers()
    if residue:
        failures.append((iteration, "leftovers after reload", kind, residue))
    problems = package_problems()
    if problems:
        failures.append((iteration, "package", kind, problems))
    current = after

    if iteration % 10 == 9:
        edited = call("insert_motion_clip", {
            "track_id": V1, "composition_id": composition,
            "timeline_in": {"value": current[0] * 75, "rate": 25}})
        after_edit, _ = state()
        undone = call("undo", {})
        after_undo, _ = state()
        redone = call("redo", {})
        after_redo, _ = state()
        if not (edited.get("ok") and undone.get("ok") and redone.get("ok")
                and after_edit == (current[0] + 1, current[1], current[2])
                and after_undo == current and after_redo == after_edit):
            failures.append((iteration, "undo/redo", current, after_edit,
                             after_undo, after_redo))
        current = after_redo if after_redo is not None else current

# A writer killed between staging its lock and publishing it leaves the
# staging directory; the next opening removes it once it is older than a
# live writer's could be (30 s, ProjectStorage.cc).
time.sleep(31)
reopened = call("describe", {})
staging = leftovers(("*.lock-*", "._*.lock-*", "*.lock", "._*.lock"))
if not reopened.get("ok") or staging:
    failures.append(("end", "lock staging left after 30 s", staging))

print(json.dumps({"counts": counts, "final_state": current,
                  "failures": len(failures)}))
for failure in failures[:20]:
    print("  ", failure)
if not options.keep:
    shutil.rmtree(folder, ignore_errors=True)
sys.exit(1 if failures else 0)
