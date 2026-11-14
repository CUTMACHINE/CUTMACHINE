"""Measures how Resolve blends a clip at 50 % opacity with the black under
it: in the encoding of its timeline, or in linear light. One clip of grey
bars, a still at opacity 100 and one at 50, per colour setting."""
import json, os, sys, time
API = "/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting"
LIB = "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fusionscript.so"
os.environ.setdefault("RESOLVE_SCRIPT_API", API)
os.environ.setdefault("RESOLVE_SCRIPT_LIB", LIB)
sys.path.append(os.path.join(API, "Modules"))
import DaVinciResolveScript as bmd

OUT = sys.argv[1]
MEDIA = sys.argv[2]
PROJECT = "CUTMACHINE sonde composition"

def say(*a):
    print(*a, flush=True)

resolve = None
for attempt in range(60):
    resolve = bmd.scriptapp("Resolve")
    if resolve is not None and resolve.GetProjectManager() is not None:
        break
    time.sleep(3)
if resolve is None:
    sys.exit("Resolve does not answer")
say("resolve", resolve.GetVersionString(), resolve.GetProductName())
pm = resolve.GetProjectManager()
current = pm.GetCurrentProject()
say("current project", current.GetName() if current else None)
names = pm.GetProjectListInCurrentFolder() or []
project = pm.LoadProject(PROJECT) if PROJECT in names else pm.CreateProject(PROJECT)
if project is None:
    sys.exit("the measurement project could not be opened")
say("project", project.GetName())
mp = project.GetMediaPool()
timeline = project.GetCurrentTimeline()
if timeline is None:
    project.SetSetting("timelineFrameRate", "24")
    items = mp.ImportMedia([MEDIA])
    say("imported", len(items or []))
    timeline = mp.CreateTimelineFromClips("barres", items)
if timeline is None:
    sys.exit("no timeline")
say("timeline", timeline.GetName(), timeline.GetStartTimecode())
clips = timeline.GetItemListInTrack("video", 1) or []
say("clips on V1", len(clips))
item = clips[0]
resolve.OpenPage("color")
time.sleep(2)
say("page", resolve.GetCurrentPage())
start = timeline.GetStartFrame()
say("set timecode", timeline.SetCurrentTimecode("01:00:01:00"), timeline.GetCurrentTimecode())

KEYS = ("colorScienceMode", "isAutoColorManage", "rcmPresetMode",
        "separateColorSpaceAndGamma", "colorSpaceInput", "colorSpaceInputGamma",
        "colorSpaceTimeline", "colorSpaceTimelineGamma", "colorSpaceOutput",
        "colorSpaceOutputGamma", "timelineWorkingLuminanceMode", "inputDRT",
        "outputDRT")
PASSES = [
    ("yrgb", {"colorScienceMode": "davinciYRGB"}),
    ("rcm-intermediate", {"colorScienceMode": "davinciYRGBColorManagedv2",
                          "isAutoColorManage": "0",
                          "colorSpaceInput": "Rec.709 Gamma 2.4",
                          "colorSpaceTimeline": "DaVinci WG/Intermediate",
                          "colorSpaceOutput": "Rec.709 Gamma 2.4",
                          "inputDRT": "None", "outputDRT": "None"}),
    ("rcm-rec709", {"colorScienceMode": "davinciYRGBColorManagedv2",
                    "isAutoColorManage": "0",
                    "colorSpaceInput": "Rec.709 Gamma 2.4",
                    "colorSpaceTimeline": "Rec.709 Gamma 2.4",
                    "colorSpaceOutput": "Rec.709 Gamma 2.4",
                    "inputDRT": "None", "outputDRT": "None"}),
]
record = {}
for name, settings in PASSES:
    taken = {}
    for key, value in settings.items():
        taken[key] = project.SetSetting(key, value)
    now = project.GetSettings() or {}
    record[name] = {"set": taken, "now": {k: now.get(k) for k in KEYS}}
    say(name, "set", taken)
    say(name, "now", record[name]["now"])
    for opacity in (100.0, 50.0):
        ok = item.SetProperty("Opacity", opacity)
        time.sleep(1.5)
        path = os.path.join(OUT, f"{name}-{int(opacity)}.png")
        if os.path.exists(path):
            os.remove(path)
        done = project.ExportCurrentFrameAsStill(path)
        say(name, "opacity", opacity, "set", ok, "read back", item.GetProperty("Opacity"), "still", done, os.path.exists(path))
item.SetProperty("Opacity", 100.0)
project.SetSetting("colorScienceMode", "davinciYRGB")
json.dump(record, open(os.path.join(OUT, "settings.json"), "w"), indent=1)
say("saved", pm.SaveProject())
say("done")
