#!/usr/bin/env python3
"""R9 / FC7 / #240 / #247 -- real Resolve imports in a throwaway project.

Synthetic pictures make a position error measurable without private rushes.
The previous Resolve project is restored, and only the project created here
is deleted. Files stay in the requested, initially absent folder for review.
Resolve Studio must already run with external scripting enabled.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "sidecar"))
import resolve_bridge as bridge


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(options):
    folder = Path(options.folder).resolve()
    folder.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ,
               CUTMACHINE_PROJECT_ARCHIVE="off",
               CUTMACHINE_ARCHIVE_BACKUP_DIR="off",
               CUTMACHINE_LOG_DIR=str(folder / "logs"),
               CUTMACHINE_MOTION_LIBRARY=str(folder / "motion"),
               CUTMACHINE_MODELS="off")

    def cli(*arguments):
        process = subprocess.run(
            [str(Path(options.binary).resolve()), *map(str, arguments)],
            env=env, capture_output=True, text=True, timeout=240)
        response = json.loads(process.stdout)
        check(process.returncode == 0 and response.get("ok"),
              f"Moteur : {response}")
        return response

    for name, size in (("P", "360x640"), ("A", "640x360"),
                       ("B", "640x360")):
        subprocess.run(
            [options.ffmpeg, "-v", "error", "-f", "lavfi", "-i",
             f"testsrc2=size={size}:rate=25:duration=12", "-c:v", "prores_ks",
             "-profile:v", "0", "-pix_fmt", "yuv422p10le", "-timecode",
             "00:00:00:00", str(folder / f"{name}.mov")], check=True)

    created = cli("--create-project", folder / "motion.cutmachine-project",
                  "Basic Motion", "--format", "640x360@25")
    path = created["path"]
    cli("--ingest", path, folder / "P.mov")
    source = cli("--describe", path)["library"][0]["id"]
    track = created["tracks"][0]["id"]

    def tool(name, arguments):
        response = cli("--call-tool", path, name, json.dumps(arguments))
        check(response["result"].get("ok"), f"{name} : {response}")
        return response["result"]

    def frames(value):
        return {"value": value, "rate": 25}

    clips = []
    for index in range(3):
        response = tool("insert_clip", {
            "track_id": track, "source_id": source,
            "source_in": frames(50 * index), "duration": frames(50),
            "timeline_in": frames(50 * index)})
        clips.append(response["delta"]["clips"][0]["clip_id"])
    tool("set_clip_framing", {"clip_ids": [clips[0]], "mode": "fill"})
    tool("set_clip_framing", {
        "clip_ids": [clips[1]], "mode": "fit", "scale": {"num": 6, "den": 5},
        "position_x": {"num": 1, "den": 10},
        "position_y": {"num": -1, "den": 20}})
    animation = []
    for parameter, first, last in (("framing.scale", 100, 140),
                                   ("framing.position_x", 0, 15),
                                   ("framing.position_y", 0, 10)):
        animation.append({"parameter": parameter, "keyframes": [
            {"time": frames(0), "value": {"num": first, "den": 100}},
            {"time": frames(49), "value": {"num": last, "den": 100}}]})
    tool("set_clip_animation", {"clip_ids": [clips[2]],
                                "animation": animation})
    cli("--export", path, folder / "engine.mov", "--software", "--entire")

    def gray(video):
        return subprocess.check_output(
            [options.ffmpeg, "-v", "error", "-i", str(video), "-vf",
             "scale=160:90", "-pix_fmt", "gray", "-f", "rawvideo", "-"])

    expected = gray(folder / "engine.mov")
    resolve = bridge.connect()
    manager = resolve.GetProjectManager()
    previous = manager.GetCurrentProject()
    previous_name = previous.GetName() if previous else None
    name = f"CUTMACHINE-interchange-{time.time_ns()}"
    project = manager.CreateProject(name)
    check(project, "Resolve a refusé le projet jetable")
    report = {"resolve": resolve.GetVersionString(), "engine": cli("--version"),
              "motion": {}, "srt": {}, "edl": {}}
    try:
        for key, value in (("timelineFrameRate", "25"),
                           ("timelineResolutionWidth", "640"),
                           ("timelineResolutionHeight", "360"),
                           ("colorScienceMode", "davinciYRGB"),
                           ("colorSpaceTimeline", "Rec.709 Gamma 2.4"),
                           ("colorSpaceOutput", "Rec.709 Gamma 2.4")):
            check(project.SetSetting(key, value), f"Réglage refusé : {key}")
        pool = project.GetMediaPool()
        check(len(pool.ImportMedia([str(folder / f"{n}.mov")
                                    for n in ("P", "A", "B")])) == 3,
              "Les trois rushes synthétiques ne sont pas importés")

        for scaling in ("fit", "fill"):
            xml = folder / f"motion-{scaling}.xml"
            cli("--export-resolve-xml", path, xml, "--resolve-scaling", scaling)
            bridge.send_timeline_xml(resolve, str(xml), f"motion-{scaling}",
                                     directory=str(folder / "xml-imports"))
            timeline = project.GetCurrentTimeline()
            check(timeline.SetSetting("useCustomSettings", "1"),
                  "Réglages propres à la timeline refusés")
            behavior = "scaleToFit" if scaling == "fit" else "scaleToCrop"
            check(timeline.SetSetting("timelineInputResMismatchBehavior",
                                      behavior), "Mode ajuster/remplir refusé")
            check(project.SetCurrentRenderFormatAndCodec("mov", "ProRes422"),
                  "Format ProRes refusé")
            check(project.SetRenderSettings({
                "SelectAllFrames": True, "TargetDir": str(folder),
                "CustomName": f"resolve-{scaling}", "ExportVideo": True,
                "ExportAudio": False, "FormatWidth": 640, "FormatHeight": 360}),
                "Réglages du rendu refusés")
            job = project.AddRenderJob()
            check(job and project.StartRendering([job]), "Rendu non démarré")
            deadline = time.monotonic() + 240
            while time.monotonic() < deadline:
                status = project.GetRenderJobStatus(job)
                if status.get("JobStatus") in ("Complete", "Failed", "Cancelled"):
                    break
                time.sleep(1)
            check(status.get("JobStatus") == "Complete", f"Rendu : {status}")
            actual = gray(folder / f"resolve-{scaling}.mov")
            check(len(actual) == len(expected) == 150 * 160 * 90,
                  "Le rendu ne contient pas les 150 images attendues")
            differences = []
            size = 160 * 90
            for frame in range(150):
                start = frame * size
                differences.append(sum(abs(a - b) for a, b in zip(
                    expected[start:start + size], actual[start:start + size]))
                                   / size)
            # The real 4 November 2026 measurement peaks at 1.61/255. Three
            # allows codec/resizer noise; the old position bug exceeds 56.
            check(max(differences) < 3,
                  f"{scaling} : image déplacée ({max(differences):.3f}/255)")
            report["motion"][scaling] = {
                "frames": 150, "max_mean_luma_error": max(differences),
                "mean_luma_error": sum(differences) / len(differences)}
            print(f"Basic Motion {scaling} : 150 images comparées", flush=True)

        cut = {"schema": bridge.TIMELINE_SCHEMA, "name": "srt-original",
               "clips": [{"path": str(folder / "A.mov"), "filename": "A.mov",
                          "start_frame": 0, "end_frame": 100,
                          "record_frame": 0, "video_layer": 0,
                          "with_audio": False}]}
        srt = folder / "revised.srt"
        contents = ""
        for count, text in enumerate(("PREMIER", "DEUXIEME", "TROISIEME"), 1):
            contents += (f"{count}\n00:00:0{count - 1},000 --> "
                         f"00:00:0{count - 1},800\n{text}\n\n")
            if count < 2:
                continue
            srt.write_text(contents)
            bridge.send_timeline(resolve, cut, name=f"srt-{count}",
                                 srt_path=str(srt),
                                 srt_directory=str(folder / "srt-imports"))
            timeline = project.GetCurrentTimeline()
            texts = [item.GetName() for item in
                     timeline.GetItemListInTrack("subtitle", 1)]
            check(texts == ["PREMIER", "DEUXIEME", "TROISIEME"][:count],
                  f"Le SRT révisé ne porte pas ses textes : {texts}")
            report["srt"][str(count)] = texts
        srt.write_text("1\n00:00:00,000 --> 00:00:00,800\nPREMIER\n\n"
                       "2\n00:00:00,000 --> 00:00:00,800\nDEUXIEME\n")
        try:
            bridge.send_timeline(resolve, cut, name="srt-overlap",
                                 srt_path=str(srt),
                                 srt_directory=str(folder / "srt-imports"))
        except bridge.ResolveBridgeError as error:
            check("1 sous-titre(s)" in str(error) and "2 cue(s)" in str(error),
                  f"Le refus n'explique pas les comptes : {error}")
        else:
            raise RuntimeError("Les cues fusionnées n'ont pas été refusées")
        check(not project.GetCurrentTimeline().GetItemListInTrack("video", 1),
              "Un plan vidéo a été posé malgré les cues fusionnées")
        report["srt"]["overlap_refused_before_picture"] = True

        edls = {
            "cut_25": [("A.mov", 0, 100, 10, 110, 100),
                       ("B.mov", 100, 150, 0, 50, 100)],
            "fondu_25": [("A.mov", 0, 100, 0, 100, 100),
                         ("B.mov", 100, 200, 50, 150, 100)],
            "m2_25_V1": [("A.mov", 0, 50, 100, 125, 50)]}
        for fixture, expected_clips in edls.items():
            text = (REPO / "tests" / "fixtures" / "edl" /
                    f"{fixture}.edl").read_text()
            text = text.replace("A.MP4", "A.mov").replace("B.MOV", "B.mov")
            edl = folder / f"{fixture}.edl"
            edl.write_text(text)
            timeline = pool.ImportTimelineFromFile(str(edl), {
                "timelineName": fixture, "importSourceClips": False,
                "sourceClipsFolders": bridge.media_pool_folders(
                    pool.GetRootFolder())})
            check(timeline, f"EDL refusée : {fixture}")
            start = timeline.GetStartFrame()
            placed, transitions = [], []
            for item in timeline.GetItemListInTrack("video", 1):
                media = item.GetMediaPoolItem()
                if media:
                    check(media.GetClipProperty("File Path") ==
                          str(folder / item.GetName()), "Mauvais rush EDL")
                    placed.append((item.GetName(), item.GetStart() - start,
                                   item.GetEnd() - start,
                                   item.GetSourceStartFrame(),
                                   item.GetSourceEndFrame(),
                                   item.GetSpeed()["Percentage"]))
                else:
                    transitions.append((item.GetStart() - start,
                                        item.GetEnd() - start))
            check(placed == expected_clips, f"Plans EDL différents : {placed}")
            check(transitions == ([(0, 10), (95, 105), (175, 200)]
                                  if fixture == "fondu_25" else []),
                  f"Fondus EDL différents : {transitions}")
            report["edl"][fixture] = {"clips": placed,
                                      "transitions": transitions}
        (folder / "report.json").write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(f"Vérifié dans Resolve {report['resolve']} : {folder / 'report.json'}")
    finally:
        manager.CloseProject(project)
        manager.DeleteProject(name)
        if previous_name:
            check(manager.LoadProject(previous_name),
                  f"Impossible de rouvrir le projet précédent : {previous_name}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Vérifie Basic Motion, le garde-fou SRT et les EDL dans "
                    "un projet Resolve Studio jetable. Le projet précédent "
                    "est restauré ; les preuves restent dans le dossier donné.")
    parser.add_argument("folder", help="Nouveau dossier pour les preuves")
    parser.add_argument("--binary", default=str(REPO / "build" / "cutmachine"))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    run(parser.parse_args())
