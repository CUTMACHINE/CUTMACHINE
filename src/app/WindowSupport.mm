// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The helpers
// the window's files share (AppDelegatePrivate.h declares them): titles and
// names, the caption picture, the tools' names, keyboard shortcuts, the
// timeline navigation trace, decoder opens, the clipboard of clips, system
// symbols, the setup notice, timecodes. Moved out of main.mm unchanged.
#include "AppDelegatePrivate.h"
#include "Generation.h"
#include "LinkGroupRepair.h"
#include "PersonMatte.h"
#include "ProcessLauncher.h"
#include "ProjectSwitch.h"
#include "WorkspaceActivity.h"

namespace cutmachine_app {

// VQA1 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. NSString's `%s` conversion
// uses the process C-string encoding, while every persisted CUTMACHINE string
// is UTF-8. Keep that boundary explicit so accented project and timeline names
// survive every title refresh.
NSString* CMStringFromUtf8(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

NSString* CMStringFromUtf8(const char* text) {
    return text ? [NSString stringWithUTF8String:text] ?: @"" : @"";
}

NSString* CMRecordTitle(const std::string& sequenceName) {
    return [NSString
        stringWithFormat:@"RECORD — %@", CMStringFromUtf8(sequenceName)];
}

NSString* CMWindowTitle(const std::string& projectName,
                        const std::string& sequenceName) {
    return [NSString stringWithFormat:@"CUTMACHINE — %@ — %@",
                                      CMStringFromUtf8(projectName),
                                      CMStringFromUtf8(sequenceName)];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. A name typed in a form, as the
// forms have always read it: without the spaces and line breaks around it.
NSString* CMTrimmedName(NSString* typed) {
    return [typed ?: @""
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
}

// What the timeline forms say of an empty name, which used to close them
// and create nothing.
NSString* const kTimelineNameMissing = @"Donnez un nom à la timeline.";

// LG5 -- a picture of the caption renderer as Core Graphics shows it: its
// premultiplied RGBA, its colours sRGB as the export writes them. +1.
CGImageRef CMCaptionPicture(const CaptionImage& image) {
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) *
                                   static_cast<size_t>(image.height) * 4)
        return nullptr;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, image.pixels.data(),
                                  static_cast<CFIndex>(image.pixels.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef picture = CGImageCreate(
        static_cast<size_t>(image.width), static_cast<size_t>(image.height), 8,
        32, static_cast<size_t>(image.width) * 4, space,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big, provider,
        nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
    return picture;
}

// PERF10 -- what the frame cache keeps once playback stops: a few seconds
// of HD, a dozen 4K frames around the playhead.
NSPasteboardType const kCutmachineMediaPasteboardType =
    @"com.cutmachine.library-media";
NSPasteboardType const kCutmachineMediaSelectionPasteboardType =
    @"com.cutmachine.library-media-selection";
NSPasteboardType const kCutmachineBinPasteboardType = @"com.cutmachine.bin";
NSPasteboardType const kCutmachineTimelinePasteboardType =
    @"com.cutmachine.timeline";
NSString* const kAutomaticProxyGenerationDefaultsKey =
    @"CUTMACHINEAutomaticProxyGeneration";
// DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. The window's own Whisper
// model, before it used the engine's: read once at launch, migrated into the
// settings file, and removed (WhisperModelSetting.h). Nothing else reads it.
NSString* const kWhisperModelPathDefaultsKey = @"CUTMACHINEWhisperModelPath";
NSString* const kProgramVideoScopeDefaultsKey = @"CUTMACHINEProgramVideoScope";

// The status line names the tool with the keys that choose it, as they are
// assigned: the defaults moved to Resolve's on 10 October 2026, and a key can
// be reassigned (KeyboardShortcuts.h).
NSString* ToolName(TimelineTool tool,
                   NSDictionary<NSString*, NSString*>* bindings) {
    const auto shown = [&](NSString* identifier) {
        return ui::shortcuts::DisplaySpec(
            std::string(bindings[identifier].UTF8String ?: ""));
    };
    NSString* name = @"";
    std::string keys;
    switch (tool) {
        case TimelineTool::Select:
            name = @"Sélection";
            keys = shown(@"tool.select");
            break;
        case TimelineTool::Hand:
            name = @"Main";
            keys = shown(@"tool.hand");
            break;
        case TimelineTool::Zoom:
            name = @"Zoom";
            keys = shown(@"tool.zoom");
            break;
        case TimelineTool::Cut: {
            name = @"Lame";
            keys = shown(@"tool.cut");
            const std::string alternate = shown(@"tool.cut.alternate");
            if (!alternate.empty()) {
                keys = keys.empty() ? alternate : keys + "/" + alternate;
            }
            break;
        }
        case TimelineTool::Trim:
            name = @"Trim";
            keys = shown(@"tool.trim");
            break;
    }
    if (keys.empty()) {
        return name;
    }
    return [NSString
        stringWithFormat:@"%@ (%@)", name,
                         [NSString stringWithUTF8String:keys.c_str()]];
}

bool ParseShortcutSpec(NSString* spec, NSString** key,
                       NSEventModifierFlags* modifiers, NSString** error) {
    // NSString lowercases every letter, accented ones included; the portable
    // reader only lowercases ASCII (KeyboardShortcuts.h).
    ui::shortcuts::Chord chord;
    std::string reason;
    if (!ui::shortcuts::ParseSpec(
            std::string(spec.lowercaseString.UTF8String ?: ""), chord,
            reason)) {
        if (error != nullptr) {
            *error = [NSString stringWithUTF8String:reason.c_str()];
        }
        return false;
    }
    const auto has = [&](ui::shortcuts::Modifier modifier) {
        return (chord.modifiers & modifier) != 0;
    };
    NSEventModifierFlags flags = 0;
    if (has(ui::shortcuts::kCommand)) {
        flags |= NSEventModifierFlagCommand;
    }
    if (has(ui::shortcuts::kOption)) {
        flags |= NSEventModifierFlagOption;
    }
    if (has(ui::shortcuts::kControl)) {
        flags |= NSEventModifierFlagControl;
    }
    if (has(ui::shortcuts::kShift)) {
        flags |= NSEventModifierFlagShift;
    }
    *key = [NSString stringWithUTF8String:chord.key.c_str()];
    *modifiers = flags;
    return true;
}

NSString* ShortcutSignature(NSString* spec, NSString** error) {
    NSString* key = nil;
    NSEventModifierFlags modifiers = 0;
    if (!ParseShortcutSpec(spec, &key, &modifiers, error)) return nil;
    if (key.length == 0) return @"";
    return [NSString
        stringWithFormat:@"%llu:%@", static_cast<unsigned long long>(modifiers),
                         key];
}

NSString* MenuKeyEquivalentForShortcut(NSString* key) {
    if ([key isEqualToString:@"space"]) return @" ";
    if ([key isEqualToString:@"delete"]) return @"\b";
    const int number =
        ui::shortcuts::FunctionKeyNumber(std::string(key.UTF8String ?: ""));
    if (number != 0) {
        return [NSString
            stringWithFormat:@"%C", static_cast<unichar>(NSF1FunctionKey +
                                                         number - 1)];
    }
    return key ?: @"";
}

// CH6 (review) -- docs/roadmap/CHARTE-2026-11.md. The commands of the menu
// bar that change the montage: its document, its timeline's clipboard, its
// selection, its In and Out points, its tool, its trim mode. Not what only
// moves the view or the playhead, plays, or opens a file. While the Charte
// panel holds the keyboard, none of them is for it (-validateMenuItem:,
// -charterRefusesMontageCommand:).
bool IsMontageMenuAction(SEL action) {
    static const SEL kActions[] = {
        // Édition.
        @selector(menuCopyTimelineClips:),
        @selector(menuPasteTimelineClips:),
        @selector(menuDeleteSelection:),
        @selector(menuRippleDeleteSelection:),
        // Clip.
        @selector(menuCutSelectedAtPlayhead:),
        @selector(menuToggleClipsEnabled:),
        @selector(menuMarkTimelineIn:),
        @selector(menuMarkTimelineOut:),
        @selector(menuClearTimelineRange:),
        @selector(menuInsertSource:),
        @selector(menuOverwriteSource:),
        @selector(menuActivateMulticamAngle:),
        @selector(menuClearMulticamAngle:),
        @selector(menuAutoAlignByWaveform:),
        // Couleur: what changes the document or its grade clipboard.
        @selector(applySonyColorPreset:),
        @selector(configureColorManagement:),
        @selector(disableColorManagement:),
        @selector(copyClipGrade:),
        @selector(pasteClipGrade:),
        // Timeline.
        @selector(createTimelinePressed:),
        @selector(menuSelectTool:),
        @selector(menuHandTool:),
        @selector(menuZoomTool:),
        @selector(menuCutTool:),
        @selector(menuTrimTool:),
        @selector(menuToggleSnapping:),
        @selector(menuToggleLinkedSelection:),
        @selector(menuSelectClipsAfter:),
        @selector(menuAddCrossDissolve:),
        @selector(menuRemoveCrossDissolve:),
        @selector(configureSequence:),
        @selector(transcribeTimeline:),
        @selector(transcribeTimelineHesitations:),
        @selector(menuAddMarker:),
        @selector(menuAnnotateMarker:),
        @selector(menuRemoveMarker:),
        @selector(menuEditCartonText:),
        @selector(menuAddVideoTrack:),
        @selector(menuAddAudioTrack:),
        @selector(removeEmptyTracksPressed:),
        // Trim.
        @selector(menuTrimSelectNearest:),
        @selector(menuTrimCycleSide:),
        @selector(menuTrimNudgeBack:),
        @selector(menuTrimNudgeForward:),
        @selector(menuTrimNudgeBackMany:),
        @selector(menuTrimNudgeForwardMany:),
        @selector(menuTrimExtend:),
        @selector(menuTrimDynamic:),
        @selector(menuRetimeControls:),
        @selector(menuLeaveTrimMode:),
    };
    for (SEL known : kActions)
        if (known == action) return true;
    return false;
}

// Whether `item` is one of the menu bar's: the only ones a key equivalent
// reaches. A context menu is clicked on the montage itself.
bool IsMenuBarItem(NSMenuItem* item) {
    for (NSMenu* menu = item.menu; menu != nil; menu = menu.supermenu)
        if (menu == NSApp.mainMenu) return true;
    return false;
}

// PERF7 -- when the render cost report started: its lines are timed from
// here. Read by the agentic QA to find the windows of a phase it played.
int64_t gRenderCostOriginNs = 0;

// Visual QA of 1 October 2026 -- a navigation defect is reported at the
// screen by someone whose gestures the session fixing it cannot see (screen
// capture is refused to it). With CUTMACHINE_NAV_TRACE set, every scroll,
// pinch and zoom-bar drag is written to stderr with what AppKit reported and
// the view before and after, so the gesture can be read and replayed in
// numbers against TimelineView.cc. Off by default: nothing is written.
bool NavTraceEnabled() {
    static const bool enabled = std::getenv("CUTMACHINE_NAV_TRACE") != nullptr;
    return enabled;
}

double NavTraceStart(const TimelineViewport& viewport) {
    return static_cast<double>(viewport.view_start.value) /
           viewport.view_start.rate;
}

const char* NavTracePart(TimelineZoomBarPart part) {
    switch (part) {
        case TimelineZoomBarPart::LeftHandle:
            return "left-handle";
        case TimelineZoomBarPart::Body:
            return "body";
        case TimelineZoomBarPart::RightHandle:
            return "right-handle";
        case TimelineZoomBarPart::None:
            break;
    }
    return "none";
}

void ParallelForEachIndex(size_t count,
                          const std::function<void(size_t)>& body) {
    constexpr size_t kMaxConcurrentOpens = 8;
    const size_t hardware = std::max(1u, std::thread::hardware_concurrency());
    const size_t workers =
        std::min(count, std::min(hardware, kMaxConcurrentOpens));
    if (workers <= 1) {
        for (size_t index = 0; index < count; ++index) body(index);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t worker = 0; worker < workers; ++worker)
        pool.emplace_back([&] {
            for (size_t index = next.fetch_add(1); index < count;
                 index = next.fetch_add(1))
                body(index);
        });
    for (std::thread& thread : pool) thread.join();
}

bool DecodeWorkerMatchesSource(const DecodeWorker& worker,
                               const DocumentSource& source) {
    if (static_cast<int64_t>(worker.FrameRateNumerator()) * source.rate.den !=
        static_cast<int64_t>(source.rate.num) * worker.FrameRateDenominator())
        return false;
    const int64_t declaredFrames =
        source.duration.to_frames(source.rate.num, source.rate.den);
    return declaredFrames <= worker.FrameCount();
}

// PERF10. What a source's decoder opens: its proxy when proxies are on and
// the proxy file is there, else the original. Nothing for a source with no
// video to decode.
std::optional<PendingDecodeOpen> DecodeOpenFor(
    const Document& document, const DocumentSource& source,
    const std::filesystem::path& base, bool proxiesEnabled) {
    const LibraryMedia* media = document.FindLibraryMedia(source.id);
    if (media && !media->has_video) return std::nullopt;
    std::filesystem::path original(source.path);
    if (original.is_relative()) original = base / original;
    std::filesystem::path selected = original;
    if (proxiesEnabled && media && !media->proxy_path.empty()) {
        std::filesystem::path proxy(media->proxy_path);
        if (proxy.is_relative()) proxy = base / proxy;
        std::error_code existsError;
        if (std::filesystem::is_regular_file(proxy, existsError) &&
            !existsError)
            selected = proxy;
    }
    return PendingDecodeOpen{source.id, selected.lexically_normal().string(),
                             original.lexically_normal().string(),
                             selected != original};
}

// PERF10. Whether the media a decoder would open is on disk: a stat, not an
// open. A file that is there but is not the media the project declares is
// only found out when its decoder opens (DecodeWorkerMatchesSource).
bool DecodeMediaIsAvailable(const PendingDecodeOpen& item) {
    std::error_code error;
    if (std::filesystem::is_regular_file(item.selected, error)) return true;
    return item.using_proxy &&
           std::filesystem::is_regular_file(item.original, error);
}

std::vector<const DocumentTrack*> TracksOfKind(const Document& document,
                                               const std::string& kind) {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == kind) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    return tracks;
}

std::vector<TimelineClipboardClip> CopyTimelineClips(
    const Document& document, const std::vector<Ulid>& clipIds) {
    std::vector<TimelineClipboardClip> result;
    for (const Ulid& id : clipIds) {
        if (std::any_of(result.begin(), result.end(),
                        [&](const auto& item) { return item.clip.id == id; }))
            continue;
        const DocumentClip* clip = document.FindClip(id);
        const DocumentTrack* track = document.FindTrackForClip(id);
        if (!clip || !track) continue;
        const auto kindTracks = TracksOfKind(document, track->kind);
        const auto found =
            std::find(kindTracks.begin(), kindTracks.end(), track);
        result.push_back({*clip, track->id, track->kind,
                          found == kindTracks.end()
                              ? 0
                              : static_cast<size_t>(
                                    std::distance(kindTracks.begin(), found))});
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const TimelineClipboardClip& left,
                        const TimelineClipboardClip& right) {
                         if (left.clip.timeline_in != right.clip.timeline_in)
                             return left.clip.timeline_in <
                                    right.clip.timeline_in;
                         if (left.track_kind != right.track_kind)
                             return left.track_kind < right.track_kind;
                         return left.kind_ordinal < right.kind_ordinal;
                     });
    return result;
}

std::optional<PasteClipsOperation> PasteTimelineClipboardAt(
    const Document& document,
    const std::vector<TimelineClipboardClip>& clipboard, RationalTime anchor) {
    if (clipboard.empty()) return std::nullopt;
    RationalTime first = clipboard.front().clip.timeline_in;
    for (const TimelineClipboardClip& item : clipboard)
        if (item.clip.timeline_in < first) first = item.clip.timeline_in;
    PasteClipsOperation operation;
    for (const TimelineClipboardClip& item : clipboard) {
        const DocumentTrack* target = document.FindTrack(item.track_id);
        if (!target || target->kind != item.track_kind) {
            const auto tracks = TracksOfKind(document, item.track_kind);
            if (item.kind_ordinal >= tracks.size()) return std::nullopt;
            target = tracks[item.kind_ordinal];
        }
        const DocumentClip& clip = item.clip;
        operation.clips.push_back({clip.id,
                                   target->id,
                                   clip.source_id,
                                   clip.source_in,
                                   clip.duration,
                                   anchor.add(clip.timeline_in.sub(first)),
                                   {},
                                   clip.link_group_id,
                                   {},
                                   clip.sync_anchor_clip_id,
                                   {},
                                   clip.sync_reference_delta});
    }
    return operation;
}

std::optional<PasteClipsOperation> PasteTimelineClipboardAtMoves(
    const std::vector<TimelineClipboardClip>& clipboard,
    const TimelineMovePreview& preview) {
    if (clipboard.empty() || !preview.valid) return std::nullopt;
    std::map<Ulid, std::pair<Ulid, RationalTime>> destinations;
    for (const LinkedClipMove& move : preview.linked_moves)
        destinations[move.clip_id] = {move.track_id, move.timeline_in};
    if (destinations.empty())
        destinations[preview.clip_id] = {preview.target_track_id,
                                         preview.timeline_in};
    PasteClipsOperation operation;
    for (const TimelineClipboardClip& item : clipboard) {
        const auto destination = destinations.find(item.clip.id);
        if (destination == destinations.end()) return std::nullopt;
        const DocumentClip& clip = item.clip;
        operation.clips.push_back({clip.id,
                                   destination->second.first,
                                   clip.source_id,
                                   clip.source_in,
                                   clip.duration,
                                   destination->second.second,
                                   {},
                                   clip.link_group_id,
                                   {},
                                   clip.sync_anchor_clip_id,
                                   {},
                                   clip.sync_reference_delta});
    }
    return operation;
}

NSImage* SystemSymbol(NSString* name, NSString* description,
                      CGFloat pointSize) {
    NSImage* image = [NSImage imageWithSystemSymbolName:name
                               accessibilityDescription:description];
    if (!image) return nil;
    NSImageSymbolConfiguration* configuration = [NSImageSymbolConfiguration
        configurationWithPointSize:pointSize
                            weight:NSFontWeightMedium];
    return [image imageWithSymbolConfiguration:configuration];
}

// DEC23 -- the Whisper model is the engine's setting, which the report
// reads: the window has no model of its own to add.
SetupNoticeResult GatherSetupNotice() {
    SetupNoticeResult result;
    setup::Machine machine = setup::CurrentMachine();
    // AL4 -- the window does not read 3 GB at each launch to verify the
    // hesitations weights: it says what this process already knows.
    machine.hesitations_weights = [](const std::string& path) {
        return KnownWhisperDecodingModel(path);
    };
    result.report = setup::CheckSetup(machine);
    NSString* settings = CMStringFromUtf8(machine.settings_file);
    if (settings.length > 0)
        result.facts.settings_file =
            settings.stringByAbbreviatingWithTildeInPath.UTF8String ?: "";
    result.facts.agent_key_saved = CMAgentAnthropicKeySaved();
    const BuildInfo& build = CurrentBuild();
    result.facts.build = build.version + ", commit " + build.commit +
                         (build.dirty ? " (modifié)" : "");
    return result;
}

NSString* TimeString(const RationalTime& time) {
    return [NSString stringWithFormat:@"%lld/%d",
                                      static_cast<long long>(time.value),
                                      time.rate];
}

// Delegates to Timecode.h's plain-C++ FormatTimecode so this label and the
// timeline toolbar format HH:MM:SS:FF identically
// instead of maintaining two copies of the same rule.
NSString* TimelineTimecode(const RationalTime& time, MediaRate rate) {
    return [NSString stringWithUTF8String:FormatTimecode(time, rate).c_str()];
}

DeleteGapOperation GapDeleteOperationForSelection(
    const Document& document, const TimelineGapSelection& gap,
    bool linkedSelection) {
    DeleteGapOperation operation{gap.track_id, gap.start, gap.duration, {}};
    if (!linkedSelection) return operation;
    const DocumentTrack* track = document.FindTrack(gap.track_id);
    if (!track) return operation;
    const RationalTime gapEnd = gap.start.add(gap.duration);
    const DocumentClip* following = nullptr;
    for (const DocumentClip& clip : track->clips) {
        if (clip.timeline_in < gapEnd) continue;
        if (!following || clip.timeline_in < following->timeline_in)
            following = &clip;
    }
    if (!following || following->link_group_id.empty()) return operation;
    for (const Ulid& clipId :
         ExpandLinkedClipSelection(document, {following->id})) {
        const DocumentTrack* linkedTrack = document.FindTrackForClip(clipId);
        if (!linkedTrack || linkedTrack->id == gap.track_id ||
            std::find(operation.linked_track_ids.begin(),
                      operation.linked_track_ids.end(),
                      linkedTrack->id) != operation.linked_track_ids.end())
            continue;
        operation.linked_track_ids.push_back(linkedTrack->id);
    }
    return operation;
}

}  // namespace cutmachine_app
