#!/usr/bin/env python3
# REMOTION-PARITY-2026-09 REM6 (DEC13) -- the film that closes the contract.
# A delivered film must unite a video played inside a composition, a
# precomposition, a transition between scenes and a raster effect, pass the
# export and verify_export, and then be looked at by the author. No film made so
# far unites the four: the
# CUTMACHINE teaser of 28 September has the precompositions and the raster
# effects, the showreel and a short reel play videos with raster
# effects, nothing has the four. This script builds, in a throwaway
# project, the film the 28 September decision described: the teaser as it
# was made, then -- through a transition -- a rush played in a composition
# under a title with a shadow and a glow. It then checks by the engine's own
# descriptor that each of the four is on screen where it should be, exports
# the timeline and replays verify_export on the file.
#
#   scripts/rem6_film.py <folder> --teaser <teaser project.cutmachine.json>
#       --rush <video file> [--composition "Teaser v4"]
#       [--rush-start FRAMES] [--binary build/cutmachine]
#
# The teaser project is only read: its composition (and the ones it shows)
# are copied as data into a new project under <folder>, their ULIDs
# remapped. The rush is ingested from where it is, and nothing is written
# beside it -- copy it first if it lives on a disk that must stay untouched.
# Everything runs through `--call-tool`, the project archive off and nothing
# of the machine's own state touched, logs and motion library under
# <folder>. The film is
# <folder>/REM6-teaser-et-rush.mp4 and the report <folder>/rem6-report.json.
import argparse
import json
import os
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("folder")
parser.add_argument("--teaser", required=True)
parser.add_argument("--rush", required=True)
parser.add_argument("--composition", default="Teaser v4")
parser.add_argument("--rush-start", type=int, default=0)
here = os.path.dirname(os.path.abspath(__file__))
parser.add_argument("--binary", default=os.path.join(here, "..", "build",
                                                     "cutmachine"))
options = parser.parse_args()
binary = os.path.abspath(options.binary)
folder = os.path.abspath(options.folder)
os.makedirs(folder, exist_ok=True)
env = dict(os.environ,
           CUTMACHINE_PROJECT_ARCHIVE="off",
           CUTMACHINE_LOG_DIR=os.path.join(folder, "logs"),
           CUTMACHINE_MOTION_LIBRARY=os.path.join(folder, "motion"),
           CUTMACHINE_MODELS="off")
ULID = re.compile(r"^[0-9A-HJKMNP-TV-Z]{26}$")
TRANSITION = 15   # frames of the fade between the teaser and the rush
RUSH_SCENE = 150  # frames of the rush scene, transition included
report = {"checks": [], "failures": []}


def check(condition, what, detail=None):
    report["checks"].append({"ok": bool(condition), "what": what,
                             "detail": detail})
    if not condition:
        report["failures"].append(what)
        print("FAIL:", what, detail if detail is not None else "")


def call(project, tool, args, timeout=7200):
    out = subprocess.run([binary, "--call-tool", project, tool,
                          json.dumps(args)], stdin=subprocess.DEVNULL,
                         capture_output=True, text=True, timeout=timeout,
                         env=env)
    try:
        answer = json.loads(out.stdout)
    except json.JSONDecodeError:
        sys.exit(f"{tool}: unparsable answer: {out.stdout[:400]} "
                 f"{out.stderr[:400]}")
    if not answer.get("ok"):
        sys.exit(f"{tool} refused: {json.dumps(answer)[:800]}")
    return answer["result"] if "result" in answer else answer


# The teaser, read only.
teaser_path = options.teaser
if os.path.isdir(teaser_path):
    teaser_path = os.path.join(teaser_path, "project.cutmachine.json")
with open(teaser_path) as handle:
    teaser = json.load(handle)
by_id = {item["id"]: item for item in teaser["compositions"]}
named = [item for item in teaser["compositions"]
         if item["name"] == options.composition]
if len(named) != 1:
    sys.exit(f"no single composition named {options.composition!r}")


def shown(definition):
    found = []

    def walk(node):
        if isinstance(node, dict):
            if node.get("type") == "composition" and isinstance(
                    node.get("composition"), str):
                found.append(node["composition"])
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(definition)
    return found


# Dependencies first, so that each copied composition names copies.
order = []


def visit(identifier, stack=()):
    if identifier in order:
        return
    if identifier in stack:
        sys.exit("the teaser shows itself")
    for child in shown(by_id[identifier]["definition"]):
        visit(child, stack + (identifier,))
    order.append(identifier)


visit(named[0]["id"])

created = subprocess.run(
    [binary, "--create-project",
     os.path.join(folder, "REM6.cutmachine-project"), "REM6",
     "--format", "1920x1080@25"], stdin=subprocess.DEVNULL,
    capture_output=True, text=True, env=env)
if created.returncode != 0:
    sys.exit(f"create-project failed: {created.stdout} {created.stderr}")
project = json.loads(created.stdout)
P = project["path"]
timeline = project["timeline_id"]
V1 = next(track["id"] for track in project["tracks"]
          if track["kind"] == "video")

rush = os.path.abspath(options.rush)
ingested = call(P, "ingest_media", {"path": rush})
if ingested.get("added") != 1:
    sys.exit(f"the rush was not ingested once: {json.dumps(ingested)[:400]}")
library = call(P, "describe", {}).get("library", [])
media = [item for item in library
         if item.get("filename") == os.path.basename(rush)]
rush_id = media[0]["id"] if len(media) == 1 else None
check(rush_id and ULID.match(rush_id), "the rush is a media of the project",
      rush_id)

remap = {}
for identifier in order:
    definition = json.loads(json.dumps(by_id[identifier]["definition"]))

    def rename(node):
        if isinstance(node, dict):
            if node.get("type") == "composition" and node.get(
                    "composition") in remap:
                node["composition"] = remap[node["composition"]]
            for value in node.values():
                rename(value)
        elif isinstance(node, list):
            for value in node:
                rename(value)
    rename(definition)
    added = call(P, "add_motion_composition",
                 {"name": by_id[identifier]["name"],
                  "definition": definition})
    remap[identifier] = added["composition"]["id"]
teaser_id = remap[named[0]["id"]]
teaser_frames = named[0]["definition"]["duration"]
total = teaser_frames + RUSH_SCENE - TRANSITION

film = {
    "width": 1920, "height": 1080, "fps": 25, "duration": total,
    "root": {"type": "series", "children": [
        {"type": "sequence", "id": "teaser", "duration": teaser_frames,
         "children": [{"type": "composition", "id": "teaser-instance",
                       "composition": teaser_id,
                       "style": {"width": "100%", "height": "100%"}}]},
        {"type": "transition", "id": "fondu", "presentation": "fade",
         "timing": {"duration": TRANSITION, "easing": "ease-in-out"}},
        {"type": "sequence", "id": "rush", "children": [
            {"type": "fill", "style": {"backgroundColor": "#0C0D0E",
                                       "padding": 72},
             "children": [
                 {"type": "video", "id": "rush-video", "media": rush_id,
                  "startFrom": options.rush_start, "muted": True,
                  "style": {"width": "100%", "height": "100%",
                            "objectFit": "cover", "borderRadius": 28}},
                 {"type": "text", "id": "rush-title",
                  "content": "Un rush joué dans une composition",
                  "style": {"position": "absolute", "left": 132,
                            "bottom": 126,
                            "fontFamily": "Fira Sans Condensed",
                            "fontWeight": 600, "fontSize": 64,
                            "color": "#F2EDE4",
                            "filter": "drop-shadow(0px 6px 10px "
                                      "rgba(0,0,0,0.7)) "
                                      "glow(14px #FF5A1F 1.5)"}}]}]}]}}
added = call(P, "add_motion_composition",
             {"name": "REM6 — teaser et rush", "definition": film})
film_id = added["composition"]["id"]
inserted = call(P, "insert_motion_clip",
                {"track_id": V1, "composition_id": film_id,
                 "timeline_in": {"value": 0, "rate": 25}})
clip_id = inserted["clip_id"]


def descriptor(frame):
    """The geometry the engine computed for that frame of the film."""
    return call(P, "render_motion_frame",
                {"composition_id": film_id, "frame": frame})


def nodes(tree):
    found = []

    def walk(node):
        if isinstance(node, dict):
            found.append(node)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
    walk(tree)
    return found


def summary(frame):
    tree = descriptor(frame)
    every = nodes(tree)
    instances = [node["composition"] for node in every
                 if isinstance(node.get("composition"), dict)]
    videos = [node["video"] for node in every
              if isinstance(node.get("video"), dict)]
    filters = [node for node in every
               if isinstance(node.get("filter"), dict)
               and node["filter"].get("stack")]
    transitions = [node["transition"] for node in every
                   if isinstance(node.get("transition"), dict)]
    return tree, instances, videos, filters, transitions


# The teaser on screen: its instance plays, and shows its own instances.
probes = {}
for frame in (12, 300, 550, 700):
    tree, instances, videos, filters, transitions = summary(frame)
    playing = [item for item in instances if item.get("state") == "playing"]
    probes[frame] = {"instances": len(playing), "videos": len(videos),
                     "effects": len(filters),
                     "transition": bool(transitions)}
    check(any(item.get("id") == teaser_id and item.get("frame") == frame
              for item in playing),
          f"frame {frame}: the teaser plays at its frame {frame}",
          [(item.get("id"), item.get("frame")) for item in playing][:3])
probes_nested = sum(1 for frame in probes if probes[frame]["instances"] > 1)
check(probes_nested > 0, "the teaser shows precompositions of its own "
      "(an instance inside the instance)", probes)
check(any(probes[frame]["effects"] for frame in probes),
      "a raster effect is on screen in the teaser", probes)

# The transition: half-way, both scenes, the fade in progress.
middle = teaser_frames - TRANSITION + TRANSITION // 2
tree, instances, videos, filters, transitions = summary(middle)
check(transitions and transitions[0].get("presentation") == "fade",
      f"frame {middle}: the fade is in progress",
      transitions[:1])
check(any(item.get("state") == "playing" for item in instances) and
      any(item.get("state") == "playing" for item in videos),
      f"frame {middle}: the teaser and the rush are both on screen",
      {"instances": [(i.get("id"), i.get("state")) for i in instances][:2],
       "videos": videos[:1]})

# The rush: the frame of the media is the one its time says.
rush_begins = teaser_frames - TRANSITION
for frame in (rush_begins + 20, total - 1):
    tree, instances, videos, filters, transitions = summary(frame)
    local = frame - rush_begins
    check(any(item.get("media") == rush_id and item.get("state") ==
              "playing" and item.get("frame") == options.rush_start + local
              for item in videos),
          f"frame {frame}: the rush shows its frame "
          f"{options.rush_start + local}", videos[:1])
    check(any(node.get("id") == "rush-title" for node in filters),
          f"frame {frame}: the title carries its shadow and glow",
          [node.get("id") for node in filters][:4])

output = os.path.join(folder, "REM6-teaser-et-rush.mp4")
exported = call(P, "export_timeline",
                {"timeline_id": timeline, "output_path": output,
                 "preset": "h264", "overwrite": True, "verify": True})
verification = exported.get("verification", {})
replayed = call(P, "verify_export", {"timeline_id": timeline,
                                     "path": output})
for name, result in (("export_timeline verify", verification),
                     ("verify_export", replayed)):
    check(result.get("divergent_frames") == 0,
          f"{name}: no frame diverges", result.get("divergent_frames"))
    check(result.get("motion_checked_frames") == total,
          f"{name}: every frame of the film is checked against the engine",
          result.get("motion_checked_frames"))
report.update({"project": P, "timeline_id": timeline, "film": output,
               "clip_id": clip_id,
               "composition_id": film_id, "teaser_id": teaser_id,
               "rush_id": rush_id, "frames": total,
               "copied_compositions": [by_id[i]["name"] for i in order],
               "export": {k: v for k, v in exported.items()
                          if k != "verification"},
               "verification": verification, "verify_export": replayed})
with open(os.path.join(folder, "rem6-report.json"), "w") as handle:
    json.dump(report, handle, indent=2, ensure_ascii=False)
print(json.dumps({"film": output, "frames": total,
                  "checks": len(report["checks"]),
                  "failures": len(report["failures"]),
                  "verify_export": {k: replayed.get(k) for k in (
                      "exact", "divergent_frames", "motion_checked_frames",
                      "verified_fraction", "not_exact_because")}},
                 ensure_ascii=False))
sys.exit(1 if report["failures"] else 0)
