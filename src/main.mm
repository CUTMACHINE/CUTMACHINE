// PRD8 -- docs/roadmap/PRODUIT-2026-10.md. The includes, the window's state
// and the interfaces are in AppDelegatePrivate.h, which the window test
// harness (UiSmokeHarness.mm) shares; the code is here.
#include "AppDelegatePrivate.h"
#include "CliMain.h"
#include "Generation.h"
#include "LinkGroupRepair.h"
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

constexpr size_t kGlobalCacheBudget = 2000000000ULL;  // 2.0 GB, all sources.
// PERF10 -- what the frame cache keeps once playback stops: a few seconds
// of HD, a dozen 4K frames around the playhead.
constexpr size_t kRestingCacheBytes = 400000000ULL;
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

constexpr NSEventModifierFlags kShortcutModifierMask =
    NSEventModifierFlagCommand | NSEventModifierFlagOption |
    NSEventModifierFlagControl | NSEventModifierFlagShift;

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

std::array<float, 3> ClipColor(const Ulid& sourceId, bool audio) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char byte : sourceId) {
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    const float variation = static_cast<float>(hash & 0xff) / 255.0f;
    const ui::theme::Color base =
        audio ? ui::theme::kTrackAudio : ui::theme::kTrackVideo;
    const float lift = (variation - 0.5f) * 0.06f;
    return {std::clamp(base.r + lift, 0.0f, 1.0f),
            std::clamp(base.g + lift, 0.0f, 1.0f),
            std::clamp(base.b + lift, 0.0f, 1.0f)};
}

std::string RushDisplayName(const Project& project, const LibraryMedia& media) {
    if (const ProjectBinMetadata* metadata = project.FindBinMetadata(media.id);
        metadata && !metadata->display_name.empty())
        return metadata->display_name;
    return media.filename;
}

std::string TimelineClipName(const Document& document, const Project& project,
                             const Ulid& sourceId) {
    std::string name;
    if (const LibraryMedia* media = document.FindLibraryMedia(sourceId))
        name = RushDisplayName(project, *media);
    if (name.empty()) {
        if (const DocumentSource* source = document.FindSource(sourceId))
            name = std::filesystem::path(source->path).filename().string();
    }
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

// PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. The name a clip carries on the
// timeline: a motion clip its composition's (MOTION-2026-09 M4), a carton its
// words, any other its rush's. Drawn by -timelineRenderData and read by
// VoiceOver (-timelineAccessibilityValue): one rule, so that what is heard is
// what is seen. `sourceId` is the source the timeline resolved for the clip.
std::string TimelineDrawnClipName(const Document& document,
                                  const Project& project,
                                  const DocumentClip* clip,
                                  const Ulid& sourceId) {
    const DocumentComposition* composition =
        clip && IsMotionClip(*clip)
            ? document.FindComposition(clip->composition_id)
            : nullptr;
    if (composition != nullptr) return composition->name;
    if (clip && sourceId.empty()) return clip->caption_text;
    return TimelineClipName(document, project, sourceId);
}

NSImage* SystemSymbol(NSString* name, NSString* description,
                      CGFloat pointSize = 13.0) {
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
    const setup::Machine machine = setup::CurrentMachine();
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

// D2 -- docs/roadmap/DIALOGUE-2026-10.md. A marker's colour is document
// data, `#rrggbb` by default; the document accepts any short text there, so
// what an agent wrote that is not a colour falls back on the theme's.
ui::theme::Color MarkerDisplayColor(const DocumentMarker& marker) {
    TransitionColor parsed;
    std::string error;
    if (!ParseTransitionColor(marker.color, parsed, error))
        return ui::theme::kMarker;
    return {static_cast<float>(parsed.red) / 255.0f,
            static_cast<float>(parsed.green) / 255.0f,
            static_cast<float>(parsed.blue) / 255.0f, 1.0f};
}

std::string SyncDriftLabel(const RationalTime& drift, MediaRate frameRate) {
    const int64_t frames = drift.to_frames(frameRate.num, frameRate.den);
    const RationalTime frameQuantized{
        frames * static_cast<int64_t>(frameRate.den), frameRate.num};
    int64_t value = frames;
    const char* unit = "f";
    if (frameQuantized != drift) {
        value = drift.to_frames(48000);
        unit = "smp";
    }
    return std::string(value > 0 ? "+" : "") + std::to_string(value) + unit;
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

@interface CutmachineSplitView : NSSplitView
@end

@implementation CutmachineSplitView
- (CGFloat)dividerThickness {
    return 7.0;
}
- (void)drawDividerInRect:(NSRect)rect {
    [CMThemeColor(ui::theme::kSurfaceBase) setFill];
    NSRectFill(rect);
    [CMThemeColor(ui::theme::kBorderStrong) setFill];
    if (self.isVertical) {
        const CGFloat x = NSMidX(rect) - 0.5;
        NSRectFill(NSMakeRect(x, NSMidY(rect) - 18.0, 1.0, 36.0));
    } else {
        const CGFloat y = NSMidY(rect) - 0.5;
        NSRectFill(NSMakeRect(NSMidX(rect) - 24.0, y, 48.0, 1.0));
    }
}
@end

@implementation TimelineMetalView {
    NSTrackingArea* _trackingArea;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame]))
        [self registerForDraggedTypes:@[
            kCutmachineMediaPasteboardType,
            kCutmachineMediaSelectionPasteboardType,
            kCutmachineEffectPasteboardType
        ]];
    return self;
}
- (id)accessibilityValue {
    return self.accessibilityValueProvider ? self.accessibilityValueProvider()
                                           : [super accessibilityValue];
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)setFrameSize:(NSSize)newSize {
    const BOOL changed = !NSEqualSizes(self.frame.size, newSize);
    [super setFrameSize:newSize];
    if (changed) [self.resizeTarget timelineMetalViewDidResize:self];
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited |
                     NSTrackingMouseMoved | NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (NSView*)hitTest:(NSPoint)point {
    // Monitor controls remain interactive while passive labels stay visual
    // overlays and editing gestures remain owned by the Metal surface.
    NSView* child = [super hitTest:point];
    // Modern AppKit controls can return a private content subview from
    // -hitTest:. Walk back to the public control instead of handing the click
    // to the Metal view, which otherwise swallows Insert/Overwrite actions.
    for (NSView* candidate = child; candidate && candidate != self;
         candidate = candidate.superview) {
        if ([candidate isKindOfClass:NSPopUpButton.class] ||
            [candidate isKindOfClass:NSButton.class])
            return candidate;
    }
    // `point` is expressed in the superview's coordinates. Comparing it to
    // `bounds` only happened to work for monitor views placed at (0, 0); the
    // timeline has a non-zero frame origin, so every click and drag was
    // rejected. Let NSView's hit test decide whether the point is inside and
    // redirect passive overlay labels back to the Metal interaction surface.
    return child ? self : nil;
}
- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    [self.eventTarget timelineMouseDown:event];
}
- (void)mouseDragged:(NSEvent*)event {
    [self.eventTarget timelineMouseDragged:event];
}
- (void)mouseUp:(NSEvent*)event {
    [self.eventTarget timelineMouseUp:event];
}
- (void)mouseMoved:(NSEvent*)event {
    [self.eventTarget timelineMouseMoved:event];
}
- (void)mouseExited:(NSEvent*)event {
    [self.eventTarget timelineMouseMoved:event];
}
- (void)scrollWheel:(NSEvent*)event {
    [self.eventTarget timelineScroll:event];
}
- (void)magnifyWithEvent:(NSEvent*)event {
    [self.eventTarget timelineMagnify:event];
}
- (void)keyDown:(NSEvent*)event {
    if (![self.eventTarget timelineKeyDown:event]) [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    [self.eventTarget timelineKeyUp:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    return [self.eventTarget timelineMenuForEvent:event];
}
// EF1 -- the effect a drag carries, on the timeline only.
- (NSString*)cmDraggedEffectId:(id<NSDraggingInfo>)sender {
    if (!self.acceptsEffects) return nil;
    return [sender.draggingPasteboard
        stringForType:kCutmachineEffectPasteboardType];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if (NSString* effectId = [self cmDraggedEffectId:sender])
        return [self.eventTarget
            timelineDragEffectId:effectId
                     atViewPoint:[self convertPoint:sender.draggingLocation
                                           fromView:nil]];
    return [[sender draggingPasteboard] availableTypeFromArray:@[
        kCutmachineMediaSelectionPasteboardType, kCutmachineMediaPasteboardType
    ]]
               ? NSDragOperationCopy
               : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    if (NSString* effectId = [self cmDraggedEffectId:sender])
        return [self.eventTarget
            timelineDragEffectId:effectId
                     atViewPoint:[self convertPoint:sender.draggingLocation
                                           fromView:nil]];
    return [[sender draggingPasteboard] availableTypeFromArray:@[
        kCutmachineMediaSelectionPasteboardType, kCutmachineMediaPasteboardType
    ]]
               ? NSDragOperationCopy
               : NSDragOperationNone;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender {
    if ([self cmDraggedEffectId:sender])
        [self.eventTarget timelineEffectDragEnded];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (NSString* effectId = [self cmDraggedEffectId:sender])
        return [self.eventTarget
            timelineDropEffectId:effectId
                     atViewPoint:[self convertPoint:sender.draggingLocation
                                           fromView:nil]];
    NSPasteboard* pasteboard = sender.draggingPasteboard;
    NSArray<NSString*>* mediaIds = [pasteboard
        propertyListForType:kCutmachineMediaSelectionPasteboardType];
    if (![mediaIds isKindOfClass:NSArray.class] || mediaIds.count == 0) {
        NSString* mediaId =
            [pasteboard stringForType:kCutmachineMediaPasteboardType];
        mediaIds = mediaId ? @[ mediaId ] : @[];
    }
    if (mediaIds.count == 0) return NO;
    const NSPoint point = [self convertPoint:sender.draggingLocation
                                    fromView:nil];
    return [self.eventTarget timelineDropMediaIds:mediaIds atViewPoint:point];
}
@end

@interface VideoFullscreenWindow : NSWindow
@end

@implementation VideoFullscreenWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

@interface ContextOutlineView : NSOutlineView
@end
@implementation ContextOutlineView
// PRD37 -- ROADMAP.md. A bin's cell ends at the list's visible edge. The
// outline is wider than the list it scrolls in (the rows' insets, the
// indentation: 144 points of outline in a list of 112, measured on 3
// October 2026), and a name left to that width ran past the edge and was
// cut there, « 1_RUSH », instead of ending in an ellipsis.
- (NSRect)frameOfCellAtColumn:(NSInteger)column row:(NSInteger)row {
    NSRect frame = [super frameOfCellAtColumn:column row:row];
    NSClipView* clip = self.enclosingScrollView.contentView;
    if (clip == nil) return frame;
    const CGFloat edge = NSMaxX([self convertRect:clip.bounds fromView:clip]);
    if (NSMaxX(frame) > edge)
        frame.size.width = std::max<CGFloat>(0.0, edge - NSMinX(frame));
    return frame;
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0)
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    else {
        for (NSInteger candidate = 0; candidate < self.numberOfRows;
             ++candidate) {
            if ([[self itemAtRow:candidate] isEqual:@"__root__"]) {
                [self selectRowIndexes:[NSIndexSet indexSetWithIndex:candidate]
                    byExtendingSelection:NO];
                break;
            }
        }
    }
    return [super menuForEvent:event];
}
@end

@interface ContextTableView : NSTableView
@end
@implementation ContextTableView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0 && ![self.selectedRowIndexes containsIndex:row])
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    return [super menuForEvent:event];
}
- (void)mouseDown:(NSEvent*)event {
    NSView* content = self.window.contentView;
    NSView* hit = [content hitTest:[content convertPoint:event.locationInWindow
                                                fromView:nil]];
    for (NSView* candidate = hit; candidate && candidate != self;
         candidate = candidate.superview) {
        if (![candidate isKindOfClass:NSTextField.class]) continue;
        NSTextField* field = (NSTextField*)candidate;
        if (![field.identifier hasPrefix:@"browser:"] || !field.editable) break;
        const NSInteger row = [self rowForView:field];
        if (row >= 0)
            [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
        [field selectText:field];
        return;
    }
    [super mouseDown:event];
}
@end

// PRD37 -- ROADMAP.md. A cell whose label follows its row's selection, in
// the rows the panels draw themselves (CMIndustrialTableRowView): the
// primary text on the selection, the secondary grey elsewhere (UiTheme.h,
// ListRowLabel). The bin list kept a grey name on AppKit's blue selection,
// 2.05:1 (QA of 3 October 2026).
@interface CMListCellView : NSTableCellView
- (void)cmShowRowSelected:(BOOL)selected;
@end

@implementation CMListCellView
- (void)cmShowRowSelected:(BOOL)selected {
    self.textField.textColor = CMThemeColor(ui::theme::ListRowLabel(selected));
}
@end

@implementation CMIndustrialTableRowView {
    NSTrackingArea* _trackingArea;
    BOOL _hovered;
    CALayer* _hoverEdge;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    if ((self = [super initWithFrame:frameRect])) {
        self.wantsLayer = YES;
        _hoverEdge = [CALayer layer];
        _hoverEdge.backgroundColor = CMThemeColor(ui::theme::kAccent).CGColor;
        [self.layer addSublayer:_hoverEdge];
    }
    return self;
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    _hovered = YES;
    self.needsDisplay = YES;
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    _hovered = NO;
    self.needsDisplay = YES;
}

- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    for (NSView* view in self.subviews)
        if ([view isKindOfClass:CMListCellView.class])
            [(CMListCellView*)view cmShowRowSelected:selected];
    self.needsDisplay = YES;
}

// PRD37 -- a cell placed in a row already selected takes its state too.
- (void)didAddSubview:(NSView*)subview {
    [super didAddSubview:subview];
    if ([subview isKindOfClass:CMListCellView.class])
        [(CMListCellView*)subview cmShowRowSelected:self.selected];
}

- (void)drawSelectionInRect:(NSRect)dirtyRect {
    (void)dirtyRect;
}

- (void)updateLayer {
    const ui::theme::Color background =
        self.selected ? ui::theme::kListRowSelected
        : _hovered    ? ui::theme::Mix(ui::theme::kSurfacePanel,
                                       ui::theme::kTextPrimary, 0.05f)
                      : ui::theme::kSurfacePanel;
    self.layer.backgroundColor = CMThemeColor(background).CGColor;
    _hoverEdge.frame = CGRectMake(0.0, 0.0, 2.0, self.bounds.size.height);
    _hoverEdge.hidden = !_hovered || self.selected;
}

@end

@implementation ContextCollectionView
- (NSIndexPath*)iconIndexPathAtPoint:(NSPoint)point {
    // The flow layout and its flipped document view can disagree about the Y
    // axis. Resolve from actual visible item views: these are the rectangles
    // AppKit displayed and the user can physically click.
    for (NSCollectionViewItem* item in self.visibleItems) {
        NSIndexPath* path = [self indexPathForItem:item];
        if (!path) continue;
        const NSRect frame = [item.view convertRect:item.view.bounds
                                             toView:self];
        if (NSPointInRect(point, frame)) return path;
    }
    return nil;
}
- (BOOL)point:(NSPoint)point isInLabelAtIndexPath:(NSIndexPath*)indexPath {
    NSCollectionViewItem* item = [self itemAtIndexPath:indexPath];
    if (!item.textField) return NO;
    const NSRect labelRect = [item.textField convertRect:item.textField.bounds
                                                  toView:self];
    return NSPointInRect(point, labelRect);
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSIndexPath* indexPath =
        [self iconIndexPathAtPoint:[self convertPoint:event.locationInWindow
                                             fromView:nil]];
    if (indexPath && ![self.selectionIndexPaths containsObject:indexPath])
        self.selectionIndexPaths = [NSSet setWithObject:indexPath];
    return [super menuForEvent:event];
}
- (void)mouseDown:(NSEvent*)event {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) gUiSmokeIconMouseDown = true;
#endif
    const NSPoint point = [self convertPoint:event.locationInWindow
                                    fromView:nil];
    // Always prefer current on-screen geometry. forwardedMouseDownPath is
    // only a fallback because NSCollectionView can reuse an item's view
    // across reloadData, leaving a previously forwarded path stale.
    NSIndexPath* path =
        [self iconIndexPathAtPoint:point] ?: self.forwardedMouseDownPath;
    self.forwardedMouseDownPath = nil;
    if (!path) {
        self.selectionIndexPaths = [NSSet set];
        return;
    }
    const BOOL renameClick =
        event.clickCount == 1 && [self point:point isInLabelAtIndexPath:path];
    NSSet<NSIndexPath*>* previous = self.selectionIndexPaths;
    const BOOL commandPressed =
        (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    const BOOL shiftPressed =
        (event.modifierFlags & NSEventModifierFlagShift) != 0;
    const BOOL deferSingleSelection = !commandPressed && !shiftPressed &&
                                      previous.count > 1 &&
                                      [previous containsObject:path];
    NSSet<NSIndexPath*>* selection = [NSSet setWithObject:path];
    if (deferSingleSelection) {
        // Keep the group intact until AppKit distinguishes a click from a
        // drag. A plain mouse-up still collapses to the clicked item.
        selection = previous;
    } else if (shiftPressed && self.allowsMultipleSelection &&
               (self.rangeSelectionAnchor || previous.count > 0)) {
        NSIndexPath* anchor = self.rangeSelectionAnchor ?: previous.anyObject;
        const NSUInteger first = std::min(anchor.item, path.item);
        const NSUInteger last = std::max(anchor.item, path.item);
        NSMutableSet<NSIndexPath*>* range =
            commandPressed ? [previous mutableCopy] : [NSMutableSet set];
        for (NSUInteger item = first; item <= last; ++item)
            [range addObject:[NSIndexPath indexPathForItem:item inSection:0]];
        selection = range;
    } else if (commandPressed && self.allowsMultipleSelection) {
        NSMutableSet<NSIndexPath*>* toggled = [previous mutableCopy];
        if ([toggled containsObject:path])
            [toggled removeObject:path];
        else
            [toggled addObject:path];
        selection = toggled;
    }
    self.selectionIndexPaths = selection;
    if (!shiftPressed) self.rangeSelectionAnchor = path;
    NSSet<NSIndexPath*>* added = [selection
        objectsPassingTest:^BOOL(NSIndexPath* candidate, BOOL* stop) {
          (void)stop;
          return ![previous containsObject:candidate];
        }];
    if (!renameClick && added.count > 0 &&
        [self.delegate respondsToSelector:@selector(collectionView:
                                              didSelectItemsAtIndexPaths:)])
        [self.delegate collectionView:self didSelectItemsAtIndexPaths:added];

    const NSEventMask trackingMask =
        NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp;
    while (true) {
        NSEvent* next = [self.window nextEventMatchingMask:trackingMask];
        if (!next || next.type == NSEventTypeLeftMouseUp) {
            if (deferSingleSelection) {
                self.selectionIndexPaths = [NSSet setWithObject:path];
                self.rangeSelectionAnchor = path;
            }
            if (renameClick) {
                NSCollectionViewItem* item = [self itemAtIndexPath:path];
                if (item.textField.editable) [item.textField selectText:self];
            } else if (event.clickCount == 2 && self.doubleClickAction) {
                [NSApp sendAction:self.doubleClickAction
                               to:self.doubleClickTarget
                             from:self];
            }
            return;
        }
        const NSPoint current = [self convertPoint:next.locationInWindow
                                          fromView:nil];
        if (std::hypot(current.x - point.x, current.y - point.y) < 4.0)
            continue;
        if ([self.delegate
                respondsToSelector:@selector(collectionView:
                                       canDragItemsAtIndexPaths:withEvent:)] &&
            ![self.delegate collectionView:self
                  canDragItemsAtIndexPaths:selection
                                 withEvent:event])
            continue;
        if (![self.delegate
                respondsToSelector:@selector(collectionView:
                                       pasteboardWriterForItemAtIndexPath:)])
            continue;
        id<NSPasteboardWriting> writer = [self.delegate collectionView:self
                                    pasteboardWriterForItemAtIndexPath:path];
        if (!writer) continue;
        if ([writer isKindOfClass:NSPasteboardItem.class] &&
            selection.count > 1) {
            NSArray<NSIndexPath*>* ordered = [selection.allObjects
                sortedArrayUsingComparator:^NSComparisonResult(
                    NSIndexPath* left, NSIndexPath* right) {
                  if (left.item < right.item) return NSOrderedAscending;
                  if (left.item > right.item) return NSOrderedDescending;
                  return NSOrderedSame;
                }];
            NSMutableArray<NSString*>* mediaIds = [NSMutableArray array];
            for (NSIndexPath* selectedPath in ordered) {
                id<NSPasteboardWriting> selectedWriter =
                    [self.delegate collectionView:self
                        pasteboardWriterForItemAtIndexPath:selectedPath];
                if (![selectedWriter isKindOfClass:NSPasteboardItem.class])
                    continue;
                NSString* identifier = [(NSPasteboardItem*)selectedWriter
                    stringForType:kCutmachineMediaPasteboardType];
                if (identifier) [mediaIds addObject:identifier];
            }
            if (mediaIds.count > 1)
                [(NSPasteboardItem*)writer
                    setPropertyList:mediaIds
                            forType:kCutmachineMediaSelectionPasteboardType];
#if defined(CUTMACHINE_UI_SMOKE_TEST)
            if (gUiSmokeTesting) gUiSmokeMediaDragCount = mediaIds.count;
#endif
        }
        NSCollectionViewItem* collectionItem = [self itemAtIndexPath:path];
        if (!collectionItem) continue;
        NSDraggingItem* draggingItem =
            [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
        NSPoint imageOffset = NSZeroPoint;
        NSImage* image = [self draggingImageForItemsAtIndexPaths:selection
                                                       withEvent:event
                                                          offset:&imageOffset];
        [draggingItem setDraggingFrame:collectionItem.view.frame
                              contents:image];
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        if (gUiSmokeTesting) gUiSmokeIconDragSession = true;
#endif
        [self beginDraggingSessionWithItems:@[ draggingItem ]
                                      event:next
                                     source:self];
        return;
    }
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    return context == NSDraggingContextWithinApplication
               ? kMediaLocalDragOperations
               : NSDragOperationCopy;
}

#if defined(CUTMACHINE_UI_SMOKE_TEST)
// PRD8 -- ROADMAP.md. The smoke's icon gesture queues its drag and its
// mouse up behind the press, but AppKit starts the drag session only at a
// later turn of the run loop, and the session runs until a mouse up reaches
// it. When the queued one had been taken by then, the session waited for a
// real hand on the mouse: the run hung until its timeout, on main as on any
// branch (4 October 2026). Half a second into a session still running, the
// smoke presses Escape, which cancels it -- never sooner, so a queued mouse
// up that is still there ends it first, as it always did.
- (void)draggingSession:(NSDraggingSession*)session
       willBeginAtPoint:(NSPoint)screenPoint {
    if ([NSCollectionView instancesRespondToSelector:@selector
                          (draggingSession:willBeginAtPoint:)])
        [super draggingSession:session willBeginAtPoint:screenPoint];
    if (!gUiSmokeTesting) return;
    NSWindow* window = self.window;
    NSTimer* escape = [NSTimer
        timerWithTimeInterval:0.5
                      repeats:NO
                        block:^(NSTimer*) {
                          if (gUiSmokeIconDragEnded) return;
                          NSEvent* key = [NSEvent
                                         keyEventWithType:NSEventTypeKeyDown
                                                 location:NSZeroPoint
                                            modifierFlags:0
                                                timestamp:NSProcessInfo
                                                              .processInfo
                                                              .systemUptime
                                             windowNumber:window.windowNumber
                                                  context:nil
                                               characters:@"\x1b"
                              charactersIgnoringModifiers:@"\x1b"
                                                isARepeat:NO
                                                  keyCode:53];
                          [NSApp postEvent:key atStart:NO];
                        }];
    [NSRunLoop.mainRunLoop addTimer:escape forMode:NSRunLoopCommonModes];
}

- (void)draggingSession:(NSDraggingSession*)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation {
    if ([NSCollectionView instancesRespondToSelector:@selector
                          (draggingSession:endedAtPoint:operation:)])
        [super draggingSession:session
                  endedAtPoint:screenPoint
                     operation:operation];
    if (gUiSmokeTesting) gUiSmokeIconDragEnded = true;
}
#endif
@end

@implementation BrowserRenameTextField
- (ContextCollectionView*)owningCollectionView {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    return (ContextCollectionView*)ancestor;
}

- (NSTableView*)owningTableView {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:NSTableView.class])
        ancestor = ancestor.superview;
    return (NSTableView*)ancestor;
}

- (void)selectBrowserObjectForEvent:(NSEvent*)event {
    if (ContextCollectionView* collection = [self owningCollectionView]) {
        NSIndexPath* path = [collection
            iconIndexPathAtPoint:[collection convertPoint:event.locationInWindow
                                                 fromView:nil]];
        if (path) collection.selectionIndexPaths = [NSSet setWithObject:path];
        return;
    }
    if (NSTableView* table = [self owningTableView]) {
        const NSInteger row = [table rowForView:self];
        if (row >= 0)
            [table selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
    }
}

- (void)mouseDown:(NSEvent*)event {
    [self selectBrowserObjectForEvent:event];
    // A name is an explicit editing affordance. Enter the field editor on the
    // first click instead of waiting for NSTableView's delayed second click.
    if (event.type == NSEventTypeLeftMouseDown && event.clickCount == 1 &&
        self.editable) {
        [self selectText:self];
        return;
    }
    [super mouseDown:event];
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    [self selectBrowserObjectForEvent:event];
    if (ContextCollectionView* collection = [self owningCollectionView])
        return collection.menu;
    if (NSTableView* table = [self owningTableView]) return table.menu;
    return [super menuForEvent:event];
}
@end

// LG5 -- docs/roadmap/LEGENDES-2026-10.md.
@implementation CMCaptionView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // Layer-hosting: AppKit never redraws over the picture set here.
        CALayer* layer = [CALayer layer];
        // Nor animates it: a hosted layer keeps Core Animation's implicit
        // actions, and a new carton faded in over a quarter of a second
        // with the old one still showing through (window test, ITW4, 9
        // October 2026). A carton changes on its frame, as in the file.
        NSNull* none = NSNull.null;
        layer.actions = @{
            @"contents" : none,
            @"bounds" : none,
            @"position" : none,
            @"hidden" : none,
            @"onOrderIn" : none,
            @"onOrderOut" : none,
        };
        layer.contentsGravity = kCAGravityResize;
        layer.magnificationFilter = kCAFilterLinear;
        layer.minificationFilter = kCAFilterLinear;
        self.layer = layer;
        self.wantsLayer = YES;
        _captionText = @"";
        _drawnKey = @"";
    }
    return self;
}
- (NSView*)hitTest:(NSPoint)point {
    return nil;
}
- (BOOL)isAccessibilityElement {
    return !self.hidden && self.captionText.length > 0;
}
- (NSAccessibilityRole)accessibilityRole {
    return NSAccessibilityStaticTextRole;
}
- (id)accessibilityValue {
    return self.captionText;
}
@end

@implementation MediaIconView
- (NSView*)hitTest:(NSPoint)point {
    if (!NSPointInRect(point, self.bounds)) return nil;
    // One owner for the whole tile avoids a race between NSTextField's field
    // editor and NSCollectionView's selection/drag tracking. The collection
    // resolves the label rectangle after mouse-up and starts editing there.
    return self;
}
- (void)mouseDown:(NSEvent*)event {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    if (ancestor) {
        ((ContextCollectionView*)ancestor).forwardedMouseDownPath =
            self.indexPath;
        [(ContextCollectionView*)ancestor mouseDown:event];
    } else
        [super mouseDown:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    return ancestor ? [(ContextCollectionView*)ancestor menuForEvent:event]
                    : [super menuForEvent:event];
}
@end

@interface MediaIconItem : NSCollectionViewItem
@end

@implementation MediaIconItem
- (void)loadView {
    self.view =
        [[MediaIconView alloc] initWithFrame:NSMakeRect(0, 0, 132, 112)];
    NSImageView* image =
        [[NSImageView alloc] initWithFrame:NSMakeRect(14, 27, 104, 76)];
    image.imageScaling = NSImageScaleProportionallyUpOrDown;
    image.wantsLayer = YES;
    image.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfaceControl).CGColor;
    image.layer.cornerRadius = 0.0;
    NSTextField* label = [[BrowserRenameTextField alloc]
        initWithFrame:NSMakeRect(4, 4, 124, 19)];
    label.editable = NO;
    label.selectable = NO;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.alignment = NSTextAlignmentCenter;
    label.font = [NSFont systemFontOfSize:11.0];
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.imageView = image;
    self.textField = label;
    [self.view addSubview:image];
    [self.view addSubview:label];
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    self.view.layer.backgroundColor =
        selected ? [NSColor selectedContentBackgroundColor].CGColor
                 : NSColor.clearColor.CGColor;
}
@end

@implementation AppDelegate

- (NSArray<NSDictionary<NSString*, NSString*>*>*)shortcutDefinitions {
    NSMutableArray<NSDictionary<NSString*, NSString*>*>* definitions =
        [NSMutableArray array];
    for (const ui::shortcuts::Definition& definition :
         ui::shortcuts::Definitions()) {
        [definitions addObject:@{
            @"id" : @(definition.id),
            @"title" : @(definition.title),
            @"default" : @(definition.default_spec)
        }];
    }
    return definitions;
}

- (void)loadShortcutBindings {
    self.shortcutBindings = [NSMutableDictionary dictionary];
    NSDictionary* stored = [NSUserDefaults.standardUserDefaults
        dictionaryForKey:@"KeyboardShortcuts.v1"];
    for (NSDictionary* definition in [self shortcutDefinitions]) {
        NSString* identifier = definition[@"id"];
        // DEC40 -- a key stored under a command's former name still
        // belongs to it (KeyboardShortcuts.h, RenamedCommands).
        std::string spec;
        const bool found = ui::shortcuts::StoredSpecFor(
            identifier.UTF8String,
            [stored](const std::string& name, std::string& value) {
                id entry = stored[@(name.c_str())];
                if (![entry isKindOfClass:NSString.class]) return false;
                value = static_cast<NSString*>(entry).UTF8String;
                return true;
            },
            spec);
        NSString* value =
            found ? CMStringFromUtf8(spec) : definition[@"default"];
        self.shortcutBindings[identifier] = value;
    }
    self.shortcutMenuItems = [NSMutableDictionary dictionary];
}

- (BOOL)event:(NSEvent*)event matchesShortcut:(NSString*)identifier {
    NSString* expectedKey = nil;
    NSEventModifierFlags expectedModifiers = 0;
    if (!ParseShortcutSpec(self.shortcutBindings[identifier], &expectedKey,
                           &expectedModifiers, nullptr) ||
        expectedKey.length == 0)
        return NO;
    ui::shortcuts::Chord expected;
    expected.key = expectedKey.UTF8String ?: "";
    NSString* eventKey = event.charactersIgnoringModifiers.lowercaseString;
    NSEventModifierFlags actual = event.modifierFlags & kShortcutModifierMask;
    if (event.keyCode == 51) {
        eventKey = @"delete";
    }
    if (event.keyCode == 117) {  // Forward Delete
        NSString* forwardKey = nil;
        NSEventModifierFlags forwardModifiers = 0;
        ParseShortcutSpec(@(ui::shortcuts::kForwardDeleteMeans), &forwardKey,
                          &forwardModifiers, nullptr);
        eventKey = forwardKey;
        actual |= forwardModifiers;
    }
    if ([eventKey isEqualToString:@" "]) {
        eventKey = @"space";
    }
    if (eventKey.length == 1) {
        const unichar character = [eventKey characterAtIndex:0];
        if (character >= NSF1FunctionKey && character <= NSF19FunctionKey) {
            eventKey = [NSString
                stringWithFormat:@"f%d", character - NSF1FunctionKey + 1];
        }
    }
    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. A Shift the character
    // needed is not a modifier of it: the period is Shift and ";" on the
    // French layout (KeyboardShortcuts.h, TypedKeyMatches).
    NSString* unshifted = eventKey;
    if ((actual & NSEventModifierFlagShift) != 0 && eventKey.length == 1 &&
        event.keyCode != 51 && event.keyCode != 117) {
        NSString* bare = [event charactersByApplyingModifiers:0];
        if (bare.length == 1) unshifted = bare.lowercaseString;
    }
    const auto bit = [&](NSEventModifierFlags flags, NSEventModifierFlags flag,
                         ui::shortcuts::Modifier modifier) {
        return (flags & flag) != 0 ? static_cast<std::uint32_t>(modifier) : 0u;
    };
    const auto bits = [&](NSEventModifierFlags flags) {
        return bit(flags, NSEventModifierFlagCommand, ui::shortcuts::kCommand) |
               bit(flags, NSEventModifierFlagOption, ui::shortcuts::kOption) |
               bit(flags, NSEventModifierFlagControl, ui::shortcuts::kControl) |
               bit(flags, NSEventModifierFlagShift, ui::shortcuts::kShift);
    };
    expected.modifiers = bits(expectedModifiers);
    return ui::shortcuts::TypedKeyMatches(expected, eventKey.UTF8String ?: "",
                                          unshifted.UTF8String ?: "",
                                          bits(actual));
}

- (void)applyShortcutBindingsToMenus {
    [self.shortcutMenuItems
        enumerateKeysAndObjectsUsingBlock:^(NSString* identifier,
                                            NSMenuItem* item, BOOL* stop) {
          (void)stop;
          NSString* key = nil;
          NSEventModifierFlags modifiers = 0;
          ParseShortcutSpec(self.shortcutBindings[identifier], &key, &modifiers,
                            nullptr);
          item.keyEquivalent = MenuKeyEquivalentForShortcut(key);
          item.keyEquivalentModifierMask = modifiers;
        }];
    // ON3 -- the Agent's toggle says its key.
    [self refreshAgentToggle];
}

- (NSMenuItem*)shortcutMenuItem:(NSString*)title
                         action:(SEL)action
                        command:(NSString*)identifier {
    NSMenuItem* item = [self menuItem:title action:action key:@""];
    self.shortcutMenuItems[identifier] = item;
    return item;
}

- (void)keyboardKeyPressed:(NSButton*)sender {
    NSString* identifier =
        self.keyboardCommandPopup.selectedItem.representedObject;
    NSTextField* field = self.keyboardEditingFields[identifier];
    if (!field || sender.identifier.length == 0) return;
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (self.keyboardCommandModifier.state == NSControlStateValueOn)
        [parts addObject:@"Cmd"];
    if (self.keyboardOptionModifier.state == NSControlStateValueOn)
        [parts addObject:@"Alt"];
    if (self.keyboardControlModifier.state == NSControlStateValueOn)
        [parts addObject:@"Ctrl"];
    if (self.keyboardShiftModifier.state == NSControlStateValueOn)
        [parts addObject:@"Shift"];
    [parts addObject:sender.identifier];
    field.stringValue = [parts componentsJoinedByString:@"+"];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@ affecté à %@", field.stringValue,
                         self.keyboardCommandPopup.selectedItem.title];
}

- (NSButton*)keyboardModifierButton:(NSString*)title frame:(NSRect)frame {
    NSButton* button = [NSButton checkboxWithTitle:title target:nil action:nil];
    button.frame = frame;
    button.font = [NSFont systemFontOfSize:11.0 weight:NSFontWeightMedium];
    return button;
}

- (NSView*)keyboardViewForDefinitions:(NSArray*)definitions
                               fields:(NSMutableDictionary*)fields {
    NSView* keyboard =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, 255)];
    NSTextField* instruction = [NSTextField
        labelWithString:@"Choisissez une commande, activez les modificateurs "
                        @"puis cliquez sur une touche."];
    instruction.frame = NSMakeRect(0, 229, 650, 20);
    instruction.textColor = NSColor.secondaryLabelColor;
    [keyboard addSubview:instruction];

    self.keyboardEditingFields = fields;
    self.keyboardCommandPopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 197, 315, 27)];
    for (NSDictionary* definition in definitions) {
        [self.keyboardCommandPopup addItemWithTitle:definition[@"title"]];
        self.keyboardCommandPopup.lastItem.representedObject =
            definition[@"id"];
    }
    [keyboard addSubview:self.keyboardCommandPopup];
    self.keyboardCommandModifier =
        [self keyboardModifierButton:@"⌘ Cmd"
                               frame:NSMakeRect(330, 199, 72, 24)];
    self.keyboardOptionModifier =
        [self keyboardModifierButton:@"⌥ Alt"
                               frame:NSMakeRect(402, 199, 68, 24)];
    self.keyboardControlModifier =
        [self keyboardModifierButton:@"⌃ Ctrl"
                               frame:NSMakeRect(470, 199, 72, 24)];
    self.keyboardShiftModifier =
        [self keyboardModifierButton:@"⇧ Shift"
                               frame:NSMakeRect(542, 199, 85, 24)];
    for (NSButton* modifier in @[
             self.keyboardCommandModifier, self.keyboardOptionModifier,
             self.keyboardControlModifier, self.keyboardShiftModifier
         ])
        [keyboard addSubview:modifier];

    NSArray<NSArray<NSString*>*>* rows = @[
        @[
            @"1", @"2", @"3", @"4", @"5", @"6", @"7", @"8", @"9", @"0",
            @"Delete"
        ],
        @[ @"A", @"Z", @"E", @"R", @"T", @"Y", @"U", @"I", @"O", @"P" ],
        @[ @"Q", @"S", @"D", @"F", @"G", @"H", @"J", @"K", @"L", @"M" ],
        @[ @"W", @"X", @"C", @"V", @"B", @"N", @",", @".", @"Space" ]
    ];
    const CGFloat keyHeight = 35.0;
    const CGFloat gap = 5.0;
    for (NSUInteger row = 0; row < rows.count; ++row) {
        CGFloat x = row == 0 ? 0.0 : row * 12.0;
        const CGFloat y = 151.0 - row * (keyHeight + gap);
        for (NSString* key in rows[row]) {
            CGFloat width = [key isEqualToString:@"Space"]    ? 170.0
                            : [key isEqualToString:@"Delete"] ? 70.0
                                                              : 48.0;
            NSString* title = [key isEqualToString:@"Space"]    ? @"Espace"
                              : [key isEqualToString:@"Delete"] ? @"⌫"
                                                                : key;
            NSButton* button =
                [NSButton buttonWithTitle:title
                                   target:self
                                   action:@selector(keyboardKeyPressed:)];
            button.frame = NSMakeRect(x, y, width, keyHeight);
            button.identifier = key;
            button.bezelStyle = NSBezelStyleTexturedRounded;
            button.font = [NSFont systemFontOfSize:12.0
                                            weight:NSFontWeightMedium];
            [keyboard addSubview:button];
            x += width + gap;
        }
    }
    return keyboard;
}

- (void)editKeyboardShortcuts:(id)sender {
    (void)sender;
    // PRD26 -- a form already on the window refuses this one, which would
    // first take the keyboard over from it (-keyboardViewForDefinitions:
    // keeps the fields it builds).
    if ([CMFormPanel formShownInWindow:self.window]) {
        NSBeep();
        return;
    }
    NSArray* definitions = [self shortcutDefinitions];
    NSMutableDictionary<NSString*, NSTextField*>* fields =
        [NSMutableDictionary dictionary];
    const CGFloat rowHeight = 32.0;
    const CGFloat documentHeight = rowHeight * definitions.count;
    NSView* document =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, documentHeight)];
    for (NSUInteger index = 0; index < definitions.count; ++index) {
        NSDictionary* definition = definitions[index];
        const CGFloat y = documentHeight - (index + 1) * rowHeight + 4.0;
        NSTextField* label = [NSTextField labelWithString:definition[@"title"]];
        label.frame = NSMakeRect(8, y + 3, 370, 22);
        NSTextField* field =
            [[NSTextField alloc] initWithFrame:NSMakeRect(390, y, 245, 25)];
        field.identifier =
            [@"shortcut." stringByAppendingString:definition[@"id"]];
        field.stringValue = self.shortcutBindings[definition[@"id"]] ?: @"";
        field.placeholderString = @"Non assigné";
        field.toolTip = @"Exemples : A, Space, F9, Alt+X, Cmd+Shift+L";
        fields[definition[@"id"]] = field;
        [document addSubview:label];
        [document addSubview:field];
    }
    NSScrollView* scroll =
        [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 650, 300)];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.documentView = document;

    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel,
    // resizable: the list takes the room, the keyboard stays on top. The
    // check runs before the panel closes, where a second alert, « Raccourci
    // invalide », reopened the form; « Valeurs par défaut » fills the
    // fields in place, as it did by reopening it.
    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 650, 565)];
    NSView* keyboard = [self keyboardViewForDefinitions:definitions
                                                 fields:fields];
    keyboard.frame = NSMakeRect(0, 310, 650, 255);
    keyboard.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    scroll.frame = NSMakeRect(0, 0, 650, 300);
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [accessory addSubview:keyboard];
    [accessory addSubview:scroll];
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Raccourcis clavier"
              message:@"Cliquez sur le clavier ou saisissez directement une "
                      @"combinaison. Une valeur vide désactive la commande."
                 form:accessory];
    [panel addButtonWithTitle:@"Enregistrer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    NSButton* defaults = [panel addButtonWithTitle:@"Valeurs par défaut"
                                              role:CMFormButtonRoleAction];
    defaults.target = self;
    defaults.action = @selector(resetKeyboardShortcutFields:);
    panel.formResizable = YES;
    __block NSMutableDictionary<NSString*, NSString*>* accepted = nil;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      NSMutableDictionary<NSString*, NSString*>* candidate =
          [NSMutableDictionary dictionary];
      NSMutableDictionary<NSString*, NSString*>* owners =
          [NSMutableDictionary dictionary];
      for (const ui::shortcuts::ReservedChord& reserved :
           ui::shortcuts::ReservedChords()) {
          owners[ShortcutSignature(@(reserved.spec), nullptr)] =
              @(reserved.title);
      }
      NSString* validationError = nil;
      for (NSDictionary* definition in definitions) {
          NSString* identifier = definition[@"id"];
          NSString* spec = [fields[identifier].stringValue
              stringByTrimmingCharactersInSet:NSCharacterSet
                                                  .whitespaceCharacterSet];
          NSString* parseError = nil;
          NSString* signature = ShortcutSignature(spec, &parseError);
          if (!signature) {
              validationError =
                  [NSString stringWithFormat:@"%@ : %@", definition[@"title"],
                                             parseError];
              break;
          }
          if (signature.length > 0 && owners[signature]) {
              validationError = [NSString
                  stringWithFormat:@"Conflit entre « %@ » et « %@ ».",
                                   owners[signature], definition[@"title"]];
              break;
          }
          if (signature.length > 0) owners[signature] = definition[@"title"];
          candidate[identifier] = spec;
      }
      if (validationError) return validationError;
      accepted = candidate;
      return nil;
    };
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn || !accepted) return;
                  self.shortcutBindings = accepted;
                  [NSUserDefaults.standardUserDefaults
                      setObject:accepted
                         forKey:@"KeyboardShortcuts.v1"];
                  [self applyShortcutBindingsToMenus];
                  self.infoLabel.stringValue = @"Raccourcis clavier mis à jour";
                }];
}

// « Valeurs par défaut » of the shortcuts' panel: every field back to its
// default, the panel left open and its error line cleared.
- (void)resetKeyboardShortcutFields:(id)sender {
    for (NSDictionary* definition in [self shortcutDefinitions])
        self.keyboardEditingFields[definition[@"id"]].stringValue =
            definition[@"default"];
    if ([sender isKindOfClass:NSView.class] &&
        [((NSView*)sender).window isKindOfClass:CMFormPanel.class])
        [(CMFormPanel*)((NSView*)sender).window showError:nil];
}

- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action key:(NSString*)key {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:key ?: @""];
    item.target = self;
    return item;
}

- (void)installApplicationMenus {
    [self loadShortcutBindings];
    NSMenu* bar = [[NSMenu alloc] initWithTitle:@"Main"];
    NSMenuItem* appRoot = [[NSMenuItem alloc] initWithTitle:@"CUTMACHINE"
                                                     action:nil
                                              keyEquivalent:@""];
    NSMenu* app = [[NSMenu alloc] initWithTitle:@"CUTMACHINE"];
    [app addItemWithTitle:@"À propos de CUTMACHINE"
                   action:@selector(orderFrontStandardAboutPanel:)
            keyEquivalent:@""];
    [app addItem:NSMenuItem.separatorItem];
    [app addItem:[self menuItem:@"Raccourcis clavier…"
                         action:@selector(editKeyboardShortcuts:)
                            key:@","]];
    [app addItem:[self menuItem:@"Moteur IA…"
                         action:@selector(configureAiEngine:)
                            key:@""]];
    [app addItem:[self menuItem:@"Modèle Whisper…"
                         action:@selector(chooseWhisperModel:)
                            key:@""]];
    // PRD21 (window) -- what this machine lacks, read again at each opening.
    [app addItem:[self menuItem:@"Diagnostic de l'installation…"
                         action:@selector(showSetupDiagnostic:)
                            key:@""]];
    [app addItem:NSMenuItem.separatorItem];
    [app addItemWithTitle:@"Quitter CUTMACHINE"
                   action:@selector(terminate:)
            keyEquivalent:@"q"];
    appRoot.submenu = app;
    [bar addItem:appRoot];

    NSMenuItem* fileRoot = [[NSMenuItem alloc] initWithTitle:@"Fichier"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* file = [[NSMenu alloc] initWithTitle:@"Fichier"];
    // DEC39 -- change project without the welcome window.
    [file addItem:[self menuItem:@"Nouveau projet…"
                          action:@selector(newProjectFromEditor:)
                             key:@""]];
    [file addItem:[self menuItem:@"Ouvrir un projet…"
                          action:@selector(openProjectFromEditor:)
                             key:@""]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Importer des rushes…"
                          action:@selector(importMediaPressed:)
                             key:@"i"]];
    [file addItem:[self menuItem:@"Importer des sous-titres SRT…"
                          action:@selector(importSubtitlesPressed:)
                             key:@""]];
    [file addItem:[self menuItem:@"Reconnecter les médias offline…"
                          action:@selector(batchRelinkOfflineMedia:)
                             key:@""]];
    [file addItem:[self menuItem:@"Collecter le projet…"
                          action:@selector(collectPortableProject:)
                             key:@""]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Enregistrer le projet"
                          action:@selector(saveProjectPressed:)
                             key:@"s"]];
    [file addItem:NSMenuItem.separatorItem];
    [file addItem:[self menuItem:@"Exporter la vidéo finale…"
                          action:@selector(exportFinalVideo:)
                             key:@"e"]];
    [file addItem:[self menuItem:@"Exporter les sous-titres SRT…"
                          action:@selector(exportSubtitlesPressed:)
                             key:@""]];
    fileRoot.submenu = file;
    [bar addItem:fileRoot];

    NSMenuItem* editRoot = [[NSMenuItem alloc] initWithTitle:@"Édition"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* edit = [[NSMenu alloc] initWithTitle:@"Édition"];
    [edit addItem:[self menuItem:@"Annuler"
                          action:@selector(menuUndo:)
                             key:@"z"]];
    NSMenuItem* redo = [self menuItem:@"Rétablir"
                               action:@selector(menuRedo:)
                                  key:@"z"];
    redo.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [edit addItem:redo];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItem:[self shortcutMenuItem:@"Copier les clips"
                                  action:@selector(menuCopyTimelineClips:)
                                 command:@"edit.copy"]];
    [edit addItem:[self shortcutMenuItem:@"Coller les clips"
                                  action:@selector(menuPasteTimelineClips:)
                                 command:@"edit.paste"]];
    [edit addItem:NSMenuItem.separatorItem];
    [edit addItem:[self shortcutMenuItem:@"Supprimer la sélection"
                                  action:@selector(menuDeleteSelection:)
                                 command:@"edit.delete"]];
    [edit addItem:[self shortcutMenuItem:@"Suppression ripple"
                                  action:@selector(menuRippleDeleteSelection:)
                                 command:@"edit.ripple"]];
    editRoot.submenu = edit;
    [bar addItem:editRoot];

    NSMenuItem* clipRoot = [[NSMenuItem alloc] initWithTitle:@"Clip"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* clip = [[NSMenu alloc] initWithTitle:@"Clip"];
    [clip addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                          action:@selector(openSelectedMediaInSourceMonitor:)
                             key:@""]];
    [clip addItem:[self menuItem:@"Couper au playhead"
                          action:@selector(menuCutSelectedAtPlayhead:)
                             key:@""]];
    // Out of lot -- the author, 29 October 2026: Resolve's Clip > Enable Clip (D).
    [clip addItem:[self shortcutMenuItem:@"Activer / désactiver le plan"
                                  action:@selector(menuToggleClipsEnabled:)
                                 command:@"clip.enable"]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self shortcutMenuItem:@"Définir le point d’entrée"
                                  action:@selector(menuMarkTimelineIn:)
                                 command:@"mark.in"]];
    [clip addItem:[self shortcutMenuItem:@"Définir le point de sortie"
                                  action:@selector(menuMarkTimelineOut:)
                                 command:@"mark.out"]];
    [clip addItem:[self shortcutMenuItem:@"Effacer les points In/Out"
                                  action:@selector(menuClearTimelineRange:)
                                 command:@"mark.clear"]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self shortcutMenuItem:@"Insérer depuis le moniteur source"
                                  action:@selector(menuInsertSource:)
                                 command:@"source.insert"]];
    [clip addItem:[self shortcutMenuItem:@"Écraser depuis le moniteur source"
                                  action:@selector(menuOverwriteSource:)
                                 command:@"source.overwrite"]];
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self menuItem:@"Activer l’angle multicam du clip"
                          action:@selector(menuActivateMulticamAngle:)
                             key:@""]];
    [clip addItem:[self menuItem:@"Afficher tous les angles multicam"
                          action:@selector(menuClearMulticamAngle:)
                             key:@""]];
    // SY2 -- docs/roadmap/SYNCHRO-2026-10.md. Resolve's Auto Align Clips.
    [clip addItem:NSMenuItem.separatorItem];
    [clip addItem:[self autoAlignMenuItem]];
    clipRoot.submenu = clip;
    [bar addItem:clipRoot];

    NSMenuItem* colorRoot = [[NSMenuItem alloc] initWithTitle:@"Couleur"
                                                       action:nil
                                                keyEquivalent:@""];
    NSMenu* color = [[NSMenu alloc] initWithTitle:@"Couleur"];
    [color addItem:[self menuItem:@"Preset Sony S-Log3 → Rec.2020 HLG"
                           action:@selector(applySonyColorPreset:)
                              key:@""]];
    [color addItem:[self menuItem:@"Configurer la gestion colorimétrique…"
                           action:@selector(configureColorManagement:)
                              key:@""]];
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self menuItem:@"Désactiver la gestion colorimétrique"
                           action:@selector(disableColorManagement:)
                              key:@""]];
    // G6 -- docs/roadmap/GRADE-2026-09.md. Record without its grade, and a
    // reference beside it: what the monitor shows, never the document.
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self shortcutMenuItem:@"Ignorer l’étalonnage"
                                   action:@selector(toggleGradeBypass:)
                                  command:@"color.bypass"]];
    [color addItem:[self shortcutMenuItem:@"Prendre l’image de référence"
                                   action:@selector(grabReferenceStill:)
                                  command:@"color.reference.grab"]];
    [color addItem:[self shortcutMenuItem:@"Comparer à l’image de référence"
                                   action:@selector(toggleReferenceSplit:)
                                  command:@"color.reference.split"]];
    [color addItem:[self menuItem:@"Oublier l’image de référence"
                           action:@selector(forgetReferenceStill:)
                              key:@""]];
    // GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. A grade copied
    // from one shot and pasted on the selection (ClipGradeCopy.h), as on
    // Resolve's Color page; paste_clip_grade builds the same step.
    [color addItem:NSMenuItem.separatorItem];
    [color addItem:[self menuItem:@"Copier l’étalonnage du plan"
                           action:@selector(copyClipGrade:)
                              key:@""]];
    [color addItem:[self menuItem:@"Coller l’étalonnage sur la sélection"
                           action:@selector(pasteClipGrade:)
                              key:@""]];
    colorRoot.submenu = color;
    [bar addItem:colorRoot];

    NSMenuItem* timelineRoot = [[NSMenuItem alloc] initWithTitle:@"Timeline"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* timeline = [[NSMenu alloc] initWithTitle:@"Timeline"];
    NSMenuItem* newTimeline = [self menuItem:@"Nouvelle timeline…"
                                      action:@selector(createTimelinePressed:)
                                         key:@"n"];
    newTimeline.keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [timeline addItem:newTimeline];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Outil Sélection"
                                      action:@selector(menuSelectTool:)
                                     command:@"tool.select"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Main"
                                      action:@selector(menuHandTool:)
                                     command:@"tool.hand"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Zoom"
                                      action:@selector(menuZoomTool:)
                                     command:@"tool.zoom"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Lame"
                                      action:@selector(menuCutTool:)
                                     command:@"tool.cut"]];
    [timeline addItem:[self shortcutMenuItem:@"Outil Trim"
                                      action:@selector(menuTrimTool:)
                                     command:@"tool.trim"]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Magnétisme"
                                      action:@selector(menuToggleSnapping:)
                                     command:@"timeline.snapping"]];
    [timeline
        addItem:[self shortcutMenuItem:@"Sélection liée"
                                action:@selector(menuToggleLinkedSelection:)
                               command:@"timeline.linked"]];
    [timeline addItem:[self shortcutMenuItem:@"Tout sélectionner après la "
                                             @"tête de lecture"
                                      action:@selector(menuSelectClipsAfter:)
                                     command:@"select.after"]];
    [timeline addItem:[self shortcutMenuItem:@"Ajouter un fondu enchaîné"
                                      action:@selector(menuAddCrossDissolve:)
                                     command:@"transition.add"]];
    [timeline addItem:[self menuItem:@"Supprimer le fondu au raccord"
                              action:@selector(menuRemoveCrossDissolve:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Utiliser les proxies"
                              action:@selector(menuToggleProxies:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Générer automatiquement les proxies"
                              action:@selector(menuToggleAutomaticProxies:)
                                 key:@""]];
    [timeline addItem:[self shortcutMenuItem:@"Cadrer toute la timeline"
                                      action:@selector(menuFitTimeline:)
                                     command:@"timeline.fit"]];
    [timeline addItem:[self menuItem:@"Réglages de séquence…"
                              action:@selector(configureSequence:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Transcrire la timeline avec Whisper…"
                              action:@selector(transcribeTimeline:)
                                 key:@""]];
    // D2 -- docs/roadmap/DIALOGUE-2026-10.md.
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self shortcutMenuItem:@"Poser un marqueur"
                                      action:@selector(menuAddMarker:)
                                     command:@"marker.add"]];
    [timeline addItem:[self shortcutMenuItem:@"Annoter le marqueur…"
                                      action:@selector(menuAnnotateMarker:)
                                     command:@"marker.note"]];
    [timeline addItem:[self menuItem:@"Supprimer le marqueur"
                              action:@selector(menuRemoveMarker:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Marqueur précédent (⇧↑)"
                              action:@selector(menuPreviousMarker:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Marqueur suivant (⇧↓)"
                              action:@selector(menuNextMarker:)
                                 key:@""]];
    // D7 -- docs/roadmap/DIALOGUE-2026-10.md.
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Modifier le texte du carton (↩)"
                              action:@selector(menuEditCartonText:)
                                 key:@""]];
    [timeline addItem:NSMenuItem.separatorItem];
    [timeline addItem:[self menuItem:@"Ajouter une piste vidéo"
                              action:@selector(menuAddVideoTrack:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Ajouter une piste audio"
                              action:@selector(menuAddAudioTrack:)
                                 key:@""]];
    [timeline addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
    timelineRoot.submenu = timeline;
    [bar addItem:timelineRoot];

    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. Resolve's Trim menu, for the
    // commands of the trim mode (Resolve 21.1 manual, p. 4340).
    NSMenuItem* trimRoot = [[NSMenuItem alloc] initWithTitle:@"Trim"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* trimMenu = [[NSMenu alloc] initWithTitle:@"Trim"];
    [trimMenu addItem:[self shortcutMenuItem:@"Point de montage le plus proche"
                                      action:@selector(menuTrimSelectNearest:)
                                     command:@"trim.select"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Côté du point (sortie, roll, "
                                             @"entrée)"
                                      action:@selector(menuTrimCycleSide:)
                                     command:@"trim.side"]];
    [trimMenu addItem:NSMenuItem.separatorItem];
    [trimMenu addItem:[self shortcutMenuItem:@"Une image en arrière"
                                      action:@selector(menuTrimNudgeBack:)
                                     command:@"trim.nudge.back"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Une image en avant"
                                      action:@selector(menuTrimNudgeForward:)
                                     command:@"trim.nudge.forward"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Cinq images en arrière"
                                      action:@selector(menuTrimNudgeBackMany:)
                                     command:@"trim.nudge.back.many"]];
    [trimMenu
        addItem:[self shortcutMenuItem:@"Cinq images en avant"
                                action:@selector(menuTrimNudgeForwardMany:)
                               command:@"trim.nudge.forward.many"]];
    [trimMenu addItem:[self shortcutMenuItem:@"Étendre à la tête de lecture"
                                      action:@selector(menuTrimExtend:)
                                     command:@"trim.extend"]];
    [trimMenu addItem:NSMenuItem.separatorItem];
    [trimMenu addItem:[self shortcutMenuItem:@"Trim dynamique (J K L)"
                                      action:@selector(menuTrimDynamic:)
                                     command:@"trim.dynamic"]];
    [trimMenu addItem:[self menuItem:@"Quitter le mode trim (Échap)"
                              action:@selector(menuLeaveTrimMode:)
                                 key:@""]];
    [trimMenu addItem:NSMenuItem.separatorItem];
    // FC3 -- Resolve's Clip > Retime Controls (manual, p. 1218).
    [trimMenu addItem:[self shortcutMenuItem:@"Contrôles de vitesse"
                                      action:@selector(menuRetimeControls:)
                                     command:@"clip.retime"]];
    trimRoot.submenu = trimMenu;
    [bar addItem:trimRoot];

    NSMenuItem* playbackRoot = [[NSMenuItem alloc] initWithTitle:@"Lecture"
                                                          action:nil
                                                   keyEquivalent:@""];
    NSMenu* playback = [[NSMenu alloc] initWithTitle:@"Lecture"];
    [playback addItem:[self shortcutMenuItem:@"Lecture / Pause"
                                      action:@selector(menuPlayPause:)
                                     command:@"play.toggle"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture arrière"
                                      action:@selector(menuPlayReverse:)
                                     command:@"play.reverse"]];
    [playback addItem:[self shortcutMenuItem:@"Arrêt"
                                      action:@selector(menuStop:)
                                     command:@"play.stop"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture avant"
                                      action:@selector(menuPlayForward:)
                                     command:@"play.forward"]];
    [playback addItem:NSMenuItem.separatorItem];
    // ON9 -- Resolve's Play Around Current Selection and its loop.
    [playback addItem:[self shortcutMenuItem:@"Lire autour de la sélection"
                                      action:@selector(menuPlayAround:)
                                     command:@"play.around"]];
    [playback addItem:[self shortcutMenuItem:@"Lecture en boucle"
                                      action:@selector(menuToggleLoop:)
                                     command:@"play.loop"]];
    playbackRoot.submenu = playback;
    [bar addItem:playbackRoot];

    NSMenuItem* viewRoot = [[NSMenuItem alloc] initWithTitle:@"Affichage"
                                                      action:nil
                                               keyEquivalent:@""];
    NSMenu* view = [[NSMenu alloc] initWithTitle:@"Affichage"];
    // ON1, ON2 -- the pages, and the panels a page can call.
    [view addItem:[self shortcutMenuItem:@"Onglet Médias"
                                  action:@selector(showMediaPage:)
                                 command:@"page.media"]];
    [view addItem:[self shortcutMenuItem:@"Onglet Montage"
                                  action:@selector(showEditPage:)
                                 command:@"page.edit"]];
    [view addItem:[self shortcutMenuItem:@"Onglet Couleur"
                                  action:@selector(showColorPage:)
                                 command:@"page.color"]];
    [view addItem:[NSMenuItem separatorItem]];
    [view addItem:[self shortcutMenuItem:@"Médiathèque"
                                  action:@selector(toggleMediaPanel:)
                                 command:@"panel.media"]];
    [view addItem:[self shortcutMenuItem:@"Inspecteur"
                                  action:@selector(toggleInspectorPanel:)
                                 command:@"panel.inspector"]];
    // ON3 -- the Agent's own window, from any page.
    [view addItem:[self shortcutMenuItem:@"Agent"
                                  action:@selector(toggleAgentWindow:)
                                 command:@"panel.agent"]];
    [view addItem:[NSMenuItem separatorItem]];
    [view addItem:[self shortcutMenuItem:@"Vidéo plein écran"
                                  action:@selector(toggleFullscreen:)
                                 command:@"view.fullscreen"]];
    NSMenuItem* output =
        [[NSMenuItem alloc] initWithTitle:@"Sortie vidéo (clean feed)"
                                   action:nil
                            keyEquivalent:@""];
    self.videoOutputMenu = [[NSMenu alloc] initWithTitle:output.title];
    self.videoOutputMenu.autoenablesItems = NO;
    output.submenu = self.videoOutputMenu;
    [view addItem:output];
    [self refreshVideoOutputMenu];
    viewRoot.submenu = view;
    [bar addItem:viewRoot];
    NSApp.mainMenu = bar;
    [self applyShortcutBindingsToMenus];
}

- (void)configureAiEngine:(id)sender {
    (void)sender;
    [self.chatPanelView showConfigurationWindow];
}

// DEC23 -- docs/roadmap/LIVRAISON-2026-09-18.md. The model chosen here is
// the engine's setting, written in the settings file: the agents and the
// command line use it too. The status line says so, or why it is used for
// this transcription only (WhisperModelSetting.h).
- (NSString*)requestWhisperModelPath {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Choisir un modèle Whisper local";
    panel.message =
        @"Sélectionnez un modèle whisper.cpp au format GGML (.bin). "
         "Ce choix est écrit dans ~/.config/cutmachine/.env : les agents et "
         "la ligne de commande s'en servent aussi.";
    panel.prompt = @"Utiliser ce modèle";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.resolvesAliases = YES;
    panel.allowedContentTypes = @[ UTTypeData ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return nil;
    NSString* modelPath = panel.URL.path;
    if (modelPath.length == 0) return nil;
    const whisper_model_setting::Choice choice =
        whisper_model_setting::SaveChosenModel(modelPath.UTF8String ?: "");
    self.binSummaryLabel.stringValue =
        CMStringFromUtf8(whisper_model_setting::ChoiceStatus(choice));
    return modelPath;
}

- (void)chooseWhisperModel:(id)sender {
    (void)sender;
    [self requestWhisperModelPath];
}

// DEC23 -- the model the transcription menus use: the engine's, resolved as
// every tool resolves it; the panel when there is none, or none there.
- (NSString*)transcriptionModelPath {
    std::string path;
    std::string reason;
    if (ResolveConfiguredWhisperModel(path, reason))
        return CMStringFromUtf8(path);
    return [self requestWhisperModelPath];
}

// DEC23 -- the window's old preference, migrated once into the settings
// file, then removed; kept when the file cannot be written, for the next
// launch to try again.
- (void)migrateWhisperModelPreference {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults objectForKey:kWhisperModelPathDefaultsKey] == nil) return;
    NSString* preference = [defaults stringForKey:kWhisperModelPathDefaultsKey];
    const whisper_model_setting::Migration migration =
        whisper_model_setting::MigrateWindowPreference(preference.UTF8String
                                                           ?: "");
    if (migration.forget_preference)
        [defaults removeObjectForKey:kWhisperModelPathDefaultsKey];
}

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. The report read again
// (the window keeps none), off the main thread. `completion` runs on the
// main run loop, scheduled there rather than on the main queue, in the modes
// of the editor and of a modal session: it must land while the welcome
// screen's session runs, and while the smoke run holds the main queue (see
// -checkSourceAvailabilityInBackground). Not during event tracking: the
// dialog it may open waits for a menu to close.
- (void)gatherSetupNotice:
    (void (^)(const SetupNoticeResult& result))completion {
    void (^done)(const SetupNoticeResult&) = [completion copy];
    static NSArray* const modes =
        @[ NSDefaultRunLoopMode, NSModalPanelRunLoopMode ];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      auto result = std::make_shared<SetupNoticeResult>(GatherSetupNotice());
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, (__bridge CFTypeRef)modes, ^{
        done(*result);
      });
      CFRunLoopWakeUp(mainLoop);
    });
}

// « CUTMACHINE ▸ Diagnostic de l'installation… », and « Diagnostic… » on the
// welcome screen: the whole report, in French (SetupNotice.h).
- (void)showSetupDiagnostic:(id)sender {
    (void)sender;
    if (self.setupDiagnosticPending) return;
    // PRD26 -- a form already on the window refuses this one, which would
    // first replace what « Copier le diagnostic » copies.
    if ([CMFormPanel formShownInWindow:self.window]) {
        NSBeep();
        return;
    }
    self.setupDiagnosticPending = YES;
    __weak AppDelegate* weakSelf = self;
    [self gatherSetupNotice:^(const SetupNoticeResult& result) {
      AppDelegate* strongSelf = weakSelf;
      if (!strongSelf) return;
      strongSelf.setupDiagnosticPending = NO;
      // PRD26 -- a sheet on the editor window; modal on the welcome screen,
      // which has none. A form opened while the report was gathered keeps
      // the window.
      if ([CMFormPanel formShownInWindow:strongSelf.window]) {
          NSBeep();
          return;
      }
      __weak AppDelegate* weakAgain = strongSelf;
      [[strongSelf setupDiagnosticPanel:result]
          presentInWindow:strongSelf.window
               completion:^(NSModalResponse response) {
                 (void)response;
                 weakAgain.setupDiagnosticClipboard = nil;
               }];
    }];
}

// The dialog, built and not shown: the window test reads it the same way.
// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The window's form panel in
// place of an alert, resizable: the text takes the room. Its colours are the
// theme's now, the panel's appearance dark to match (FormPanel.h).
- (CMFormPanel*)setupDiagnosticPanel:(const SetupNoticeResult&)result {
    using namespace ui::theme;
    const NSRect frame = NSMakeRect(0, 0, 560, 320);
    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:frame];
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    const NSSize content = [NSScrollView
        contentSizeForFrameSize:frame.size
        horizontalScrollerClass:nil
          verticalScrollerClass:NSScroller.class
                     borderType:NSBezelBorder
                    controlSize:NSControlSizeRegular
                  scrollerStyle:NSScroller.preferredScrollerStyle];
    NSTextView* text = [[NSTextView alloc]
        initWithFrame:NSMakeRect(0, 0, content.width, content.height)];
    text.identifier = @"setup.diagnostic.text";
    text.editable = NO;
    text.selectable = YES;
    text.drawsBackground = NO;
    text.textContainerInset = NSMakeSize(kSpaceS, kSpaceS);
    text.verticallyResizable = YES;
    text.horizontallyResizable = NO;
    text.autoresizingMask = NSViewWidthSizable;
    text.textContainer.widthTracksTextView = YES;
    NSDictionary* plain = @{
        NSFontAttributeName : CMFont(kFontSizeBody, NSFontWeightRegular),
        NSForegroundColorAttributeName : CMTextPrimaryColor()
    };
    NSDictionary* heading = @{
        NSFontAttributeName : CMFont(kFontSizeSection, NSFontWeightSemibold),
        NSForegroundColorAttributeName : CMTextPrimaryColor()
    };
    NSMutableAttributedString* body = [NSMutableAttributedString new];
    for (const setup_notice::Run& run :
         setup_notice::DiagnosticRuns(result.report, result.facts))
        [body
            appendAttributedString:[[NSAttributedString alloc]
                                       initWithString:CMStringFromUtf8(run.text)
                                           attributes:run.heading ? heading
                                                                  : plain]];
    [text.textStorage setAttributedString:body];
    scroll.documentView = text;
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Diagnostic de l'installation"
              message:@"Ce dont CUTMACHINE dispose sur cette machine, relu à "
                      @"l'instant. « Copier le diagnostic » met ce texte dans "
                      @"le presse-papiers, suivi du rapport du moteur "
                      @"(cutmachine --check-setup)."
                 form:scroll];
    [panel addButtonWithTitle:@"Fermer" role:CMFormButtonRoleAccept];
    NSButton* copy = [panel addButtonWithTitle:@"Copier le diagnostic"
                                          role:CMFormButtonRoleAction];
    copy.identifier = @"setup.diagnostic.copy";
    // Its own action, not the panel's: the dialog stays open once copied.
    copy.target = self;
    copy.action = @selector(copySetupDiagnostic:);
    panel.formResizable = YES;
    self.setupDiagnosticClipboard = CMStringFromUtf8(
        setup_notice::ClipboardText(result.report, result.facts));
    return panel;
}

- (void)copySetupDiagnostic:(id)sender {
    if (![self writeSetupDiagnosticToPasteboard:NSPasteboard.generalPasteboard])
        return;
    if ([sender isKindOfClass:NSButton.class])
        ((NSButton*)sender).title = @"Diagnostic copié";
}

- (BOOL)writeSetupDiagnosticToPasteboard:(NSPasteboard*)pasteboard {
    NSString* text = self.setupDiagnosticClipboard;
    if (text.length == 0) return NO;
    [pasteboard clearContents];
    return [pasteboard setString:text forType:NSPasteboardTypeString];
}

- (void)toggleFullscreen:(id)sender {
    (void)sender;
    if (self.videoFullscreenActive) {
        self.videoFullscreenActive = NO;
        [self.programMonitorView removeFromSuperview];
        [self.videoFullscreenWindow orderOut:nil];
        self.programMonitorView.frame = self.programMonitorPanel.bounds;
        [self.programMonitorPanel addSubview:self.programMonitorView];
        self.programMonitorTitleLabel.hidden = NO;
        self.programMonitorZoomPopup.hidden = NO;
        self.sourceMonitorToggleButton.hidden = NO;
        [self.window makeKeyAndOrderFront:nil];
        [self.window makeFirstResponder:self.programMonitorView];
    } else {
        NSScreen* screen = self.window.screen ?: NSScreen.mainScreen;
        if (!screen) return;
        [self.programMonitorView removeFromSuperview];
        if (!self.videoFullscreenWindow) {
            self.videoFullscreenWindow = [[VideoFullscreenWindow alloc]
                initWithContentRect:screen.frame
                          styleMask:NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO
                             screen:screen];
            self.videoFullscreenWindow.backgroundColor = NSColor.blackColor;
            self.videoFullscreenWindow.opaque = YES;
            self.videoFullscreenWindow.level = NSMainMenuWindowLevel + 1;
            self.videoFullscreenWindow.animationBehavior =
                NSWindowAnimationBehaviorNone;
            self.videoFullscreenWindow.releasedWhenClosed = NO;
            self.videoFullscreenWindow.collectionBehavior =
                NSWindowCollectionBehaviorFullScreenAuxiliary |
                NSWindowCollectionBehaviorStationary;
        } else {
            [self.videoFullscreenWindow setFrame:screen.frame display:NO];
        }
        self.videoFullscreenActive = YES;
        self.programMonitorView.frame =
            self.videoFullscreenWindow.contentView.bounds;
        [self.videoFullscreenWindow.contentView
            addSubview:self.programMonitorView];
        self.programMonitorTitleLabel.hidden = YES;
        self.programMonitorZoomPopup.hidden = YES;
        self.sourceMonitorToggleButton.hidden = YES;
        self.state->sourceMonitorActive = false;
        [self.videoFullscreenWindow makeKeyAndOrderFront:nil];
        [self.videoFullscreenWindow makeFirstResponder:self.programMonitorView];
    }
    self.state->overlayDirty = true;
    // The motion overlay places its outlines for the size Record had
    // (motionCanvasPrepare): entering, it is cleared, and leaving, the
    // selection is placed again for the panel. The node under the mouse is
    // forgotten either way -- the mouse is not where it was.
    self.state->motionCanvas.hovered.clear();
    [self motionCanvasRefresh];
}

- (void)saveProjectPressed:(id)sender {
    (void)sender;
    std::string message;
    if ([self persistEdits:message]) {
        self.infoLabel.stringValue = @"Projet enregistré";
    } else {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Échec de l’enregistrement : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
    }
}

- (BOOL)commitColorSettings:(const ColorManagementSettings&)settings {
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            SetColorManagementOperation { settings }
        }
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Unable to apply color settings (%s): %s\n",
                     EditErrorName(error), message.c_str());
        NSBeep();
        return NO;
    }
    self.state->overlayDirty = true;
    return YES;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel.
// The operation is applied before the panel closes: refused, the panel
// stays open with the engine's reason, where a second alert -- titled
// « Export impossible » -- used to say it once the form was gone.
- (void)configureSequence:(id)sender {
    (void)sender;
    if (!self.state) return;
    const DocumentSequence current = self.state->document.sequence;
    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 108)];

    NSTextField* nameLabel = [NSTextField labelWithString:@"Nom :"];
    nameLabel.frame = NSMakeRect(0, 79, 120, 22);
    nameLabel.alignment = NSTextAlignmentRight;
    [accessory addSubview:nameLabel];
    NSTextField* name =
        [[NSTextField alloc] initWithFrame:NSMakeRect(132, 76, 316, 24)];
    name.identifier = @"sequence.name";
    name.stringValue = [NSString stringWithUTF8String:current.name.c_str()];
    [accessory addSubview:name];

    auto addPopup = [&](NSString* labelText, NSArray<NSString*>* choices,
                        CGFloat y) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3, 120, 22);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* menu =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(132, y, 316, 26)
                                       pullsDown:NO];
        [menu addItemsWithTitles:choices];
        [accessory addSubview:menu];
        return menu;
    };
    NSString* currentFormat = [NSString
        stringWithFormat:@"Actuel — %d × %d", current.width, current.height];
    NSPopUpButton* format =
        addPopup(@"Dimensions :",
                 @[
                     currentFormat, @"HD — 1920 × 1080", @"UHD — 3840 × 2160",
                     @"Vertical HD — 1080 × 1920", @"Carré — 1080 × 1080"
                 ],
                 44);
    NSString* currentRate = [NSString stringWithFormat:@"Actuelle — %d/%d i/s",
                                                       current.frame_rate.num,
                                                       current.frame_rate.den];
    NSPopUpButton* cadence =
        addPopup(@"Cadence :",
                 @[
                     currentRate, @"23,976 i/s", @"24 i/s", @"25 i/s",
                     @"29,97 i/s", @"50 i/s", @"59,94 i/s"
                 ],
                 12);
    format.identifier = @"sequence.format";
    cadence.identifier = @"sequence.rate";
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Réglages de séquence"
              message:@"Le format de séquence pilote le moniteur et les "
                      @"presets d’export."
                 form:accessory];
    [panel addButtonWithTitle:@"Appliquer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = name;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      UpdateSequenceOperation updated;
      updated.sequence_id = current.id;
      updated.name = name.stringValue.UTF8String ?: "Sequence 1";
      updated.width = current.width;
      updated.height = current.height;
      updated.frame_rate = current.frame_rate;
      switch (format.indexOfSelectedItem) {
          case 1:
              updated.width = 1920;
              updated.height = 1080;
              break;
          case 2:
              updated.width = 3840;
              updated.height = 2160;
              break;
          case 3:
              updated.width = 1080;
              updated.height = 1920;
              break;
          case 4:
              updated.width = 1080;
              updated.height = 1080;
              break;
          default:
              break;
      }
      switch (cadence.indexOfSelectedItem) {
          case 1:
              updated.frame_rate = {24000, 1001};
              break;
          case 2:
              updated.frame_rate = {24, 1};
              break;
          case 3:
              updated.frame_rate = {25, 1};
              break;
          case 4:
              updated.frame_rate = {30000, 1001};
              break;
          case 5:
              updated.frame_rate = {50, 1};
              break;
          case 6:
              updated.frame_rate = {60000, 1001};
              break;
          default:
              break;
      }
      EditError editError = EditError::None;
      std::string message;
      if (![self applyAndPersistTimelineOperation:Operation{std::move(updated)}
                                            error:editError
                                          message:message])
          return message.empty() ? @"Réglages refusés par le moteur."
                                 : CMStringFromUtf8(message);
      return nil;
    };
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self refreshTimelineAfterEditFromPosition:self.state->
                                                             requestedPosition];
                  [self rebuildMediaList];
                  self.state->overlayDirty = true;
                }];
}

- (void)showExportError:(const std::string&)message {
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"Export impossible";
    alert.informativeText =
        [NSString stringWithUTF8String:message.c_str()] ?: @"Erreur inconnue";
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

// What a save panel shown as a sheet answered. In the window test, the
// destination the test set and nothing without one: AppKit's panel runs out
// of process, a test can only end its sheet, and the panel then answers the
// last folder it was used in -- on 8 October 2026, a bench project's
// package, which a test export wrote into.
static NSURL* CMSaveSheetAnswer(NSSavePanel* save, NSModalResponse response) {
    if (response != NSModalResponseOK) return nil;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) {
        if (gUiSmokeSaveAnswer.empty()) return nil;
        NSURL* answer = [NSURL fileURLWithPath:@(gUiSmokeSaveAnswer.c_str())];
        gUiSmokeSaveAnswer.clear();
        return answer;
    }
#endif
    return save.URL;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The settings on the window's
// form panel, the destination in the save panel, the render under a
// progress panel: three sheets on the window, one after the other. The
// window stays blocked while the export runs, as under the alert it
// replaces; the menus stay live, as they were (ExportPlan is a copy: an
// edit made meanwhile does not reach the file being written).
- (void)exportFinalVideo:(id)sender {
    (void)sender;
    if (!self.state || self.state->exportRunning) return;

    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 172)];
    auto popup = [&](NSString* labelText, NSArray<NSString*>* choices,
                     CGFloat y) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3, 132, 22);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* menu =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(144, y, 364, 26)
                                       pullsDown:NO];
        [menu addItemsWithTitles:choices];
        [accessory addSubview:menu];
        return menu;
    };

    NSMutableArray<NSString*>* presetNames = [NSMutableArray array];
    for (const ExportPresetDescriptor& preset : Exporter::Presets())
        [presetNames addObject:[NSString stringWithUTF8String:preset.name]];
    NSPopUpButton* preset = popup(@"Preset :", presetNames, 140);
    // PRD16 -- ROADMAP.md. The dialog opens on the engine's default for this
    // timeline, the file --export and export_timeline write without a codec.
    [preset
        selectItemAtIndex:(NSInteger)Exporter::PresetIndex(
                              Exporter::DefaultPreset(self.state->document))];
    // VQA2 -- the SDR tag rides with the container: Rec.709-A exists only
    // in a .mov (Export.h), so the menu offers no pair the engine refuses.
    NSPopUpButton* container =
        popup(@"Conteneur :",
              @[
                  @"MP4 · Rec.709", @"QuickTime MOV · Rec.709",
                  @"QuickTime MOV · Rec.709-A (gamma 2,4, comme le moniteur)"
              ],
              108);
    NSPopUpButton* resolution =
        popup(@"Résolution :",
              @[
                  @"Selon le preset", @"Format de la séquence", @"1920 × 1080",
                  @"3840 × 2160"
              ],
              76);
    NSPopUpButton* cadence =
        popup(@"Cadence :",
              @[
                  @"Selon le preset", @"Cadence de la séquence", @"23,976 i/s",
                  @"24 i/s", @"25 i/s", @"29,97 i/s", @"50 i/s", @"59,94 i/s"
              ],
              44);
    NSPopUpButton* encoder =
        popup(@"Encodage :",
              @[
                  @"Selon le preset", @"VideoToolbox — rapide",
                  @"Logiciel — qualité constante"
              ],
              12);
    preset.identifier = @"export.preset";
    container.identifier = @"export.container";
    resolution.identifier = @"export.resolution";
    cadence.identifier = @"export.rate";
    encoder.identifier = @"export.encoder";
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Exporter la vidéo finale"
              message:@"Choisissez un preset. Les autres menus servent "
                      @"uniquement d’override ponctuel."
                 form:accessory];
    [panel addButtonWithTitle:@"Choisir la destination…"
                         role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = preset;
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self exportWithPreset:preset
                               container:container
                              resolution:resolution
                                 cadence:cadence
                                 encoder:encoder];
                }];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The export's progress, a panel
// with its « Annuler »: the bar is its form. Its title says the codec the
// preset gives (PRD16). Built apart, so the window test reads it.
- (CMFormPanel*)exportProgressPanel:(BOOL)h264 {
    NSProgressIndicator* indicator =
        [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 420, 20)];
    indicator.identifier = @"export.progress";
    indicator.indeterminate = NO;
    indicator.minValue = 0.0;
    indicator.maxValue = 100.0;
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:h264 ? @"Export H.264 en cours" : @"Export HEVC en cours"
              message:@"Préparation du rendu…"
                 form:indicator];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    return panel;
}

// The collection's, which cannot be cancelled: the alert's own « OK » let
// the person put it away and go on editing -- the collection copies the
// project as saved when it started -- and « Masquer » keeps that, named for
// what it does.
- (CMFormPanel*)collectProgressPanel {
    NSProgressIndicator* indicator =
        [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(0, 0, 420, 18)];
    indicator.identifier = @"collect.progress";
    indicator.indeterminate = YES;
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Collecte du projet en cours"
              message:@"Copie et vérification des médias originaux…"
                 form:indicator];
    [panel addButtonWithTitle:@"Masquer" role:CMFormButtonRoleCancel];
    return panel;
}

// What the export form chose, read once it has closed: the destination,
// then the render.
- (void)exportWithPreset:(NSPopUpButton*)preset
               container:(NSPopUpButton*)container
              resolution:(NSPopUpButton*)resolution
                 cadence:(NSPopUpButton*)cadence
                 encoder:(NSPopUpButton*)encoder {
    if (!self.state || self.state->exportRunning) return;
    const int32_t sequenceWidth = self.state->document.sequence.width;
    const int32_t sequenceHeight = self.state->document.sequence.height;
    const MediaRate sequenceRate = self.state->document.sequence.frame_rate;

    const auto& presets = Exporter::Presets();
    const size_t presetIndex = std::min<size_t>(
        static_cast<size_t>(preset.indexOfSelectedItem), presets.size() - 1);
    ExportSettings selectedSettings =
        Exporter::SettingsForPreset(presets[presetIndex].id, "", sequenceWidth,
                                    sequenceHeight, sequenceRate);
    switch (resolution.indexOfSelectedItem) {
        case 1:
            selectedSettings.width = sequenceWidth + sequenceWidth % 2;
            selectedSettings.height = sequenceHeight + sequenceHeight % 2;
            break;
        case 2:
            selectedSettings.width = 1920;
            selectedSettings.height = 1080;
            break;
        case 3:
            selectedSettings.width = 3840;
            selectedSettings.height = 2160;
            break;
        default:
            break;
    }
    switch (cadence.indexOfSelectedItem) {
        case 1:
            selectedSettings.frame_rate = sequenceRate;
            break;
        case 2:
            selectedSettings.frame_rate = {24000, 1001};
            break;
        case 3:
            selectedSettings.frame_rate = {24, 1};
            break;
        case 4:
            selectedSettings.frame_rate = {25, 1};
            break;
        case 5:
            selectedSettings.frame_rate = {30000, 1001};
            break;
        case 6:
            selectedSettings.frame_rate = {50, 1};
            break;
        case 7:
            selectedSettings.frame_rate = {60000, 1001};
            break;
        default:
            break;
    }
    // PRD16 -- the override changes how the preset's codec is encoded, never
    // the codec: with the H.264 preset it used to force an HEVC encoder.
    const bool h264 = Exporter::IsH264(selectedSettings.encoder);
    if (encoder.indexOfSelectedItem == 1)
        selectedSettings.encoder = h264 ? ExportEncoder::H264VideoToolbox
                                        : ExportEncoder::HevcVideoToolbox;
    else if (encoder.indexOfSelectedItem == 2)
        selectedSettings.encoder =
            h264 ? ExportEncoder::H264Software : ExportEncoder::HevcSoftware;

    const BOOL mov = container.indexOfSelectedItem >= 1;
    if (container.indexOfSelectedItem == 2)
        selectedSettings.sdr_tag = ExportSdrTag::Rec709A;
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Destination de l’export";
    save.nameFieldStringValue = mov ? @"export.mov" : @"export.mp4";
    save.allowedContentTypes =
        @[ [UTType typeWithFilenameExtension:(mov ? @"mov" : @"mp4")] ];
    [save
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse response) {
                 NSURL* chosen = CMSaveSheetAnswer(save, response);
                 if (!chosen) return;

                 ExportSettings settings = selectedSettings;
                 settings.output_path = chosen.fileSystemRepresentation ?: "";
                 settings.overwrite =
                     true;  // NSSavePanel already confirmed this.

                 ExportPlan plan;
                 std::string error;
                 if (!Exporter::BuildPlan(self.state->document,
                                          self.documentPath.UTF8String ?: "",
                                          settings, plan, error)) {
                     [self showExportError:error];
                     return;
                 }

                 void (^startExport)(void) = ^{
                   // PRD26 -- a progress panel with its « Annuler », in
                   // place of an alert.
                   CMFormPanel* progressPanel = [self
                       exportProgressPanel:Exporter::IsH264(settings.encoder)];
                   NSProgressIndicator* indicator =
                       (NSProgressIndicator*)progressPanel.form;
                   self.state->exportCancel.store(false);
                   self.state->exportRunning = true;
                   [progressPanel
                       presentInWindow:self.window
                            completion:^(NSModalResponse) {
                              if (self.state && self.state->exportRunning)
                                  self.state->exportCancel.store(true);
                            }];

                   dispatch_async(
                       dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                       ^{
                         std::string renderError;
                         const bool ok = Exporter::Run(
                             plan,
                             [&](const ExportProgress& progress) {
                                 dispatch_async(dispatch_get_main_queue(), ^{
                                   indicator.doubleValue =
                                       progress.fraction * 100.0;
                                   [progressPanel
                                       setMessage:
                                           [NSString
                                               stringWithFormat:
                                                   @"%lld / %lld images — "
                                                   @"%.2fx",
                                                   static_cast<long long>(
                                                       progress
                                                           .rendered_frames),
                                                   static_cast<long long>(
                                                       progress.total_frames),
                                                   progress.speed]];
                                 });
                             },
                             &self.state->exportCancel, renderError);
                         dispatch_async(dispatch_get_main_queue(), ^{
                           self.state->exportRunning = false;
                           [progressPanel endWithResponse:NSModalResponseStop];
                           if (!ok) {
                               if (renderError != "export cancelled")
                                   [self showExportError:renderError];
                               return;
                           }
                           NSAlert* complete = [NSAlert new];
                           complete.messageText = @"Export terminé";
                           complete.informativeText = chosen.path;
                           [complete beginSheetModalForWindow:self.window
                                            completionHandler:nil];
                         });
                       });
                 };
                 // Decided on 8 October 2026 (ROADMAP.md): a caption
                 // family the caption renderer lacks is warned about,
                 // not refused (FontCatalog.h).
                 if (plan.caption_font_substitutions.empty()) {
                     startExport();
                     return;
                 }
                 const CaptionFontNotice notice =
                     CaptionFontWarning(plan.caption_font_substitutions);
                 NSAlert* warning = [NSAlert new];
                 warning.alertStyle = NSAlertStyleWarning;
                 warning.messageText =
                     [NSString stringWithUTF8String:notice.title.c_str()];
                 warning.informativeText =
                     [NSString stringWithUTF8String:notice.text.c_str()];
                 [warning addButtonWithTitle:@"Exporter quand même"];
                 // Escape is given by NSAlert to a button titled with the
                 // "Cancel" of the language macOS lends the app; said here,
                 // it does not depend on it (QA pass of 8 October 2026).
                 [warning addButtonWithTitle:@"Annuler"].keyEquivalent =
                     @"\033";
                 [warning beginSheetModalForWindow:self.window
                                 completionHandler:^(NSModalResponse choice) {
                                   if (choice == NSAlertFirstButtonReturn)
                                       startExport();
                                 }];
               }];
}

- (void)applySonyColorPreset:(id)sender {
    (void)sender;
    ColorManagementSettings settings;
    settings.enabled = true;
    settings.input_gamut = "sony_sgamut3_cine";
    settings.input_transfer = "sony_slog3";
    settings.input_ycbcr_matrix = "bt709";
    settings.input_range = "full";
    settings.working_gamut = "acescct";
    settings.output_gamut = "rec2020";
    settings.output_transfer = "hlg";
    [self commitColorSettings:settings];
}

- (void)disableColorManagement:(id)sender {
    (void)sender;
    ColorManagementSettings settings = self.state->document.color_management;
    settings.enabled = false;
    [self commitColorSettings:settings];
}

- (void)configureColorManagement:(id)sender {
    (void)sender;
    const ColorManagementSettings& current =
        self.state->document.color_management;

    NSView* accessory =
        [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 500, 264)];
    NSButton* enabled = [NSButton checkboxWithTitle:@"Activer pour ce projet"
                                             target:nil
                                             action:nil];
    enabled.identifier = @"color.enabled";
    enabled.frame = NSMakeRect(0, 236, 500, 24);
    enabled.state =
        current.enabled ? NSControlStateValueOn : NSControlStateValueOff;
    [accessory addSubview:enabled];

    CGFloat y = 202.0;
    auto addPopup = [&](NSString* labelText,
                        std::initializer_list<std::pair<NSString*, NSString*>>
                            choices,
                        const std::string& selected) {
        NSTextField* label = [NSTextField labelWithString:labelText];
        label.frame = NSMakeRect(0, y + 3.0, 175, 20);
        label.alignment = NSTextAlignmentRight;
        [accessory addSubview:label];
        NSPopUpButton* popup =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(188, y, 300, 26)
                                       pullsDown:NO];
        for (const auto& choice : choices) {
            [popup addItemWithTitle:choice.first];
            popup.lastItem.representedObject = choice.second;
            if ([choice.second
                    isEqualToString:[NSString
                                        stringWithUTF8String:selected.c_str()]])
                [popup selectItem:popup.lastItem];
        }
        [accessory addSubview:popup];
        y -= 32.0;
        return popup;
    };
    NSPopUpButton* inputGamut =
        addPopup(@"Gamut d’entrée :",
                 {{@"Sony S-Gamut3.Cine", @"sony_sgamut3_cine"},
                  {@"Sony S-Gamut3", @"sony_sgamut3"},
                  {@"Rec.709", @"rec709"},
                  {@"Rec.2020", @"rec2020"}},
                 current.input_gamut);
    NSPopUpButton* inputTransfer = addPopup(@"Courbe d’entrée :",
                                            {{@"Sony S-Log3", @"sony_slog3"},
                                             {@"Rec.709", @"rec709"},
                                             {@"HLG", @"hlg"},
                                             {@"Linéaire", @"linear"}},
                                            current.input_transfer);
    NSPopUpButton* ycbcr = addPopup(@"Matrice YCbCr :",
                                    {{@"Auto (métadonnées)", @"auto"},
                                     {@"BT.709", @"bt709"},
                                     {@"BT.2020 non constante", @"bt2020_ncl"}},
                                    current.input_ycbcr_matrix);
    NSPopUpButton* inputRange = addPopup(@"Plage du signal :",
                                         {{@"Auto (métadonnées)", @"auto"},
                                          {@"Full / Extended", @"full"},
                                          {@"Legal / Limited", @"limited"}},
                                         current.input_range);
    NSPopUpButton* working = addPopup(@"Espace de travail :",
                                      {{@"ACEScct (AP1)", @"acescct"},
                                       {@"Rec.2020 linéaire", @"rec2020"},
                                       {@"Rec.709 linéaire", @"rec709"}},
                                      current.working_gamut);
    NSPopUpButton* outputGamut =
        addPopup(@"Gamut de sortie :",
                 {{@"Rec.2020", @"rec2020"}, {@"Rec.709", @"rec709"}},
                 current.output_gamut);
    NSPopUpButton* outputTransfer = addPopup(
        @"Courbe de sortie :", {{@"HLG", @"hlg"}, {@"Rec.709", @"rec709"}},
        current.output_transfer);
    inputGamut.identifier = @"color.input_gamut";
    inputTransfer.identifier = @"color.input_transfer";
    ycbcr.identifier = @"color.ycbcr_matrix";
    inputRange.identifier = @"color.input_range";
    working.identifier = @"color.working_gamut";
    outputGamut.identifier = @"color.output_gamut";
    outputTransfer.identifier = @"color.output_transfer";

    const auto chosen = [=]() {
        const auto value = [](NSPopUpButton* popup) {
            NSString* string = popup.selectedItem.representedObject;
            return std::string(string.UTF8String ?: "");
        };
        ColorManagementSettings settings;
        settings.enabled = enabled.state == NSControlStateValueOn;
        settings.input_gamut = value(inputGamut);
        settings.input_transfer = value(inputTransfer);
        settings.input_ycbcr_matrix = value(ycbcr);
        settings.input_range = value(inputRange);
        settings.working_gamut = value(working);
        settings.output_gamut = value(outputGamut);
        settings.output_transfer = value(outputTransfer);
        return settings;
    };
    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel;
    // the HLG rule is checked before it closes, where a second alert,
    // « Configuration incompatible », said it once the form was gone.
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Gestion colorimétrique du projet"
              message:@"Les transformations sont calculées en linéaire dans "
                      @"l’espace de composition AP1, avec ACEScct pour le "
                      @"grading. Pour les rushes Sony habituels, utilisez "
                      @"S-Gamut3.Cine / S-Log3 en plage Full, puis Rec.2020 "
                      @"/ HLG."
                 form:accessory];
    [panel addButtonWithTitle:@"Appliquer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      const ColorManagementSettings settings = chosen();
      if (settings.output_transfer == "hlg" &&
          settings.output_gamut != "rec2020")
          return @"La sortie HLG doit utiliser le gamut Rec.2020.";
      return nil;
    };
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self commitColorSettings:chosen()];
                }];
}

- (NSArray<NSString*>*)recentProjectPaths {
    NSArray* stored = [NSUserDefaults.standardUserDefaults
        arrayForKey:@"RecentProjectPaths.v2"];
    NSMutableArray<NSString*>* available = [NSMutableArray array];
    for (id value in stored) {
        if (![value isKindOfClass:NSString.class]) continue;
        BOOL directory = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:value
                                               isDirectory:&directory] &&
            !directory)
            [available addObject:value];
        if (available.count == 8) break;
    }
    return available;
}

- (void)recordRecentProject:(NSString*)path {
    if (path.length == 0) return;
    NSString* standardized = path.stringByStandardizingPath;
    NSMutableArray<NSString*>* recent =
        [NSMutableArray arrayWithObject:standardized];
    for (NSString* candidate in [self recentProjectPaths]) {
        if (![candidate isEqualToString:standardized])
            [recent addObject:candidate];
        if (recent.count == 8) break;
    }
    [NSUserDefaults.standardUserDefaults setObject:recent
                                            forKey:@"RecentProjectPaths.v2"];
}

- (void)startupNewPressed:(id)sender {
    (void)sender;
    self.startupChoice = 1;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupOpenPressed:(id)sender {
    (void)sender;
    self.startupChoice = 2;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupRecentPressed:(id)sender {
    (void)sender;
    NSString* path = self.recentProjectsPopup.selectedItem.representedObject;
    if (path.length == 0) return;
    self.startupSelectedPath = path;
    self.startupChoice = 3;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupQuitPressed:(id)sender {
    (void)sender;
    self.startupChoice = 0;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)showStartupWindow {
    [self buildStartupWindow];
    [self.startupWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp runModalForWindow:self.startupWindow];
}

// PRD21 (window) -- built apart from its modal session, so the window test
// builds and reads the screen the way it is shown, without a session that
// would hold the run until someone answers it.
- (void)buildStartupWindow {
    const NSRect frame = NSMakeRect(0, 0, 680, 430);
    self.startupWindow =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskTitled
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.startupWindow.title = @"Bienvenue dans CUTMACHINE";
    self.startupWindow.releasedWhenClosed = NO;
    [self.startupWindow center];

    NSView* content = [[NSView alloc] initWithFrame:frame];
    content.wantsLayer = YES;
    content.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfacePanel).CGColor;
    self.startupWindow.contentView = content;

    NSImageView* icon =
        [[NSImageView alloc] initWithFrame:NSMakeRect(54, 278, 92, 92)];
    icon.image = NSApp.applicationIconImage
                     ?: SystemSymbol(@"scissors", @"CUTMACHINE", 64.0);
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [content addSubview:icon];

    NSTextField* title = [NSTextField labelWithString:@"CUTMACHINE"];
    title.frame = NSMakeRect(170, 325, 455, 42);
    title.font = [NSFont systemFontOfSize:30.0 weight:NSFontWeightBold];
    title.textColor = NSColor.labelColor;
    [content addSubview:title];
    NSTextField* subtitle = [NSTextField
        labelWithString:
            @"Commencez un nouveau montage ou reprenez un projet existant."];
    subtitle.frame = NSMakeRect(172, 294, 445, 24);
    subtitle.font = [NSFont systemFontOfSize:14.0];
    subtitle.textColor = NSColor.secondaryLabelColor;
    [content addSubview:subtitle];

    NSButton* create = [NSButton buttonWithTitle:@"Nouveau projet"
                                          target:self
                                          action:@selector(startupNewPressed:)];
    create.frame = NSMakeRect(54, 211, 274, 56);
    create.bezelStyle = NSBezelStyleTexturedRounded;
    create.font = [NSFont systemFontOfSize:15.0 weight:NSFontWeightSemibold];
    create.image = SystemSymbol(@"plus.square", @"Nouveau projet", 20.0);
    create.imagePosition = NSImageLeading;
    create.keyEquivalent = @"\r";
    [content addSubview:create];

    NSButton* open = [NSButton buttonWithTitle:@"Ouvrir un projet…"
                                        target:self
                                        action:@selector(startupOpenPressed:)];
    open.frame = NSMakeRect(352, 211, 274, 56);
    open.bezelStyle = NSBezelStyleTexturedRounded;
    open.font = [NSFont systemFontOfSize:15.0 weight:NSFontWeightSemibold];
    open.image = SystemSymbol(@"folder", @"Ouvrir un projet", 20.0);
    open.imagePosition = NSImageLeading;
    [content addSubview:open];

    NSTextField* recentLabel = [NSTextField labelWithString:@"Projets récents"];
    recentLabel.frame = NSMakeRect(54, 158, 160, 22);
    recentLabel.font = [NSFont systemFontOfSize:12.0
                                         weight:NSFontWeightSemibold];
    recentLabel.textColor = NSColor.secondaryLabelColor;
    [content addSubview:recentLabel];
    self.recentProjectsPopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(54, 116, 460, 34)];
    NSArray<NSString*>* recentPaths = [self recentProjectPaths];
    if (recentPaths.count == 0) {
        [self.recentProjectsPopup addItemWithTitle:@"Aucun projet récent"];
        self.recentProjectsPopup.enabled = NO;
    } else {
        for (NSString* path in recentPaths) {
            NSString* title =
                path.lastPathComponent.stringByDeletingPathExtension;
            if ([title.pathExtension.lowercaseString
                    isEqualToString:@"cutmachine"])
                title = title.stringByDeletingPathExtension;
            [self.recentProjectsPopup addItemWithTitle:title];
            self.recentProjectsPopup.lastItem.representedObject = path;
            self.recentProjectsPopup.lastItem.toolTip = path;
        }
    }
    [content addSubview:self.recentProjectsPopup];
    NSButton* recent =
        [NSButton buttonWithTitle:@"Ouvrir"
                           target:self
                           action:@selector(startupRecentPressed:)];
    recent.frame = NSMakeRect(526, 116, 100, 34);
    recent.enabled = recentPaths.count > 0;
    [content addSubview:recent];

    NSButton* quit = [NSButton buttonWithTitle:@"Quitter"
                                        target:self
                                        action:@selector(startupQuitPressed:)];
    quit.frame = NSMakeRect(526, 38, 100, 30);
    quit.keyEquivalent = @"\e";
    [content addSubview:quit];

    // PRD21 (window) -- at the foot, left of « Quitter », from the screen's
    // left margin.
    [self installStartupSetupNoticeIn:content left:54.0 beside:quit.frame];
}

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. An app opened from the
// Finder gets launchd's PATH, where FFmpeg is not, and nothing said so: the
// welcome screen now says what this machine lacks to edit and export
// (SetupNotice.h), with « Diagnostic… » for the rest. The row stays hidden
// until the report comes back from its thread: neither a slow disk nor an
// FFmpeg that does not answer delays the screen.
- (void)installStartupSetupNoticeIn:(NSView*)content
                               left:(double)left
                             beside:(NSRect)quit {
    using namespace ui::theme;
    const NSRect rowFrame =
        NSMakeRect(left, NSMinY(quit) - kSpaceXs, NSMinX(quit) - kSpaceM - left,
                   NSHeight(quit) + 2.0 * kSpaceXs);
    NSView* row = [[NSView alloc] initWithFrame:rowFrame];
    row.identifier = @"startup.setup_notice";
    row.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    row.hidden = YES;

    constexpr double kIconSize = 16.0;
    NSImageView* icon = [[NSImageView alloc]
        initWithFrame:NSMakeRect(
                          0, std::floor((NSHeight(rowFrame) - kIconSize) / 2.0),
                          kIconSize, kIconSize)];
    icon.identifier = @"startup.setup_notice.icon";
    icon.image = SystemSymbol(@"exclamationmark.triangle.fill",
                              @"Installation incomplète", kFontSizeSection);
    icon.imageScaling = NSImageScaleProportionallyDown;
    icon.contentTintColor = CMThemeColor(kError);
    icon.autoresizingMask = NSViewMaxXMargin;
    [row addSubview:icon];

    NSButton* diagnose =
        [NSButton buttonWithTitle:@"Diagnostic…"
                           target:self
                           action:@selector(showSetupDiagnostic:)];
    diagnose.identifier = @"startup.setup_notice.diagnose";
    [diagnose sizeToFit];
    const double buttonWidth = std::max(NSWidth(quit), NSWidth(diagnose.frame));
    diagnose.frame = NSMakeRect(NSWidth(rowFrame) - buttonWidth, kSpaceXs,
                                buttonWidth, NSHeight(quit));
    diagnose.autoresizingMask = NSViewMinXMargin;
    [row addSubview:diagnose];

    const double labelX = kIconSize + kSpaceS;
    NSTextField* label = [NSTextField wrappingLabelWithString:@""];
    label.identifier = @"startup.setup_notice.text";
    label.frame =
        NSMakeRect(labelX, 0, NSMinX(diagnose.frame) - kSpaceM - labelX,
                   NSHeight(rowFrame));
    label.font = CMFont(kFontSizeBody, NSFontWeightRegular);
    label.textColor = CMTextPrimaryColor();
    label.maximumNumberOfLines = 2;
    label.cell.truncatesLastVisibleLine = YES;
    label.autoresizingMask = NSViewWidthSizable;
    [row addSubview:label];
    [content addSubview:row];
    self.startupSetupNotice = row;
    self.startupSetupNoticeLabel = label;
    self.startupSetupNoticeChecked = NO;

    __weak AppDelegate* weakSelf = self;
    __weak NSWindow* window = self.startupWindow;
    [self gatherSetupNotice:^(const SetupNoticeResult& result) {
      AppDelegate* strongSelf = weakSelf;
      // A screen built again meanwhile has its own report on the way.
      if (!strongSelf || !window || strongSelf.startupWindow != window) return;
      [strongSelf
          applyStartupSetupNotice:setup_notice::WelcomeLine(result.report)];
    }];
}

- (void)applyStartupSetupNotice:(const std::string&)line {
    NSView* row = self.startupSetupNotice;
    NSTextField* label = self.startupSetupNoticeLabel;
    label.stringValue = CMStringFromUtf8(line);
    // Two lines at most, centred on the row.
    NSRect frame = label.frame;
    const NSSize needed = [label.cell
        cellSizeForBounds:NSMakeRect(0, 0, NSWidth(frame), CGFLOAT_MAX)];
    frame.size.height =
        std::min(std::ceil(needed.height), NSHeight(row.bounds));
    frame.origin.y = std::floor((NSHeight(row.bounds) - NSHeight(frame)) / 2.0);
    label.frame = frame;
    row.hidden = line.empty();
    self.startupSetupNoticeChecked = YES;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The new project's name, asked
// on the form panel: modal, the welcome screen having no window to hang a
// sheet from. Built apart, so the window test reads it without its modal
// loop.
- (CMFormPanel*)newProjectNamePanel {
    NSTextField* name =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 26)];
    name.identifier = @"project.new.name";
    name.stringValue = @"Nouveau projet";
    name.placeholderString = @"Nom du projet";
    CMFormPanel* panel =
        [[CMFormPanel alloc] initWithTitle:@"Créer un nouveau projet"
                                   message:@"Donnez un nom au projet."
                                      form:name];
    [panel addButtonWithTitle:@"Continuer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = name;
    return panel;
}

// The name typed in the new project's form, or « Nouveau projet ».
static NSString* CMNewProjectName(CMFormPanel* panel) {
    NSString* projectName =
        CMTrimmedName(((NSTextField*)panel.form).stringValue);
    return projectName.length > 0 ? projectName : @"Nouveau projet";
}

// Where the new project goes: the welcome screen runs it modal, the editor
// as a sheet (DEC39).
- (NSSavePanel*)newProjectSavePanelNamed:(NSString*)projectName {
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Enregistrer le nouveau projet";
    save.prompt = @"Créer";
    save.nameFieldStringValue =
        [projectName stringByAppendingString:@".cutmachine-project"];
    UTType* collectionType =
        [UTType typeWithIdentifier:@"com.cutmachine.project-collection"];
    if (collectionType) save.allowedContentTypes = @[ collectionType ];
    save.canCreateDirectories = YES;
    return save;
}

// The engine's package, made as create_project makes it: the project file
// written, or nil and why.
- (NSString*)createProjectNamed:(NSString*)projectName
                             at:(NSURL*)destination
                          error:(NSString**)failure {
    Project project(projectName.UTF8String ?: "Nouveau projet");
    project.timelines.front().name = "Timeline 1";
    std::string error;
    std::string path;
    if (!CreatePortableProject(destination.fileSystemRepresentation ?: "",
                               project, path, error)) {
        if (failure) *failure = CMStringFromUtf8(error);
        return nil;
    }
    return CMStringFromUtf8(path);
}

- (BOOL)createStartupProject {
    CMFormPanel* panel = [self newProjectNamePanel];
    __block NSModalResponse answer = NSModalResponseCancel;
    [panel presentInWindow:nil
                completion:^(NSModalResponse response) {
                  answer = response;
                }];
    if (answer != NSAlertFirstButtonReturn) return NO;
    NSString* projectName = CMNewProjectName(panel);

    NSSavePanel* save = [self newProjectSavePanelNamed:projectName];
    if ([save runModal] != NSModalResponseOK || !save.URL) return NO;

    NSString* failure = nil;
    NSString* path = [self createProjectNamed:projectName
                                           at:save.URL
                                        error:&failure];
    if (!path) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Impossible de créer le projet";
        alert.informativeText = failure;
        [alert runModal];
        return NO;
    }
    self.documentPath = path;
    return YES;
}

- (NSString*)resolvedProjectPath:(NSString*)selection {
    NSString* selected = selection.stringByStandardizingPath;
    BOOL directory = NO;
    if ([NSFileManager.defaultManager fileExistsAtPath:selected
                                           isDirectory:&directory] &&
        directory)
        selected = [selected
            stringByAppendingPathComponent:@"project.cutmachine.json"];
    return selected;
}

// The welcome screen runs it modal, the editor as a sheet (DEC39).
- (NSOpenPanel*)projectOpenPanel {
    NSOpenPanel* open = [NSOpenPanel openPanel];
    open.title = @"Ouvrir un projet CUTMACHINE";
    open.prompt = @"Ouvrir";
    UTType* collectionType =
        [UTType typeWithIdentifier:@"com.cutmachine.project-collection"];
    if (collectionType) open.allowedContentTypes = @[ collectionType ];
    open.allowsMultipleSelection = NO;
    open.canChooseFiles = YES;
    open.canChooseDirectories = YES;
    return open;
}

- (BOOL)openStartupProject {
    NSOpenPanel* open = [self projectOpenPanel];
    if ([open runModal] != NSModalResponseOK || !open.URL) return NO;
    self.documentPath = [self resolvedProjectPath:open.URL.path];
    return YES;
}

// DEC39 -- docs/roadmap/PHASES-2026-08.md. « Fichier › Nouveau projet… » and
// « Fichier › Ouvrir un projet… » in the editor, as Resolve's Project
// Manager changes project without leaving the program. The window does not
// swap its project in place: it checks the other one (ProjectSwitch.h),
// writes what it holds, and quits, and -applicationWillTerminate: starts a
// new editor on the other project once the lock of this one is released.
// The welcome window is not shown in between.

// Why the window cannot change project now, said on a sheet; NO then.
- (BOOL)projectSwitchAllowed {
    if (!self.state) return NO;
    size_t running = 0;
    if (self.state->mediaTasks) {
        for (const MediaTaskSnapshot& task : self.state->mediaTasks->Snapshot())
            if (task.state == MediaTaskState::Running ||
                task.state == MediaTaskState::Queued)
                ++running;
    }
    const std::string refusal =
        project_switch::BusyRefusal(self.state->exportRunning, running);
    if (refusal.empty()) return YES;
    [self showProjectSwitchRefusal:CMStringFromUtf8(refusal)];
    return NO;
}

- (void)showProjectSwitchRefusal:(NSString*)refusal {
    self.infoLabel.stringValue = refusal;
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Changement de projet impossible";
    alert.informativeText = refusal;
    if (self.window.attachedSheet == nil)
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
    else
        NSBeep();
}

- (void)newProjectFromEditor:(id)sender {
    (void)sender;
    if ([CMFormPanel formShownInWindow:self.window]) {
        NSBeep();
        return;
    }
    if (![self projectSwitchAllowed]) return;
    CMFormPanel* panel = [self newProjectNamePanel];
    __weak AppDelegate* weakSelf = self;
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  NSString* projectName = CMNewProjectName(panel);
                  // After this sheet is gone: the save panel is a sheet too.
                  dispatch_async(dispatch_get_main_queue(), ^{
                    [weakSelf chooseNewProjectLocationNamed:projectName];
                  });
                }];
}

- (void)chooseNewProjectLocationNamed:(NSString*)projectName {
    NSSavePanel* save = [self newProjectSavePanelNamed:projectName];
    [save beginSheetModalForWindow:self.window
                 completionHandler:^(NSModalResponse response) {
                   NSURL* chosen = CMSaveSheetAnswer(save, response);
                   if (!chosen) return;
                   NSString* failure = nil;
                   NSString* path = [self createProjectNamed:projectName
                                                          at:chosen
                                                       error:&failure];
                   if (!path) {
                       dispatch_async(dispatch_get_main_queue(), ^{
                         [self showProjectSwitchRefusal:
                                   [@"Impossible de créer le projet : "
                                       stringByAppendingString:failure]];
                       });
                       return;
                   }
                   dispatch_async(dispatch_get_main_queue(), ^{
                     [self switchToProjectAtPath:path];
                   });
                 }];
}

- (void)openProjectFromEditor:(id)sender {
    (void)sender;
    if (self.window.attachedSheet != nil) {
        NSBeep();
        return;
    }
    if (![self projectSwitchAllowed]) return;
    NSOpenPanel* open = [self projectOpenPanel];
    [open beginSheetModalForWindow:self.window
                 completionHandler:^(NSModalResponse response) {
                   if (response != NSModalResponseOK || !open.URL) return;
                   NSString* chosen = open.URL.path;
                   dispatch_async(dispatch_get_main_queue(), ^{
                     [self switchToProjectAtPath:chosen];
                   });
                 }];
}

// Checked again here, the panels having taken their time: busy, the same
// project, one that cannot be opened. Then what this project holds is
// written, and the editor quits to reopen on `chosen`. YES when it does
// (in the window test, when it would: the path is left in
// pendingProjectSwitchPath and nothing quits).
- (BOOL)switchToProjectAtPath:(NSString*)chosen {
    if (![self projectSwitchAllowed]) return NO;
    const project_switch::Plan plan = project_switch::PlanProjectSwitch(
        self.documentPath.fileSystemRepresentation ?: "",
        chosen.fileSystemRepresentation ?: "");
    if (plan.outcome == project_switch::Outcome::kAlreadyOpen) {
        self.infoLabel.stringValue = @"Ce projet est déjà ouvert";
        return NO;
    }
    if (plan.outcome == project_switch::Outcome::kRefused) {
        [self showProjectSwitchRefusal:CMStringFromUtf8(plan.refusal)];
        return NO;
    }
    NSString* executable = NSBundle.mainBundle.executablePath;
    if (executable.length == 0 ||
        access(executable.fileSystemRepresentation, X_OK) != 0) {
        [self showProjectSwitchRefusal:@"CUTMACHINE ne retrouve pas son "
                                       @"propre programme pour se rouvrir "
                                       @"sur l’autre projet."];
        return NO;
    }
    std::string message;
    if (![self persistEdits:message]) {
        [self showProjectSwitchRefusal:
                  [NSString stringWithFormat:@"Ce projet n’a pas pu être "
                                             @"enregistré avant de le "
                                             @"fermer : %@",
                                             CMStringFromUtf8(message)]];
        return NO;
    }
    self.pendingProjectSwitchPath = CMStringFromUtf8(plan.project_path);
#if !defined(CUTMACHINE_UI_SMOKE_TEST)
    std::fprintf(stderr, "Project switch: reopening on %s\n",
                 plan.project_path.c_str());
    [NSApp terminate:nil];
#endif
    return YES;
}

- (void)collectPortableProject:(id)sender {
    (void)sender;
    if (!self.state || self.documentPath.length == 0) return;
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Collecter le projet et ses médias";
    save.prompt = @"Collecter";
    save.canCreateDirectories = YES;
    NSString* projectName =
        [NSString stringWithUTF8String:self.state->project.name.c_str()];
    save.nameFieldStringValue =
        [projectName stringByAppendingString:@".cutmachine-project"];
    [save
        beginSheetModalForWindow:self.window
               completionHandler:^(NSModalResponse response) {
                 NSURL* chosen = CMSaveSheetAnswer(save, response);
                 if (!chosen) return;
                 NSString* source = [self.documentPath copy];
                 NSString* destination = [chosen.path copy];
                 // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. A progress
                 // panel in place of an alert (-collectProgressPanel).
                 CMFormPanel* progress = [self collectProgressPanel];
                 [(NSProgressIndicator*)progress.form startAnimation:nil];
                 [progress presentInWindow:self.window completion:nil];

                 auto result = std::make_shared<PortableProjectResult>();
                 auto error = std::make_shared<std::string>();
                 dispatch_async(
                     dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                       const bool success = CollectPortableProject(
                           source.fileSystemRepresentation ?: "",
                           destination.fileSystemRepresentation ?: "", *result,
                           *error);
                       dispatch_async(dispatch_get_main_queue(), ^{
                         [progress endWithResponse:NSModalResponseStop];
                         NSAlert* finished = [NSAlert new];
                         if (!success) {
                             finished.alertStyle = NSAlertStyleCritical;
                             finished.messageText = @"Collecte impossible";
                             finished.informativeText =
                                 [NSString stringWithUTF8String:error->c_str()];
                             [finished beginSheetModalForWindow:self.window
                                              completionHandler:nil];
                             return;
                         }
                         finished.messageText = @"Projet portable créé";
                         // FI12 -- docs/roadmap/FIABILITE-2026-10.md. A
                         // journal whose rewrite could not be proven is
                         // copied as it was: undoing there can reach the
                         // source's files, and the person is told.
                         const size_t verbatim =
                             result->verbatim_histories.size();
                         NSString* history =
                             verbatim == 0
                                 ? @""
                                 : [NSString
                                       stringWithFormat:
                                           @"\n%zu historique%@ copié%@ tel "
                                           @"quel : y annuler peut ramener "
                                           @"un fichier du projet source.",
                                           verbatim, verbatim == 1 ? @"" : @"s",
                                           verbatim == 1 ? @"" : @"s"];
                         finished.informativeText = [NSString
                             stringWithFormat:
                                 @"%llu média%@ · %.2f Go copiés\n%@%@",
                                 static_cast<unsigned long long>(
                                     result->media_count),
                                 result->media_count == 1 ? @"" : @"s",
                                 result->media_bytes / 1000000000.0,
                                 destination, history];
                         [finished
                             addButtonWithTitle:@"Révéler dans le Finder"];
                         [finished addButtonWithTitle:@"Fermer"];
                         [finished
                             beginSheetModalForWindow:self.window
                                    completionHandler:^(
                                        NSModalResponse choice) {
                                      if (choice == NSAlertFirstButtonReturn)
                                          [NSWorkspace.sharedWorkspace
                                              activateFileViewerSelectingURLs:@[
                                                  chosen
                                              ]];
                                    }];
                       });
                     });
               }];
}

- (BOOL)chooseStartupProject {
    while (self.documentPath.length == 0) {
        self.startupChoice = 0;
        self.startupSelectedPath = nil;
        [self showStartupWindow];
        if (self.startupChoice == 0) return NO;
        if (self.startupChoice == 1) {
            if ([self createStartupProject]) return YES;
        } else if (self.startupChoice == 2) {
            if ([self openStartupProject]) return YES;
        } else if (self.startupChoice == 3 &&
                   self.startupSelectedPath.length > 0) {
            self.documentPath = self.startupSelectedPath;
            return YES;
        }
    }
    return YES;
}

- (instancetype)initWithDocumentPath:(NSString*)documentPath {
    if ((self = [super init])) {
        _documentPath = [documentPath copy];
        _state = new AppState();
        _state->automaticProxiesEnabled = [NSUserDefaults.standardUserDefaults
            boolForKey:kAutomaticProxyGenerationDefaultsKey];
        _state->programVideoScope = VideoScopeModeFromPreference(
            static_cast<int32_t>([NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey]));
        [self loadMonitorCompare];
        _mediaThumbnails = [NSMutableDictionary dictionary];
    }
    return self;
}

- (BOOL)loadDocumentAndSources {
    if ([self loadDocumentAndSourcesHoldingLock]) return YES;
    // A load that fails must not keep the project locked. Several callers
    // react to NO with -[NSApp terminate:], which tears the process down
    // without unwinding C++ destructors, so ProjectSessionLock's own
    // destructor never runs and the .lock directory outlives the process.
    // Acquire() does reclaim a lock whose owning pid is gone, so this only
    // ever leaked litter rather than wedging the next open -- but the litter
    // is confusing next to a project that failed to open for another reason.
    if (self.state) self.state->projectLock.reset();
    return NO;
}

- (BOOL)loadDocumentAndSourcesHoldingLock {
    const std::string documentPath(self.documentPath.UTF8String ?: "");
    std::string error;
    self.lastProjectLoadError = nil;
    self.projectArchiveWarningShown = NO;
    self.state->projectLock = std::make_unique<ProjectSessionLock>();
    std::string lockOwner;
    if (!self.state->projectLock->Acquire(documentPath, lockOwner, error)) {
        self.lastProjectLoadError =
            [NSString stringWithUTF8String:error.c_str()];
        return NO;
    }
    if (!RecoverTextArtifactTransaction(
            std::filesystem::path(documentPath).parent_path().string(),
            error)) {
        self.lastProjectLoadError =
            [NSString stringWithFormat:@"Transaction projet irrécupérable : %@",
                                       CMStringFromUtf8(error)];
        return NO;
    }
    // SAVING_ROADMAP.md, the editor killed. A temporary of an editor killed
    // inside WriteAutosave: this one holds the lock, the only autosave
    // writer, so what is left is a dead writer's. Not fatal.
    size_t autosaveLeftovers = 0;
    if (!ProjectRecovery::DiscardAutosaveLeftovers(documentPath,
                                                   autosaveLeftovers, error))
        std::fprintf(stderr, "%s\n", error.c_str());
    bool recoveredAutosave = false;
    const ProjectRecoveryInfo recovery = ProjectRecovery::Inspect(documentPath);
    if (recovery.state == ProjectRecoveryState::Available) {
        NSAlert* alert = [NSAlert new];
        alert.messageText = @"Récupération disponible";
        // FI11 -- docs/roadmap/FIABILITE-2026-10.md. Beside an unreadable
        // project file the autosave is all that is left: declining keeps it
        // for the next opening instead of discarding it.
        alert.informativeText =
            recovery.primary_unreadable
                ? @"Le fichier du projet est illisible. La sauvegarde "
                  @"automatique est le seul état récupérable : elle est "
                  @"gardée si vous ne récupérez pas maintenant."
                : @"Une sauvegarde automatique valide est disponible pour "
                  @"récupérer le projet.";
        [alert addButtonWithTitle:@"Récupérer"];
        [alert addButtonWithTitle:recovery.primary_unreadable ? @"Plus tard"
                                                              : @"Ignorer"];
        // SAVING_ROADMAP.md, the editor killed. The forced-stop bench
        // (scripts/editor_forced_stop_bench.py) relaunches the editor
        // screen locked, where this alert would wait for a click that
        // cannot come. CUTMACHINE_RECOVERY_ANSWER=recover|decline answers
        // it as the first or second button would, and says so on stderr;
        // unset, or any other value, the person answers.
        NSModalResponse answer = NSAlertSecondButtonReturn;
        const char* scripted = std::getenv("CUTMACHINE_RECOVERY_ANSWER");
        const std::string scriptedAnswer = scripted != nullptr ? scripted : "";
        if (scriptedAnswer == "recover" || scriptedAnswer == "decline") {
            answer = scriptedAnswer == "recover" ? NSAlertFirstButtonReturn
                                                 : NSAlertSecondButtonReturn;
            std::fprintf(stderr,
                         "Recovery alert answered by "
                         "CUTMACHINE_RECOVERY_ANSWER: %s\n",
                         scriptedAnswer.c_str());
        } else {
            answer = [alert runModal];
        }
        if (answer == NSAlertFirstButtonReturn) {
            Project recovered;
            std::map<std::string, EditLog> recoveredLogs;
            ProjectEditLog recoveredProjectLog;
            if (!ProjectRecovery::LoadAutosave(documentPath, recovered,
                                               recoveredLogs,
                                               recoveredProjectLog, error)) {
                std::fprintf(stderr, "Unable to recover project: %s\n",
                             error.c_str());
                return NO;
            }
            if (!CommitStoredProjectAndLogs(documentPath, recovered,
                                            recoveredLogs, recoveredProjectLog,
                                            error)) {
                std::fprintf(stderr, "Unable to recover project: %s\n",
                             error.c_str());
                return NO;
            }
            self.state->project = std::move(recovered);
            recoveredAutosave = true;
        }
        if ((recoveredAutosave || !recovery.primary_unreadable) &&
            !ProjectRecovery::DiscardAutosave(documentPath, error))
            std::fprintf(stderr, "Unable to discard autosave: %s\n",
                         error.c_str());
    } else if (recovery.state == ProjectRecoveryState::Invalid) {
        std::fprintf(stderr, "Invalid autosave ignored: %s\n",
                     recovery.error.c_str());
    }
    // Recovery must run before package validation: an interrupted commit can
    // leave the main generation incomplete while its autosave is still the
    // only valid representation of the project.
    CollectionIntegrityReport integrity;
    if (!VerifyPortableProject(documentPath, integrity, error)) {
        self.lastProjectLoadError =
            [NSString stringWithFormat:@"Paquet projet invalide : %@",
                                       CMStringFromUtf8(error)];
        return NO;
    }
    if (!integrity.missing_media.empty() || !integrity.modified_media.empty() ||
        !integrity.damaged_looks.empty()) {
        NSAlert* warning = [NSAlert new];
        warning.alertStyle = NSAlertStyleCritical;
        warning.messageText = @"Intégrité du projet compromise";
        NSString* media = [NSString
            stringWithFormat:@"%lu média%@ manquant%@ et %lu modifié%@. "
                             @"Le montage peut être ouvert, mais son rendu "
                              "n’est plus garanti identique.",
                             (unsigned long)integrity.missing_media.size(),
                             integrity.missing_media.size() == 1 ? @"" : @"s",
                             integrity.missing_media.size() == 1 ? @"" : @"s",
                             (unsigned long)integrity.modified_media.size(),
                             integrity.modified_media.size() == 1 ? @"" : @"s"];
        // FI13 -- docs/roadmap/FIABILITE-2026-10.md. A damaged look opens
        // with the project; its timeline's renders refuse until the .cube
        // is imported again, which repairs the package's copy.
        if (!integrity.damaged_looks.empty()) {
            media = [media
                stringByAppendingFormat:
                    @"\n\nLook absent ou modifié sur %lu timeline%@. Ses "
                    @"rendus sont refusés jusqu’à ce que le fichier .cube "
                    @"soit réimporté.",
                    (unsigned long)integrity.damaged_looks.size(),
                    integrity.damaged_looks.size() == 1 ? @"" : @"s"];
        }
        warning.informativeText = media;
        [warning addButtonWithTitle:@"Ouvrir quand même"];
        [warning addButtonWithTitle:@"Annuler"];
        if ([warning runModal] != NSAlertFirstButtonReturn) {
            self.lastProjectLoadError =
                @"Ouverture annulée après contrôle d’intégrité.";
            return NO;
        }
    }
    if (!recoveredAutosave &&
        !LoadStoredProject(documentPath, self.state->project, error)) {
        std::fprintf(stderr, "Unable to load project: %s\n", error.c_str());
        return NO;
    }
    self.state->activeTimelineId = self.state->project.active_timeline_id;
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : self.state->project.timelines) {
        const std::string logPath =
            TimelineEditLogPathForProject(documentPath, timeline.id);
        std::error_code logExistsError;
        if (std::filesystem::exists(logPath, logExistsError)) {
            EditLog log;
            if (!EditLog::Load(logPath, log, editError, error)) {
                std::fprintf(stderr, "Unable to load timeline edit log: %s\n",
                             error.c_str());
                return NO;
            }
            self.state->timelineEditLogs[timeline.id] = std::move(log);
        }
        if (logExistsError) {
            std::fprintf(stderr, "Unable to inspect timeline edit log: %s\n",
                         logExistsError.message().c_str());
            return NO;
        }
    }
    const auto activeLog =
        self.state->timelineEditLogs.find(self.state->activeTimelineId);
    self.state->editLog = activeLog == self.state->timelineEditLogs.end()
                              ? EditLog{}
                              : activeLog->second;
    // DEC24 -- SAVING_ROADMAP.md. A journal written before S6 is migrated
    // here, under the lock this window holds, instead of being converted
    // again at every opening; one that cannot be written back still opens.
    ProjectEditLogMigration projectLogMigration;
    if (!LoadProjectEditLogMigrating(documentPath, self.state->projectEditLog,
                                     projectLogMigration, error)) {
        std::fprintf(stderr, "Unable to load project edit log: %s\n",
                     error.c_str());
        return NO;
    }
    if (!projectLogMigration.rewrite_error.empty()) {
        std::fprintf(stderr, "Project edit log not migrated: %s\n",
                     projectLogMigration.rewrite_error.c_str());
    }
    self.state->timelineEditLogs[self.state->activeTimelineId] =
        self.state->editLog;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        self.state->targetedTrackIds.insert(track.id);
    self.state->timelineTargetTracks[self.state->activeTimelineId] =
        self.state->targetedTrackIds;
    self.state->viewport.view_start = {0, 1};
    self.state->viewport.pixels_per_second = 100.0;
    self.state->viewport.track_height = ui::theme::kTimelineTrackHeight;
    self.state->viewport.header_width = ui::theme::kTimelineTrackHeaderWidth;
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    try {
        self.state->duration = self.state->timeline->Duration();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Invalid timeline duration: %s\n",
                     exception.what());
        return NO;
    }
    self.state->frameCache = std::make_unique<FrameCache>(kGlobalCacheBudget);
    self.state->performanceMetrics = std::make_unique<PerformanceMetrics>();
    const std::filesystem::path baseDirectory =
        std::filesystem::absolute(std::filesystem::path(documentPath))
            .parent_path();
    // PERF-2026-09. This used to re-probe every rush on every open -- an
    // avformat_open_input plus avformat_find_stream_info per source,
    // measured at about 110 ms for a 256 MB camera file -- to fill a map the
    // interface reads for rotation, colour and channel counts.
    //
    // LibraryMedia already carries every one of those fields, and
    // `metadata_complete` already says whether they were probed. The rest of
    // the project trusts exactly that: Operations.cc validates inserts
    // against it, Export.cc resolves the rendered deliverable from it,
    // Relink and TimelineSheets read it. Only this path went back to the
    // file, which meant the monitor could show a rotation the export would
    // not apply -- two answers to one question, and the slow one was not the
    // authoritative one.
    //
    // So the store answers, and the drive is only consulted for the entries
    // it cannot answer for: version-1 media promoted without technical
    // metadata (`metadata_complete == false`, see Document.cc), and sources
    // with no library entry at all. In a project ingested by any current
    // version that is nothing, and opening it costs no media I/O here.
    //
    // The trade is that a rush swapped on disk behind the project's back no
    // longer changes the monitor until it is re-ingested or relinked. It
    // never changed the export, so this makes the two agree instead of
    // letting them drift; a swap that matters is still caught when the
    // decoder opens, by DecodeWorkerMatchesSource.
    const std::vector<DocumentSource>& openSources =
        self.state->document.sources;
    struct ProbedSource {
        bool needs_probe = false;
        bool probed = false;
        LibraryMedia detected;
        std::string media_path;
        std::string probe_error;
    };
    std::vector<ProbedSource> probes(openSources.size());
    for (size_t index = 0; index < openSources.size(); ++index) {
        const DocumentSource& source = openSources[index];
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(source.id);
        if (media && media->metadata_complete) {
            self.state->mediaMetadata[source.id] = *media;
            continue;
        }
        ProbedSource& probe = probes[index];
        probe.needs_probe = true;
        std::filesystem::path mediaPath(source.path);
        if (mediaPath.is_relative()) mediaPath = baseDirectory / mediaPath;
        probe.media_path = mediaPath.lexically_normal().string();
        probe.detected.id = source.id;
        probe.detected.path = media ? media->path : source.path;
        probe.detected.filename = mediaPath.filename().string();
    }
    // Whatever is left is legacy, and rare, so it still goes to the drive --
    // together rather than one at a time. See ParallelForEachIndex above.
    ParallelForEachIndex(probes.size(), [&](size_t index) {
        ProbedSource& probe = probes[index];
        if (!probe.needs_probe) return;
        probe.probed = ProbeMediaMetadata(probe.media_path, probe.detected,
                                          probe.probe_error);
    });
    for (size_t index = 0; index < openSources.size(); ++index) {
        const DocumentSource& source = openSources[index];
        const ProbedSource& probe = probes[index];
        if (!probe.needs_probe) continue;
        if (probe.probed) {
            const LibraryMedia& detected = probe.detected;
            self.state->mediaMetadata[source.id] = detected;
            std::fprintf(
                stderr,
                "Media %s: %dx%d, %s/%s, range=%s matrix=%s transfer=%s "
                "primaries=%s, rotation %d degrees, %s\n",
                detected.filename.c_str(), detected.width, detected.height,
                detected.codec.c_str(), detected.pixel_format.c_str(),
                detected.color_range.c_str(), detected.color_space.c_str(),
                detected.color_transfer.c_str(),
                detected.color_primaries.c_str(), detected.rotation_degrees,
                detected.orientation.c_str());
        } else {
            std::fprintf(stderr, "Metadata probe failed for %s: %s\n",
                         probe.media_path.c_str(), probe.probe_error.c_str());
        }
    }

    if (![self separateEmbeddedAudioByDefault:error]) {
        std::fprintf(stderr, "Unable to separate embedded audio: %s\n",
                     error.c_str());
        return NO;
    }
    self.state->audioPlayback = std::make_unique<AudioPlayback>();
    if (!self.state->audioPlayback->Open(self.state->document,
                                         baseDirectory.string(), error)) {
        std::fprintf(stderr, "Unable to initialize audio: %s\n", error.c_str());
        return NO;
    }
    // PERF-2026-09. The decoders are deliberately not opened here. Each one
    // is an avformat_open_input plus avformat_find_stream_info against the
    // media itself, and doing them before the window exists is what made
    // opening a project on an external drive show nothing at all for
    // seconds. Since PERF10 they open when the monitor first asks for their
    // source (-requestFrame:ofSource:), and
    // -checkSourceAvailabilityInBackground only checks, once the editor is on
    // screen, that each file is there.
    [self rebuildVideoTrackIds];
    [self reportPendingProjectArchive];
    return YES;
}

- (BOOL)separateEmbeddedAudioByDefault:(std::string&)message {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    std::vector<Ulid> videoClipIds;
    for (const DocumentTrack& track : stagedDocument.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            if (!clip.include_audio) continue;
            const auto detected =
                self.state->mediaMetadata.find(clip.source_id);
            const LibraryMedia* media =
                detected == self.state->mediaMetadata.end()
                    ? stagedDocument.FindLibraryMedia(clip.source_id)
                    : &detected->second;
            if (media && media->metadata_complete && media->has_audio)
                videoClipIds.push_back(clip.id);
        }
    }
    EditError error = EditError::None;
    for (const Ulid& videoClipId : videoClipIds) {
        const Ulid audioClipId = GenerateUlid();
        Ulid targetTrackId;
        for (const DocumentTrack* track :
             TimelineTracksInDisplayOrder(stagedDocument)) {
            if (track->kind != "audio") continue;
            Document candidate = stagedDocument;
            Operation probe =
                DetachAudioOperation{videoClipId, track->id, audioClipId, {}};
            Operation inverse = RemoveClipOperation{};
            EditError probeError = EditError::None;
            std::string probeMessage;
            if (ApplyOperation(candidate, probe, inverse, probeError,
                               probeMessage)) {
                targetTrackId = track->id;
                break;
            }
        }
        if (targetTrackId.empty()) {
            int32_t index = 0;
            for (const DocumentTrack& track : stagedDocument.sequence.tracks)
                index = std::max(index, track.index + 1);
            targetTrackId = GenerateUlid();
            if (!stagedLog.Apply(
                    stagedDocument,
                    Operation{AddTrackOperation{targetTrackId, "audio", index}},
                    error, message))
                return NO;
        }
        if (!stagedLog.Apply(stagedDocument,
                             Operation{DetachAudioOperation{
                                 videoClipId, targetTrackId, audioClipId, {}}},
                             error, message))
            return NO;
    }
    size_t migratedLinks = 0;
    std::vector<Ulid> claimedAudio;
    struct LegacyPair {
        Ulid video;
        Ulid audio;
        Ulid group;
    };
    std::vector<LegacyPair> legacyPairs;
    for (const DocumentTrack& videoTrack : stagedDocument.sequence.tracks) {
        if (videoTrack.kind != "video") continue;
        for (const DocumentClip& video : videoTrack.clips) {
            if (video.include_audio || !video.sync_anchor_clip_id.empty())
                continue;
            const DocumentClip* match = nullptr;
            for (const DocumentTrack& audioTrack :
                 stagedDocument.sequence.tracks) {
                if (audioTrack.kind != "audio") continue;
                for (const DocumentClip& audio : audioTrack.clips) {
                    if (!audio.sync_anchor_clip_id.empty() ||
                        std::find(claimedAudio.begin(), claimedAudio.end(),
                                  audio.id) != claimedAudio.end() ||
                        audio.source_id != video.source_id ||
                        audio.source_in != video.source_in ||
                        audio.duration != video.duration ||
                        audio.timeline_in != video.timeline_in)
                        continue;
                    match = &audio;
                    break;
                }
                if (match) break;
            }
            if (!match) continue;
            const Ulid audioId = match->id;
            const Ulid groupId =
                !video.link_group_id.empty()
                    ? video.link_group_id
                    : (!match->link_group_id.empty() ? match->link_group_id
                                                     : audioId);
            legacyPairs.push_back({video.id, audioId, groupId});
            claimedAudio.push_back(audioId);
        }
    }
    for (const auto& pair : legacyPairs) {
        if (!stagedLog.Apply(
                stagedDocument,
                Operation{SetClipLinkOperation{pair.video, pair.audio,
                                               pair.group, pair.group}},
                error, message))
            return NO;
        ++migratedLinks;
    }
    if (!videoClipIds.empty() || migratedLinks > 0) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message])
            return NO;
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        std::fprintf(
            stderr,
            "Separated %zu embedded audio clip(s), migrated %zu A/V link(s)\n",
            videoClipIds.size(), migratedLinks);
    }
    return YES;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
#if !defined(CUTMACHINE_UI_SMOKE_TEST)
    // PRD35 -- ROADMAP.md. The app keeps the engine log; the window test,
    // built from these sources, is a test binary and keeps none.
    engine_log::StartProcessLog("app", true);
    // DEC23 -- before the welcome screen reads the setup report. Not in the
    // window test: it would write the settings file of whoever runs it.
    [self migrateWhisperModelPreference];
#endif
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    const BOOL pathWasProvided = self.documentPath.length > 0;
    while (true) {
        if (self.documentPath.length == 0 && ![self chooseStartupProject]) {
            [NSApp terminate:nil];
            return;
        }
        if ([self loadDocumentAndSources]) break;
        NSAlert* failure = [NSAlert new];
        failure.alertStyle = NSAlertStyleCritical;
        failure.messageText = @"Impossible d’ouvrir le projet";
        failure.informativeText =
            self.lastProjectLoadError
                ?: @"Le fichier n’est pas un projet CUTMACHINE valide ou il "
                   @"est inaccessible.";
        [failure runModal];
        if (pathWasProvided) {
            [NSApp terminate:nil];
            return;
        }
        self.documentPath = nil;
        delete self.state;
        self.state = new AppState();
        self.state->automaticProxiesEnabled =
            [NSUserDefaults.standardUserDefaults
                boolForKey:kAutomaticProxyGenerationDefaultsKey];
        self.state->programVideoScope = VideoScopeModeFromPreference(
            static_cast<int32_t>([NSUserDefaults.standardUserDefaults
                integerForKey:kProgramVideoScopeDefaultsKey]));
        [self loadMonitorCompare];
        self.mediaThumbnails = [NSMutableDictionary dictionary];
    }
    [self recordRecentProject:self.documentPath];
    [self installApplicationMenus];

    const NSRect windowRect = NSMakeRect(0.0, 0.0, 1600.0, 960.0);
    self.window =
        [[NSWindow alloc] initWithContentRect:windowRect
                                    styleMask:(NSWindowStyleMaskTitled |
                                               NSWindowStyleMaskClosable |
                                               NSWindowStyleMaskResizable)
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title = CMWindowTitle(self.state->project.name,
                                      self.state->document.sequence.name);
    self.window.delegate = self;
    self.window.acceptsMouseMovedEvents = YES;
    self.window.contentMinSize = NSMakeSize(900.0, 560.0);

    NSView* content = [[NSView alloc] initWithFrame:windowRect];
    content.wantsLayer = YES;
    content.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfaceBase).CGColor;
    self.window.contentView = content;
    constexpr double mediaPanelWidth = 320.0;
    // F2.1 design system: fixed-width right dock (Inspector/Chat tabs).
    // Fixed, not user-resized-and-remembered, on purpose -- see
    // PanelHostView.h.
    constexpr double rightDockWidth = 300.0;
    // UI-2026-08 -- transport and zoom now belong to the timeline display
    // list. The only fixed bottom strip is the 42 pt status surface.
    const double workspaceHeight = windowRect.size.height - 42.0;
    self.workspaceSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 42.0, windowRect.size.width,
                                 workspaceHeight)];
    self.workspaceSplitView.vertical = YES;
    self.workspaceSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.workspaceSplitView.delegate = self;
    self.workspaceSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    // ON1 -- docs/roadmap/ONGLETS-2026-10.md. No autosaved geometry: the
    // panes a page shows, and how wide, come from the page and the widths
    // the hand gave it (UiPreferences.h), and the number of panes changes
    // from one page to the next. Autosaved geometry is positional, and a
    // two-pane frame restored onto three panes bricked the window once
    // (the old ".3pane" name retired it then).
    [content addSubview:self.workspaceSplitView];
    self.mediaPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth, workspaceHeight)];
    self.mediaPanel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.mediaPanel.wantsLayer = YES;
    // Same surface token as the new right dock (UiTheme.h kSurfacePanel) --
    // both are side panels of the same visual kind.
    self.mediaPanel.layer.backgroundColor = CMSurfacePanelColor().CGColor;
    [self.workspaceSplitView addArrangedSubview:self.mediaPanel];

    // F2.3 -- ROADMAP.md: the Média/Audio/Légendes tabs of the one
    // PanelSlot::Media (MediaPanelModel.h), a strip off the top and one
    // container per tab under it. UI-2026-08 had left the dock a media
    // browser alone (#12); they are back since the visual QA of 19 September
    // 2026, the caption tab dressing cartons only.
    const double mediaContentHeight =
        workspaceHeight - ui::theme::kTabStripHeight;
    NSMutableArray<NSString*>* mediaTabTitles = [NSMutableArray array];
    for (ui::media_panel::Tab tab : ui::media_panel::AllTabs())
        [mediaTabTitles
            addObject:[NSString
                          stringWithUTF8String:ui::media_panel::TabTitle(tab)]];
    self.mediaTabStrip = [[CMTabStripView alloc]
        initWithFrame:NSMakeRect(0.0, mediaContentHeight, mediaPanelWidth,
                                 ui::theme::kTabStripHeight)
               titles:mediaTabTitles];
    self.mediaTabStrip.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    self.mediaTabStrip.target = self;
    self.mediaTabStrip.action = @selector(mediaTabChanged:);
    [self.mediaPanel addSubview:self.mediaTabStrip];
    constexpr double binSidebarX = 8.0;
    constexpr double binSidebarWidth = 112.0;
    constexpr double mediaBrowserX = 128.0;
    constexpr double mediaBrowserWidth = mediaPanelWidth - mediaBrowserX - 8.0;

    self.mediaTabContentMedia =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth,
                                                 mediaContentHeight)];
    self.mediaTabContentMedia.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentMedia];

    NSTextField* libraryTitle =
        [NSTextField labelWithString:@"PROJET / CHUTIERS"];
    libraryTitle.frame = NSMakeRect(14.0, mediaContentHeight - 34.0,
                                    mediaPanelWidth - 28.0, 18.0);
    libraryTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    libraryTitle.font = [NSFont systemFontOfSize:11.0
                                          weight:NSFontWeightSemibold];
    libraryTitle.textColor = NSColor.secondaryLabelColor;
    [self.mediaTabContentMedia addSubview:libraryTitle];

    self.binOutline = [[ContextOutlineView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, binSidebarWidth,
                                 mediaContentHeight - 154.0)];
    NSTableColumn* binColumn =
        [[NSTableColumn alloc] initWithIdentifier:@"bin"];
    [self.binOutline addTableColumn:binColumn];
    self.binOutline.outlineTableColumn = binColumn;
    self.binOutline.headerView = nil;
    self.binOutline.dataSource = self;
    self.binOutline.delegate = self;
    self.binOutline.rowHeight = 26.0;
    self.binOutline.indentationPerLevel = 12.0;
    self.binOutline.backgroundColor = CMSurfacePanelColor();
    binColumn.width = binSidebarWidth;
    // PRD37 -- ROADMAP.md. A deep bin does not widen the column further: its
    // name ends in an ellipsis at the list's edge instead (« 1_RUSH »,
    // « Chutier re » cut there, QA of 3 October 2026; ContextOutlineView
    // keeps every cell inside the list).
    self.binOutline.autoresizesOutlineColumn = NO;
    // And a selected bin is drawn in the theme's row
    // (-outlineView:rowViewForItem:, UiTheme.h kListRowSelected), with
    // nothing of AppKit's over it: its blue, or its grey once the list is
    // not focused, covered that row and left the name at 2.05:1.
    self.binOutline.selectionHighlightStyle =
        NSTableViewSelectionHighlightStyleNone;
    [self.binOutline registerForDraggedTypes:@[
        kCutmachineMediaPasteboardType, kCutmachineBinPasteboardType,
        kCutmachineTimelinePasteboardType, NSPasteboardTypeFileURL
    ]];
    [self.binOutline setDraggingSourceOperationMask:NSDragOperationMove
                                           forLocal:YES];
    NSScrollView* binScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(binSidebarX, 112.0, binSidebarWidth,
                                 mediaContentHeight - 154.0)];
    binScroll.autoresizingMask = NSViewHeightSizable | NSViewMaxXMargin;
    binScroll.documentView = self.binOutline;
    binScroll.hasVerticalScroller = YES;
    binScroll.borderType = NSNoBorder;
    binScroll.drawsBackground = YES;
    binScroll.backgroundColor = CMSurfacePanelColor();
    [self.mediaTabContentMedia addSubview:binScroll];
    self.binSidebarScroll = binScroll;

    self.mediaSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(mediaBrowserX, mediaContentHeight - 70.0,
                                 mediaBrowserWidth - 80.0, 26.0)];
    // PRD37 -- ROADMAP.md. No prompt in the field: forty points of text room
    // at the QA window's width held « Reche » of « Rechercher nom, codec,
    // format… », and a narrower window none of it (QA of 3 October 2026).
    // The magnifier says search, as in the editors the timelines go to; what
    // is searched is said where nothing cuts it. Empty, not nil: a nil
    // prompt is AppKit's own « Search », in English and cut as well.
    self.mediaSearchField.placeholderString = @"";
    self.mediaSearchField.toolTip = @"Rechercher par nom, codec ou format";
    self.mediaSearchField.accessibilityLabel =
        @"Rechercher par nom, codec ou format";
    self.mediaSearchField.target = self;
    self.mediaSearchField.action = @selector(mediaSearchChanged:);
    self.mediaSearchField.continuous = YES;
    self.mediaSearchField.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentMedia addSubview:self.mediaSearchField];

    self.mediaViewToggle = [[NSSegmentedControl alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 80.0,
                                 mediaContentHeight - 70.0, 72.0, 26.0)];
    self.mediaViewToggle.segmentCount = 2;
    [self.mediaViewToggle setImage:SystemSymbol(@"list.bullet", @"Vue liste")
                        forSegment:0];
    [self.mediaViewToggle
          setImage:SystemSymbol(@"square.grid.2x2", @"Vue grille")
        forSegment:1];
    // PRD27 -- the two segments carry only a symbol: VoiceOver reads the
    // description SystemSymbol gives each image, the pointer gets it too.
    [self.mediaViewToggle setToolTip:@"Vue liste" forSegment:0];
    [self.mediaViewToggle setToolTip:@"Vue grille" forSegment:1];
    self.mediaViewToggle.selectedSegment = 1;
    self.mediaViewToggle.target = self;
    self.mediaViewToggle.action = @selector(mediaViewChanged:);
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin;
    self.mediaViewToggle.autoresizingMask = NSViewMinYMargin | NSViewMinXMargin;
    [self.mediaTabContentMedia addSubview:self.mediaViewToggle];

    self.mediaTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth - 24.0, 500.0)];
    for (NSArray<NSString*>* definition in @[
             @[ @"name", @"Nom", @"145" ], @[ @"format", @"Format", @"85" ],
             @[ @"duration", @"Durée", @"60" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.mediaTable addTableColumn:column];
    }
    self.mediaTable.dataSource = self;
    self.mediaTable.delegate = self;
    self.mediaTable.allowsMultipleSelection = YES;
    self.mediaTable.rowHeight = 44.0;
    self.mediaTable.usesAlternatingRowBackgroundColors = NO;
    self.mediaListScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(mediaBrowserX, 112.0, mediaBrowserWidth,
                                 mediaContentHeight - 190.0)];
    self.mediaListScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaListScroll.documentView = self.mediaTable;
    self.mediaListScroll.hasVerticalScroller = YES;
    self.mediaListScroll.hasHorizontalScroller = YES;
    self.mediaListScroll.borderType = NSBezelBorder;
    [self.mediaTabContentMedia addSubview:self.mediaListScroll];

    NSCollectionViewFlowLayout* iconLayout =
        [[NSCollectionViewFlowLayout alloc] init];
    iconLayout.itemSize = NSMakeSize(132.0, 112.0);
    iconLayout.minimumInteritemSpacing = 6.0;
    iconLayout.minimumLineSpacing = 8.0;
    iconLayout.sectionInset = NSEdgeInsetsMake(8, 8, 8, 8);
    self.mediaCollection = [[ContextCollectionView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaBrowserWidth, 500.0)];
    self.mediaCollection.collectionViewLayout = iconLayout;
    self.mediaCollection.dataSource = self;
    self.mediaCollection.delegate = self;
    self.mediaCollection.selectable = YES;
    self.mediaCollection.allowsMultipleSelection = YES;
    [self.mediaCollection
        setDraggingSourceOperationMask:kMediaLocalDragOperations
                              forLocal:YES];
    [self.mediaCollection setDraggingSourceOperationMask:NSDragOperationCopy
                                                forLocal:NO];
    [self.mediaCollection registerForDraggedTypes:@[
        kCutmachineMediaPasteboardType, kCutmachineBinPasteboardType,
        kCutmachineTimelinePasteboardType, NSPasteboardTypeFileURL
    ]];
    [self.mediaCollection registerClass:MediaIconItem.class
                  forItemWithIdentifier:@"media-icon"];
    self.mediaIconScroll =
        [[NSScrollView alloc] initWithFrame:self.mediaListScroll.frame];
    self.mediaIconScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.mediaIconScroll.documentView = self.mediaCollection;
    self.mediaIconScroll.hasVerticalScroller = YES;
    self.mediaIconScroll.borderType = NSBezelBorder;
    self.mediaListScroll.hidden = YES;
    self.mediaIconScroll.hidden = NO;
    [self.mediaTabContentMedia addSubview:self.mediaIconScroll];

    self.mediaTable.target = self;
    self.mediaTable.doubleAction = @selector(openSelectedMediaInSourceMonitor:);
    ContextCollectionView* iconCollection =
        (ContextCollectionView*)self.mediaCollection;
    iconCollection.doubleClickTarget = self;
    iconCollection.doubleClickAction = @selector(openIconItem:);
    [self.mediaTable setDraggingSourceOperationMask:kMediaLocalDragOperations
                                           forLocal:YES];

    self.assignMediaButton = CMMakeStyledButton(@"IMPORTER DES RUSHES…", self,
                                                @selector(importMediaPressed:));
    self.assignMediaButton.frame = NSMakeRect(12.0, 76.0, 184.0, 28.0);
    // PRD37 -- it takes what the row has left of SOURCE, wider or narrower.
    self.assignMediaButton.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    self.assignMediaButton.image =
        SystemSymbol(@"square.and.arrow.down", @"Importer des rushes");
    self.assignMediaButton.imagePosition = NSImageLeading;
    [self.mediaTabContentMedia addSubview:self.assignMediaButton];

    self.sourceMonitorButton = CMMakeStyledButton(
        @"SOURCE", self, @selector(openSelectedMediaInSourceMonitor:));
    self.sourceMonitorButton.frame = NSMakeRect(202.0, 76.0, 106.0, 28.0);
    // PRD37 -- ROADMAP.md. Its mask was set twice, the second one kept: the
    // button followed the panel's right edge over a neighbour of fixed
    // width, and a narrower window put SOURCE on « IMPORTER DES RUSHES… »
    // (QA of 3 October 2026). It still follows that edge; the import button
    // gives up the room instead (its mask above).
    self.sourceMonitorButton.autoresizingMask =
        NSViewMinXMargin | NSViewMaxYMargin;
    self.sourceMonitorButton.image =
        SystemSymbol(@"rectangle.on.rectangle", @"Moniteur Source");
    self.sourceMonitorButton.imagePosition = NSImageLeading;
    self.sourceMonitorButton.toolTip =
        @"Ouvre le média sélectionné dans le moniteur source";
    [self.mediaTabContentMedia addSubview:self.sourceMonitorButton];

    NSMenu* binContext = [[NSMenu alloc] initWithTitle:@"Chutier"];
    [binContext addItem:[self menuItem:@"Nouveau chutier"
                                action:@selector(createBinPressed:)
                                   key:@""]];
    [binContext addItem:[self menuItem:@"Renommer"
                                action:@selector(renameBinPressed:)
                                   key:@""]];
    [binContext addItem:NSMenuItem.separatorItem];
    [binContext addItem:[self menuItem:@"Supprimer"
                                action:@selector(deleteBinPressed:)
                                   key:@""]];
    self.binOutline.menu = binContext;

    NSMenu* mediaContext = [[NSMenu alloc] initWithTitle:@"Média"];
    [mediaContext addItem:[self menuItem:@"Nouveau chutier"
                                  action:@selector(createBinPressed:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext
        addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                        action:@selector(openSelectedMediaInSourceMonitor:)
                           key:@""]];
    // PRD39 -- ROADMAP.md. It asks for the destination: it said it moved
    // into the selected bin.
    [mediaContext addItem:[self menuItem:@"Déplacer vers un chutier…"
                                  action:@selector(assignMediaToBinPressed:)
                                     key:@""]];
    [mediaContext
        addItem:[self menuItem:@"Nouvelle timeline avec la sélection"
                        action:@selector(createTimelineFromSelectedMedia:)
                           key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Révéler dans le Finder"
                                  action:@selector(revealSelectedMediaInFinder:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Reconnecter le média…"
                                  action:@selector(relinkSelectedMedia:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Reconnecter les médias offline…"
                                  action:@selector(batchRelinkOfflineMedia:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    // G3 -- docs/roadmap/GRADE-2026-09.md. The colour input the selected
    // media are read as: the choices MediaPanelModel.h lists, checked in
    // -validateMenuItem:, and the file read again as ingest reads it.
    NSMenu* colorInputMenu =
        [[NSMenu alloc] initWithTitle:@"Espace couleur d’entrée"];
    const auto& colorInputChoices = ui::media_panel::ColorInputChoices();
    for (size_t index = 0; index < colorInputChoices.size(); ++index) {
        NSMenuItem* choice =
            [self menuItem:CMStringFromUtf8(colorInputChoices[index].label)
                    action:@selector(setSelectedMediaColorInput:)
                       key:@""];
        choice.tag = static_cast<NSInteger>(index);
        [colorInputMenu addItem:choice];
        if (index == 0) [colorInputMenu addItem:NSMenuItem.separatorItem];
    }
    [colorInputMenu addItem:NSMenuItem.separatorItem];
    [colorInputMenu
        addItem:[self menuItem:@"Relire dans le fichier"
                        action:@selector(rereadSelectedMediaColorInput:)
                           key:@""]];
    NSMenuItem* colorInputItem =
        [[NSMenuItem alloc] initWithTitle:@"Espace couleur d’entrée"
                                   action:nil
                            keyEquivalent:@""];
    colorInputItem.submenu = colorInputMenu;
    [mediaContext addItem:colorInputItem];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Transcrire avec Whisper…"
                                  action:@selector(transcribeSelectedMedia:)
                                     key:@""]];
    [mediaContext addItem:NSMenuItem.separatorItem];
    [mediaContext addItem:[self menuItem:@"Générer / recréer le proxy"
                                  action:@selector(generateSelectedMediaProxy:)
                                     key:@""]];
    [mediaContext addItem:[self menuItem:@"Supprimer le proxy"
                                  action:@selector(removeSelectedMediaProxy:)
                                     key:@""]];
    [mediaContext
        addItem:[self menuItem:@"Régénérer la vignette"
                        action:@selector(regenerateSelectedMediaThumbnail:)
                           key:@""]];
    self.mediaTable.menu = mediaContext;
    self.mediaCollection.menu = mediaContext;

    self.binSummaryLabel = [NSTextField labelWithString:@""];
    self.binSummaryLabel.frame =
        NSMakeRect(14.0, 42.0, mediaPanelWidth - 28.0, 34.0);
    self.binSummaryLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.binSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.binSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.binSummaryLabel.maximumNumberOfLines = 2;
    [self.mediaTabContentMedia addSubview:self.binSummaryLabel];
    self.mediaTaskLabel = [NSTextField labelWithString:@"Tâches média : prêt"];
    self.mediaTaskLabel.frame =
        NSMakeRect(14.0, 13.0, mediaPanelWidth - 152.0, 18.0);
    self.mediaTaskLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.mediaTaskLabel.font = [NSFont systemFontOfSize:10.0];
    self.mediaTaskLabel.textColor = NSColor.tertiaryLabelColor;
    self.mediaTaskLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [self.mediaTabContentMedia addSubview:self.mediaTaskLabel];
    self.mediaTaskProgress = [[NSProgressIndicator alloc]
        initWithFrame:NSMakeRect(mediaPanelWidth - 132.0, 16.0, 88.0, 10.0)];
    self.mediaTaskProgress.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskProgress.minValue = 0.0;
    self.mediaTaskProgress.maxValue = 100.0;
    self.mediaTaskProgress.doubleValue = 0.0;
    self.mediaTaskProgress.indeterminate = NO;
    // PRD37 -- shown while a task waits or runs (-refreshMediaTaskStatus).
    self.mediaTaskProgress.hidden = YES;
    [self.mediaTabContentMedia addSubview:self.mediaTaskProgress];
    self.mediaTaskCancelButton =
        CMMakeStyledButton(@"×", self, @selector(cancelDisplayedMediaTask:));
    self.mediaTaskCancelButton.frame =
        NSMakeRect(mediaPanelWidth - 40.0, 7.0, 28.0, 26.0);
    self.mediaTaskCancelButton.autoresizingMask =
        NSViewMaxYMargin | NSViewMinXMargin;
    self.mediaTaskCancelButton.toolTip = @"Annuler la tâche média active";
    // PRD27 -- VoiceOver would read « × ».
    self.mediaTaskCancelButton.accessibilityLabel =
        self.mediaTaskCancelButton.toolTip;
    self.mediaTaskCancelButton.enabled = NO;
    [self.mediaTabContentMedia addSubview:self.mediaTaskCancelButton];

    [self buildAudioTabContentWithWidth:mediaPanelWidth
                                 height:mediaContentHeight];
    [self buildCaptionsTabContentWithWidth:mediaPanelWidth
                                    height:mediaContentHeight];
    self.mediaTabContentAudio.hidden = YES;
    self.mediaTabContentCaptions.hidden = YES;
    // EF1 -- docs/roadmap/EFFETS-2026-10.md. The effects library.
    self.effectsPanelView = [[CMEffectsPanelView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, mediaPanelWidth,
                                 mediaContentHeight)];
    self.effectsPanelView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.effectsPanelView.delegate = self;
    self.effectsPanelView.hidden = YES;
    [self.mediaPanel addSubview:self.effectsPanelView];
    NSString* rememberedMediaTab =
        [CMUiPreferences.sharedPreferences lastMediaPanelTab];
    if (const auto tab = ui::media_panel::TabFromIdentifier(
            rememberedMediaTab.UTF8String ?: "")) {
        const auto& tabs = ui::media_panel::AllTabs();
        const auto found = std::find(tabs.begin(), tabs.end(), *tab);
        if (found != tabs.begin() && found != tabs.end())
            [self.mediaTabStrip
                selectIndex:static_cast<NSInteger>(found - tabs.begin())];
    }

    const double editorWidth =
        windowRect.size.width - mediaPanelWidth - rightDockWidth - 2.0;
    self.editorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth, workspaceHeight)];
    self.editorSplitView.vertical = NO;
    self.editorSplitView.arrangesAllSubviews = NO;
    self.editorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.editorSplitView.delegate = self;
    // v5 retires every earlier key: v4 and before were written while the panes
    // were inserted in the reverse of their visual order and their frames
    // rewritten afterwards, so the saved coordinates describe a composition
    // this build no longer produces.
    self.editorSplitView.autosaveName =
        @"CUTMACHINE.EditorSplit.MonitorsTop.v5";
    self.editorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;

    // NSSplitView is flipped: arranged subview 0 is the *top* pane of a
    // horizontal split and -setPosition: measures from the top edge. The
    // monitors therefore go in first and the timeline second -- the order
    // they appear on screen -- and AppKit needs no help holding them there.
    const double editorDivider = self.editorSplitView.dividerThickness;
    const double monitorPaneHeight = workspaceHeight * 0.44;
    const double timelinePaneTop = monitorPaneHeight + editorDivider;
    self.metalView = [[TimelineMetalView alloc]
        initWithFrame:NSMakeRect(0.0, timelinePaneTop, editorWidth,
                                 workspaceHeight - timelinePaneTop)];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.metalView.eventTarget = self;
    self.metalView.resizeTarget = self;
    self.metalView.acceptsEffects = YES;
    // ON8 -- docs/roadmap/ONGLETS-2026-10.md. The trim bar, in the tool row.
    self.trimBarView = [[CMTrimBarView alloc] initWithFrame:NSZeroRect];
    self.trimBarView.delegate = self;
    [self.metalView addSubview:self.trimBarView];
    self.monitorSplitView = [[CutmachineSplitView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, editorWidth, monitorPaneHeight)];
    self.monitorSplitView.vertical = YES;
    self.monitorSplitView.dividerStyle = NSSplitViewDividerStyleThin;
    self.monitorSplitView.delegate = self;
    // The two viewers are one comparison surface: while Source is visible,
    // both panes always receive exactly half of the available width.
    self.monitorSplitView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceMonitorPanel =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, editorWidth * 0.5,
                                                 monitorPaneHeight)];
    self.programMonitorPanel = [[NSView alloc]
        initWithFrame:NSMakeRect(editorWidth * 0.5, 0.0, editorWidth * 0.5,
                                 monitorPaneHeight)];
    for (NSView* panel in
         @[ self.sourceMonitorPanel, self.programMonitorPanel ]) {
        panel.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        panel.wantsLayer = YES;
        panel.layer.backgroundColor = NSColor.blackColor.CGColor;
    }
    self.sourceMonitorView = [[TimelineMetalView alloc]
        initWithFrame:self.sourceMonitorPanel.bounds];
    self.sourceMonitorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.sourceMonitorView.eventTarget = self;
    self.sourceMonitorView.resizeTarget = self;
    [self.sourceMonitorPanel addSubview:self.sourceMonitorView];
    self.programMonitorView = [[TimelineMetalView alloc]
        initWithFrame:self.programMonitorPanel.bounds];
    self.programMonitorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.programMonitorView.eventTarget = self;
    self.programMonitorView.resizeTarget = self;
    [self.programMonitorPanel addSubview:self.programMonitorView];
    // PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. The three Metal surfaces
    // were silent to VoiceOver. Each is a group named as the window names it
    // -- a group, so that the controls laid over a monitor stay inside it,
    // within reach -- and the timeline has a value, asked each time VoiceOver
    // reads it (UiAccessibility.h).
    const auto nameForVoiceOver = [](NSView* view, const char* label) {
        view.accessibilityElement = YES;
        view.accessibilityRole = NSAccessibilityGroupRole;
        view.accessibilityLabel = CMStringFromUtf8(label);
    };
    nameForVoiceOver(self.metalView, ui::accessibility::kTimelineLabel);
    nameForVoiceOver(self.sourceMonitorView,
                     ui::accessibility::kSourceMonitorLabel);
    nameForVoiceOver(self.programMonitorView,
                     ui::accessibility::kRecordMonitorLabel);
    __weak AppDelegate* valueOwner = self;
    self.metalView.accessibilityValueProvider = ^NSString* {
      return [valueOwner timelineAccessibilityValue];
    };
    // REM5 -- the motion selection, drawn over Record, taking no event.
    self.motionCanvasView = [[CMMotionCanvasView alloc]
        initWithFrame:self.programMonitorView.bounds];
    self.motionCanvasView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.motionCanvasView.hidden = YES;
    [self.programMonitorView addSubview:self.motionCanvasView];
    [self.monitorSplitView addArrangedSubview:self.sourceMonitorPanel];
    [self.monitorSplitView addArrangedSubview:self.programMonitorPanel];
    [self.monitorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                            forSubviewAtIndex:0];
    [self.monitorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                            forSubviewAtIndex:1];
    // -adjustSubviews before every -setPosition:ofDividerAtIndex: below, and
    // never the other way round. Freshly added arranged subviews all still
    // sit at x/y 0 -- the split view has not placed them relative to each
    // other yet -- and setting a divider position against overlapping frames
    // makes AppKit resolve the conflict by collapsing panes to zero width.
    // -adjustSubviews lays them out first, so each -setPosition: is then a
    // small nudge from a valid arrangement rather than a guess from garbage.
    [self.monitorSplitView adjustSubviews];
    [self.monitorSplitView setPosition:editorWidth * 0.5 ofDividerAtIndex:0];
    // Monitors above, timeline below -- see the flipped-coordinates note next
    // to editorSplitView's autosaveName. Divider 0 is then the boundary the
    // -constrain{Min,Max}Coordinate: pair below already describes: at least
    // 180 pt of monitors above it, at least 200 pt of timeline under it.
    [self.editorSplitView insertArrangedSubview:self.monitorSplitView
                                        atIndex:0];
    [self.editorSplitView insertArrangedSubview:self.metalView atIndex:1];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:0];
    [self.editorSplitView setHoldingPriority:NSLayoutPriorityDefaultLow
                           forSubviewAtIndex:1];
    [self.editorSplitView adjustSubviews];
    [self.editorSplitView setPosition:monitorPaneHeight ofDividerAtIndex:0];
    [self.workspaceSplitView addArrangedSubview:self.editorSplitView];

    // F2.1 design system: fixed right dock, tabs = FixedPanelLayout()'s
    // PanelDock::Right slots -- the Inspector alone since ON3 took the Agent
    // into its own window (-buildAgentWindow), so the host draws no tab
    // strip. F2.2 calls -setContentView:forSlot: once at startup to replace
    // its placeholder with real content; nothing else about this dock's
    // shape changes at runtime.
    self.rightDockPanel = [[CMPanelHostView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)
                 dock:ui::PanelDock::Right];
    self.rightDockPanel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.workspaceSplitView addArrangedSubview:self.rightDockPanel];

    // F2.2 -- Inspector: clip properties + the eight F1.3 color.* grading
    // knobs (ColorEffects.h), sliders built on CMControlRowView. Every
    // slider edit becomes a SetClipEffectsOperation, delivered here via
    // -inspectorView:didCommitClipEffects: (below) and applied through
    // self.state->editLog -- the same EditLog::Apply path CLI/MCP already
    // use for this operation (PHILOSOPHY.md principle 3, "aucune surface
    // n'est privilégiée"). See InspectorView.h/.mm for the view itself.
    self.inspectorView = [[CMInspectorView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    self.inspectorView.delegate = self;
    self.inspectorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.rightDockPanel setContentView:self.inspectorView
                                forSlot:ui::PanelSlot::Inspector];
    // REM5 -- swapped in while a motion node is selected on Record.
    self.motionInspectorView = [[CMMotionInspectorView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    self.motionInspectorView.delegate = self;
    self.motionInspectorView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;

    // F2.4 -- ROADMAP.md: chat panel, wired to the exact same in-memory
    // Document/EditLog `self.state->editLog.Apply(self.state->document, ...)`
    // already mutates everywhere else in this file, through
    // McpLiveBackend -> McpToolRegistry::Call -- the identical dispatcher
    // McpServer.cc's `tools/call` uses for an external MCP client. No
    // second edit path: see McpLiveBackend.h/ChatSession.h for why. `state`
    // is allocated once, above, and never reallocated after the window is
    // built (only its fields are mutated in place by ordinary edits and
    // project loads), so the reference this backend holds stays valid for
    // the window's lifetime.
    __weak AppDelegate* weakSelf = self;
    self.chatBackend = new McpLiveBackend(
        self.state->document, self.state->editLog,
        [weakSelf]() {
            // Called once the edit is committed (SetEditCommitCallback
            // below) and the window's document and journal hold it.
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) return;
            const RationalTime playhead = strongSelf.state->requestedPosition;
            // PL1 -- ROADMAP.md. The agent's last step is a timeline
            // step: Cmd+Z must reach it, not a project step taken
            // before the turn (a rename, a timeline created earlier).
            strongSelf.state->lastHistoryDomain = HistoryDomain::Timeline;
            [strongSelf refreshTimelineAfterEditFromPosition:playhead];
            [strongSelf rebuildMediaList];
        },
        [weakSelf](ProjectOperation operation, std::vector<EntryMotif> motifs,
                   std::string& resultJson, std::string& errorName,
                   std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            if (![strongSelf persistEdits:message]) {
                errorName = "IoError";
                return false;
            }
            Project candidate = strongSelf.state->project;
            ProjectEditLog projectLog = strongSelf.state->projectEditLog;
            EditError editError = EditError::None;
            // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. The agent's motif
            // rides on the project journal's entry.
            if (!projectLog.Apply(candidate, std::move(operation), editError,
                                  message, std::move(motifs))) {
                errorName = EditErrorName(editError);
                return false;
            }
            const Ulid createdId = candidate.active_timeline_id;
            if (![strongSelf commitProjectCandidate:candidate
                                            editLog:strongSelf.state->editLog
                                         projectLog:projectLog
                                            message:message]) {
                errorName = "IoError";
                return false;
            }
            strongSelf.state->project = std::move(candidate);
            strongSelf.state->projectEditLog = std::move(projectLog);
            strongSelf.state->lastHistoryDomain = HistoryDomain::Project;
            // AX1 -- docs/roadmap/AX-2026-09.md. A project edit may change
            // the timeline on screen (rename_timeline on the active one):
            // the window's document is rebuilt from the project, as the
            // Bin panel's own rename does. It was not, and the next
            // timeline edit wrote the old name back over the rename.
            if (strongSelf.state->project.FindTimeline(
                    strongSelf.state->activeTimelineId) != nullptr) {
                strongSelf.state->document =
                    strongSelf.state->project.MakeDocument(
                        strongSelf.state->activeTimelineId);
                strongSelf.programMonitorTitleLabel.stringValue =
                    CMRecordTitle(strongSelf.state->document.sequence.name);
                strongSelf.window.title =
                    CMWindowTitle(strongSelf.state->project.name,
                                  strongSelf.state->document.sequence.name);
            }
            [strongSelf rebuildMediaList];
            if (![strongSelf
                    activateTimelineIdentifier:
                        [NSString stringWithUTF8String:createdId.c_str()]]) {
                errorName = "UnknownSequence";
                message = "the created timeline could not be activated";
                return false;
            }
            resultJson = "{\"ok\":true,\"timeline_id\":\"" + createdId + "\"}";
            return true;
        },
        [weakSelf](std::string& json, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            // QA pass of 8 October 2026 -- the timeline the window shows,
            // which every edit of this backend changes (each is committed to
            // the project before the call returns). The project's own active
            // timeline is the one it was opened on: once another was chosen
            // in the window, an agent described the first and edited the
            // second.
            Project shown = strongSelf.state->project;
            shown.active_timeline_id = strongSelf.state->activeTimelineId;
            json = DescribeProject(shown);
            return true;
        },
        [weakSelf](std::string& json, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return DescribeTimelineTranscriptForAgent(
                strongSelf.state->document,
                projectPath.parent_path() / ".cutmachine" / "transcripts", json,
                message);
        },
        [weakSelf](const Ulid& sourceId, Transcript& transcript,
                   std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return LoadSourceWords(projectPath.parent_path() / ".cutmachine",
                                   sourceId, transcript, message);
        },
        [weakSelf](const Ulid& sourceId, ShotQualityReport& report) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                return ShotQualityCacheRead{
                    ShotQualityCacheState::Absent, 0,
                    "the application window is no longer available"};
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return ReadShotQualityCache(
                (projectPath.parent_path() / ".cutmachine" / "shotquality" /
                 (sourceId + ".json"))
                    .string(),
                report);
        },
        [weakSelf](const Ulid& sourceId, SpeechOnsetReport& report,
                   std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path projectPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return LoadSpeechOnset((projectPath.parent_path() / ".cutmachine" /
                                    "speechonset" / (sourceId + ".json"))
                                       .string(),
                                   report, message);
        },
        [weakSelf](const Ulid& timelineId, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (!strongSelf) {
                message = "the application window is no longer available";
                return false;
            }
            if (![strongSelf
                    activateTimelineIdentifier:
                        [NSString stringWithUTF8String:timelineId.c_str()]]) {
                message = "unknown timeline_id '" + timelineId + "'";
                return false;
            }
            return true;
        },
        [[CMUiPreferences sharedPreferences]
            requiresExplicitTimelineForAgentEdits]);
    self.chatPanelView = [[CMChatPanelView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, rightDockWidth, workspaceHeight)];
    self.chatBackend->SetProjectPath(self.documentPath.UTF8String ?: "");
    // SAVING_ROADMAP.md, a write refused under the editor (S3) -- 24 October
    // 2026. An edit of the Agent panel or of an agent relayed to the window
    // is committed as a mouse edit is (-applyAndPersistTimelineOperation:):
    // the staged document and journal first, the window's own only once
    // they are saved. A refused write leaves the window as it was, says so
    // under the timeline, and the caller is refused by name; it used to be
    // answered `ok` over an edit the window had just undone.
    self.chatBackend->SetEditCommitCallback([weakSelf](const Document& staged,
                                                       const EditLog& stagedLog,
                                                       std::string& message) {
        AppDelegate* strongSelf = weakSelf;
        if (strongSelf == nullptr || strongSelf.state == nullptr) {
            message = "the application window is no longer available";
            return false;
        }
        if (![strongSelf persistStagedDocumentNow:staged
                                          editLog:stagedLog
                                          message:message]) {
            std::fprintf(stderr, "Unable to persist an agent's edit: %s\n",
                         message.c_str());
            strongSelf.infoLabel.stringValue = [NSString
                stringWithFormat:@"Modification de l'agent refusée, rien n'a "
                                 @"changé : %@",
                                 CMStringFromUtf8(message)];
            return false;
        }
        return true;
    });
    // CHAT-PARITE -- ROADMAP.md F2.4 and P7. A transcription asked for from
    // the chat panel runs on the task manager the Media menu's uses: the
    // task bar shows its progress and cancels it, and quitting stops it. The
    // chat worker waits for it; the main thread never does. `state` and its
    // manager outlive the chat panel (see above). CHAT-ANALYSES -- so do the
    // shot quality, speech onset and beat analyses the chat asks for.
    MediaTaskManager* editorTasks = self.state->mediaTasks.get();
    // Visual QA of 19 September 2026 -- the tasks the chat started, so its
    // Stop button cancels them and nothing else the editor is running.
    struct ChatTasks {
        std::mutex mutex;
        std::set<Ulid> running;
    };
    const auto chatTasks = std::make_shared<ChatTasks>();
    self.chatBackend->SetMediaTaskRunner(
        [editorTasks, chatTasks](MediaTaskKind kind, std::string label,
                                 MediaTaskManager::Work work) {
            Ulid started;
            const MediaTaskSnapshot finished = RunMediaTaskAndWait(
                *editorTasks, kind, std::move(label), std::move(work),
                kMediaTaskWaitLimitMilliseconds, [&](const Ulid& id) {
                    started = id;
                    std::lock_guard<std::mutex> lock(chatTasks->mutex);
                    chatTasks->running.insert(id);
                });
            std::lock_guard<std::mutex> lock(chatTasks->mutex);
            chatTasks->running.erase(started);
            return finished;
        });
    // F2.4 -- ROADMAP.md. ingest_media, asked by the Agent panel or by an
    // agent relayed to the window, is read on that runner and ends in the
    // window's own import, as one from the File menu: the rushes reach the
    // browser with their thumbnails and waveforms, and the line under it
    // says what came in. No target bin, as for --ingest, unless the agent
    // names one (DEC32, PRD7).
    self.chatBackend->SetIngestCommitCallback(
        [weakSelf](const std::vector<ProbedIngestFile>& files,
                   const std::optional<Ulid>& targetBin, IngestOutcome& outcome,
                   std::string& errorName, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (strongSelf == nullptr || strongSelf.state == nullptr) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            const std::filesystem::path documentPath =
                std::filesystem::absolute(std::filesystem::path(
                    strongSelf.documentPath.UTF8String ?: ""));
            return static_cast<bool>([strongSelf commitProbedMedia:files
                                                      documentPath:documentPath
                                                         targetBin:targetBin
                                                           outcome:outcome
                                                         errorName:errorName
                                                           message:message]);
        });
    self.chatBackend->SetProjectSnapshotCallback(
        [weakSelf](Project& project, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (strongSelf == nullptr) {
                message = "the application window is no longer available";
                return false;
            }
            project = strongSelf.state->project;
            return project.CommitDocument(strongSelf.state->activeTimelineId,
                                          strongSelf.state->document, message);
        });
    // OR2 -- publish the engine's returned state in the window only after
    // its edit, journals and receipt have committed together.
    self.chatBackend->SetRunStepCommitCallback(
        [weakSelf](const RunStepRequest& request, std::string& result,
                   std::string& errorName, std::string& message) {
            AppDelegate* editor = weakSelf;
            if (editor == nil || editor.state == nullptr ||
                editor.documentPath.length == 0) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            Project observed;
            if (!editor.chatBackend->SnapshotProject(observed, message)) {
                errorName = "IoError";
                return false;
            }
            Project committed;
            EditLog log;
            const auto storedLog =
                editor.state->timelineEditLogs.find(request.timeline_id);
            const EditLog* liveLog =
                request.timeline_id == editor.state->activeTimelineId
                    ? &editor.state->editLog
                    : (storedLog == editor.state->timelineEditLogs.end()
                           ? nullptr
                           : &storedLog->second);
            if (!CommitStoredRunStep(editor.documentPath.UTF8String, request,
                                     committed, log, result, errorName, message,
                                     &observed, liveLog,
                                     &editor.state->projectEditLog)) {
                return false;
            }
            const bool addedEntry =
                liveLog != nullptr &&
                log.AppliedCount() > liveLog->AppliedCount();
            editor.state->project = std::move(committed);
            editor.state->timelineEditLogs[request.timeline_id] =
                std::move(log);
            editor.state->document = editor.state->project.MakeDocument(
                editor.state->activeTimelineId);
            if (request.timeline_id == editor.state->activeTimelineId) {
                editor.state->editLog =
                    editor.state->timelineEditLogs.at(request.timeline_id);
                if (addedEntry) {
                    editor.state->lastHistoryDomain = HistoryDomain::Timeline;
                }
            }
            [editor refreshTimelineAfterEditFromPosition:editor.state->
                                                         requestedPosition];
            [editor rebuildMediaList];
            [editor reportPendingProjectArchive];
            return true;
        });
    // OR2 -- a stored receipt also has to match the unsaved window state.
    self.chatBackend->SetRunStepMatchCallback(
        [weakSelf](const RunStepReceipt& receipt) {
            AppDelegate* editor = weakSelf;
            if (editor == nil || editor.state == nullptr) {
                return false;
            }
            Project observed;
            std::string message;
            if (!editor.chatBackend->SnapshotProject(observed, message)) {
                return false;
            }
            const auto stored =
                editor.state->timelineEditLogs.find(receipt.timeline_id);
            const EditLog empty;
            const EditLog& log =
                receipt.timeline_id == editor.state->activeTimelineId
                    ? editor.state->editLog
                    : (stored == editor.state->timelineEditLogs.end()
                           ? empty
                           : stored->second);
            return RunStepMatchesState(receipt, observed, log,
                                       editor.state->projectEditLog);
        });
    // AX5 -- docs/roadmap/AX-2026-09.md. What this window shows, for an
    // agent asked about "this clip" or "here" (get_editor_view).
    self.chatBackend->SetEditorViewCallback(
        [weakSelf](EditorView& view, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            if (strongSelf == nullptr || strongSelf.state == nullptr) {
                message = "the application window is no longer available";
                return false;
            }
            view.timeline_id = strongSelf.state->activeTimelineId;
            view.playhead = strongSelf.state->requestedPosition;
            if (strongSelf.state->interaction)
                view.selected_clip_ids =
                    strongSelf.state->interaction->SelectedClipIds();
            // D5 -- docs/roadmap/DIALOGUE-2026-10.md.
            view.mark_in = strongSelf.state->timelineIn;
            view.mark_out = strongSelf.state->timelineOut;
            return true;
        });
    self.chatBackend->SetVideoOutputCallback(
        [weakSelf](const std::optional<std::string>& screen,
                   VideoOutputState& state, std::string& errorName,
                   std::string& message) {
            AppDelegate* editor = weakSelf;
            if (!editor || !editor.state) {
                errorName = "IoError";
                message = "the application window is no longer available";
                return false;
            }
            return static_cast<bool>([editor accessVideoOutput:screen
                                                         state:state
                                                     errorName:errorName
                                                       message:message]);
        });
    // VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. And the window itself,
    // as pixels (capture_editor_window).
    self.chatBackend->SetEditorCaptureCallback(
        [weakSelf](const EditorCaptureRequest& request, EditorCapture& capture,
                   std::string& errorName, std::string& message) {
            AppDelegate* strongSelf = weakSelf;
            errorName = "IoError";
            if (strongSelf == nullptr || strongSelf.state == nullptr) {
                message = "the application window is no longer available";
                return false;
            }
            NSBitmapImageRep* drawn =
                [strongSelf drawEditorWindowOffscreen:message];
            if (drawn == nil) return false;
            NSWindow* window = strongSelf.window;
            NSView* frameView =
                window.contentView.superview ?: window.contentView;
            const CGFloat backing =
                static_cast<CGFloat>(drawn.pixelsWide) /
                std::max<CGFloat>(1.0, frameView.bounds.size.width);
            const std::pair<const char*, NSView*> surfaces[] = {
                {"timeline", strongSelf.metalView},
                {"record", strongSelf.programMonitorView},
                {"source", strongSelf.sourceMonitorView},
            };
            const auto placed = [&](NSView* view, CGFloat pixelsPerPoint) {
                return view.window == window
                           ? WindowCaptureRect(view, pixelsPerPoint)
                           : CGRectNull;
            };
            CGRect crop = CGRectMake(0, 0, drawn.pixelsWide, drawn.pixelsHigh);
            if (request.region != "window") {
                for (const auto& [name, view] : surfaces)
                    if (request.region == name) crop = placed(view, backing);
                if (CGRectIsNull(crop) || CGRectIsEmpty(crop)) {
                    errorName = "ValidationFailed";
                    message = "the " + request.region +
                              " is not on show in the window";
                    return false;
                }
            }
            const double factor = request.scale / backing;
            const auto width = static_cast<size_t>(
                std::max(1.0, std::round(crop.size.width * factor)));
            const auto height = static_cast<size_t>(
                std::max(1.0, std::round(crop.size.height * factor)));
            CGImageRef image =
                WindowCaptureSrgb(drawn.CGImage, crop, width, height);
            capture.jpeg = EncodeWindowCapture(image, true);
            CGImageRelease(image);
            if (capture.jpeg.empty()) {
                message = "the window was drawn but could not be encoded";
                return false;
            }
            capture.width = static_cast<int64_t>(width);
            capture.height = static_cast<int64_t>(height);
            if (request.region != "window") {
                capture.regions.push_back(
                    {request.region, 0, 0, capture.width, capture.height});
                return true;
            }
            for (const auto& [name, view] : surfaces) {
                const CGRect rect = placed(view, request.scale);
                if (CGRectIsNull(rect) || CGRectIsEmpty(rect)) continue;
                capture.regions.push_back(
                    {name, static_cast<int64_t>(rect.origin.x),
                     static_cast<int64_t>(rect.origin.y),
                     static_cast<int64_t>(rect.size.width),
                     static_cast<int64_t>(rect.size.height)});
            }
            return true;
        });
    [self.chatPanelView configureWithBackend:*self.chatBackend];
    [self startAgentEndpoint];
    [self.chatPanelView setStopHandler:^{
      std::lock_guard<std::mutex> lock(chatTasks->mutex);
      for (const Ulid& id : chatTasks->running) editorTasks->Cancel(id);
    }];
    [self buildAgentWindow];

    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:0];
    [self.workspaceSplitView setHoldingPriority:NSLayoutPriorityDefaultHigh
                              forSubviewAtIndex:2];
    [self.workspaceSplitView adjustSubviews];
    [self.workspaceSplitView setPosition:mediaPanelWidth ofDividerAtIndex:0];
    [self.workspaceSplitView setPosition:mediaPanelWidth + editorWidth
                        ofDividerAtIndex:1];

    self.infoLabel = [NSTextField labelWithString:@"Aucun clip sélectionné"];
    self.infoLabel.frame =
        NSMakeRect(20.0, 12.0, windowRect.size.width - 40.0, 18.0);
    self.infoLabel.autoresizingMask = NSViewWidthSizable;
    self.infoLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:11.0
                                         weight:NSFontWeightRegular];
    self.infoLabel.textColor = CMThemeColor(ui::theme::kTextSecondary);
    self.infoLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    // ON1 -- the bottom strip: the status line, the pages, the panels.
    self.workspaceBar = [[CMWorkspaceBarView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, windowRect.size.width, 42.0)];
    self.workspaceBar.autoresizingMask = NSViewWidthSizable;
    self.workspaceBar.delegate = self;
    self.workspaceBar.statusLabel = self.infoLabel;
    [content addSubview:self.workspaceBar];

    [self refreshBinControlsSelecting:@"__root__"];

    self.state->renderer = std::make_unique<Renderer>();
    if (!self.state->renderer->Initialize(self.metalView)) {
        std::fprintf(stderr, "Renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }
    self.state->sourceRenderer = std::make_unique<Renderer>();
    self.state->programRenderer = std::make_unique<Renderer>();
    if (!self.state->sourceRenderer->Initialize(self.sourceMonitorView) ||
        !self.state->programRenderer->Initialize(self.programMonitorView)) {
        std::fprintf(stderr, "Monitor renderer initialization failed\n");
        [NSApp terminate:nil];
        return;
    }
    self.offlineMediaLabel = [NSTextField labelWithString:@"MÉDIA OFFLINE"];
    self.offlineMediaLabel.alignment = NSTextAlignmentCenter;
    self.offlineMediaLabel.font =
        [NSFont systemFontOfSize:24.0 weight:NSFontWeightSemibold];
    self.offlineMediaLabel.textColor = NSColor.secondaryLabelColor;
    self.offlineMediaLabel.hidden = YES;
    [self.programMonitorView addSubview:self.offlineMediaLabel];
    self.offlineMediaLabel.frame =
        NSMakeRect(0.0, 0.0, self.programMonitorView.bounds.size.width,
                   self.programMonitorView.bounds.size.height);
    self.offlineMediaLabel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    self.programCaptionView = [[CMCaptionView alloc] initWithFrame:NSZeroRect];
    self.programCaptionView.hidden = YES;
    [self.programMonitorView addSubview:self.programCaptionView];
    self.sourceOfflineMediaLabel =
        [NSTextField labelWithString:@"MÉDIA OFFLINE"];
    self.sourceOfflineMediaLabel.alignment = NSTextAlignmentCenter;
    self.sourceOfflineMediaLabel.font =
        [NSFont systemFontOfSize:24.0 weight:NSFontWeightSemibold];
    self.sourceOfflineMediaLabel.textColor = NSColor.secondaryLabelColor;
    self.sourceOfflineMediaLabel.hidden = YES;
    self.sourceOfflineMediaLabel.frame = self.sourceMonitorView.bounds;
    self.sourceOfflineMediaLabel.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.sourceMonitorView addSubview:self.sourceOfflineMediaLabel];

    self.sourceMonitorTitleLabel =
        [NSTextField labelWithString:@"SOURCE — aucun rush chargé"];
    self.programMonitorTitleLabel = [NSTextField
        labelWithString:CMRecordTitle(self.state->document.sequence.name)];
    for (NSTextField* label in
         @[ self.sourceMonitorTitleLabel, self.programMonitorTitleLabel ]) {
        TimelineMetalView* monitor = label == self.programMonitorTitleLabel
                                         ? self.programMonitorView
                                         : self.sourceMonitorView;
        label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightBold);
        label.drawsBackground = NO;
        label.frame = NSMakeRect(
            8.0,
            monitor.bounds.size.height - ui::theme::kPanelHeaderHeight - 4.0,
            monitor.bounds.size.width - 16.0, ui::theme::kPanelHeaderHeight);
        label.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    }
    self.sourceMonitorTitleLabel.textColor =
        CMThemeColor(ui::theme::kTextPrimary);
    self.programMonitorTitleLabel.textColor = NSColor.whiteColor;
    [self.sourceMonitorView addSubview:self.sourceMonitorTitleLabel];
    [self.programMonitorView addSubview:self.programMonitorTitleLabel];
    self.sourceMonitorZoneLabel =
        [NSTextField labelWithString:@"IN —    OUT —"];
    self.sourceMonitorZoneLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.sourceMonitorZoneLabel.textColor =
        CMThemeColor(ui::theme::kTextSecondary);
    self.sourceMonitorZoneLabel.frame = NSMakeRect(10.0, 29.0, 300.0, 16.0);
    self.sourceMonitorZoneLabel.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    [self.sourceMonitorView addSubview:self.sourceMonitorZoneLabel];
    NSButton* sourceInsertButton =
        CMMakeStyledButton(@"INSÉRER", self, @selector(menuInsertSource:));
    NSButton* sourceOverwriteButton =
        CMMakeStyledButton(@"ÉCRASER", self, @selector(menuOverwriteSource:));
    sourceInsertButton.frame = NSMakeRect(10.0, 49.0, 78.0, 24.0);
    sourceOverwriteButton.frame = NSMakeRect(92.0, 49.0, 82.0, 24.0);
    sourceInsertButton.image =
        SystemSymbol(@"arrow.down.to.line", @"Insérer depuis Source", 10.0);
    sourceOverwriteButton.image =
        SystemSymbol(@"square.on.square", @"Écraser depuis Source", 10.0);
    for (NSButton* button in @[ sourceInsertButton, sourceOverwriteButton ]) {
        button.font = [NSFont systemFontOfSize:10.0];
        button.imagePosition = NSImageLeading;
        button.autoresizingMask = NSViewMaxYMargin;
        [self.sourceMonitorView addSubview:button];
    }
    self.sourceMonitorZoomPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(
                          self.sourceMonitorView.bounds.size.width - 92.0, 4.0,
                          84.0, 24.0)
            pullsDown:NO];
    self.programMonitorZoomPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(
                          self.programMonitorView.bounds.size.width - 92.0, 4.0,
                          84.0, 24.0)
            pullsDown:NO];
    for (NSPopUpButton* popup in
         @[ self.sourceMonitorZoomPopup, self.programMonitorZoomPopup ]) {
        [popup
            addItemsWithTitles:@[ @"Fit", @"25%", @"50%", @"100%", @"200%" ]];
        const NSInteger zoomTags[] = {0, 25, 50, 100, 200};
        for (NSInteger index = 0; index < popup.numberOfItems; ++index)
            [popup itemAtIndex:index].tag = zoomTags[index];
        popup.target = self;
        popup.action = @selector(monitorZoomChanged:);
        popup.controlSize = NSControlSizeSmall;
        popup.font =
            [NSFont monospacedDigitSystemFontOfSize:10.0
                                             weight:NSFontWeightMedium];
        popup.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
        popup.toolTip = @"Zoom d’affichage du moniteur";
    }
    [self.sourceMonitorView addSubview:self.sourceMonitorZoomPopup];
    [self.programMonitorView addSubview:self.programMonitorZoomPopup];
    self.programVideoScopePopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(8.0, 4.0, 132.0, 24.0)
                                   pullsDown:NO];
    [self.programVideoScopePopup addItemsWithTitles:@[
        @"Aucun scope", @"Waveform", @"Waveform RGB", @"Parade RGB",
        @"Vectorscope", @"Fausses couleurs"
    ]];
    // Tags are the stored VideoScopeMode values; the RGB waveform was
    // appended to the enum but is listed beside the luma one.
    const NSInteger scopeTags[] = {
        static_cast<NSInteger>(VideoScopeMode::Off),
        static_cast<NSInteger>(VideoScopeMode::Waveform),
        static_cast<NSInteger>(VideoScopeMode::WaveformRgb),
        static_cast<NSInteger>(VideoScopeMode::ParadeRgb),
        static_cast<NSInteger>(VideoScopeMode::Vectorscope),
        static_cast<NSInteger>(VideoScopeMode::FalseColor)};
    for (NSInteger index = 0; index < self.programVideoScopePopup.numberOfItems;
         ++index)
        [self.programVideoScopePopup itemAtIndex:index].tag = scopeTags[index];
    [self.programVideoScopePopup
        selectItemWithTag:static_cast<NSInteger>(
                              self.state->programVideoScope)];
    self.programVideoScopePopup.target = self;
    self.programVideoScopePopup.action = @selector(programVideoScopeChanged:);
    self.programVideoScopePopup.controlSize = NSControlSizeSmall;
    self.programVideoScopePopup.font =
        [NSFont monospacedDigitSystemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.programVideoScopePopup.autoresizingMask =
        NSViewMaxXMargin | NSViewMaxYMargin;
    self.programVideoScopePopup.toolTip =
        @"Afficher un scope de la sortie Record après grading";
    [self.programMonitorView addSubview:self.programVideoScopePopup];
    // G6 -- said on the monitor itself: a picture shown without its grade,
    // or half of it a reference, must never pass for the film.
    self.programCompareBadge = [NSTextField labelWithString:@""];
    self.programCompareBadge.frame = NSMakeRect(
        148.0, 8.0,
        std::max(0.0, self.programMonitorView.bounds.size.width - 248.0), 16.0);
    self.programCompareBadge.autoresizingMask =
        NSViewWidthSizable | NSViewMaxYMargin;
    self.programCompareBadge.alignment = NSTextAlignmentCenter;
    self.programCompareBadge.font =
        [NSFont monospacedSystemFontOfSize:ui::theme::kFontSizeCaption
                                    weight:NSFontWeightBold];
    self.programCompareBadge.textColor = CMThemeColor(ui::theme::kAccent);
    self.programCompareBadge.hidden = YES;
    [self.programMonitorView addSubview:self.programCompareBadge];
    [self refreshCompareBadge];
    self.sourceMonitorToggleButton =
        CMMakeStyledButton(@"SOURCE ON", self, @selector(toggleSourceMonitor:));
    self.sourceMonitorToggleButton.state = NSControlStateValueOn;
    self.sourceMonitorToggleButton.frame = NSMakeRect(
        self.programMonitorView.bounds.size.width - 198.0, 4.0, 98.0, 24.0);
    self.sourceMonitorToggleButton.autoresizingMask =
        NSViewMinXMargin | NSViewMaxYMargin;
    self.sourceMonitorToggleButton.font =
        [NSFont systemFontOfSize:10.0 weight:NSFontWeightMedium];
    self.sourceMonitorToggleButton.image = SystemSymbol(
        @"rectangle.split.2x1", @"Afficher le moniteur Source", 11.0);
    self.sourceMonitorToggleButton.imagePosition = NSImageLeading;
    self.sourceMonitorToggleButton.toolTip =
        @"Afficher ou masquer le moniteur Source";
    [self.programMonitorView addSubview:self.sourceMonitorToggleButton];
    // ON7 -- the edit trace stays on Montage while the Agent is hidden.
    self.workspaceActivityScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(0, 0, ui::workspace::kActivityWidth, 400)];
    self.workspaceActivityScroll.hasVerticalScroller = YES;
    self.workspaceActivityScroll.borderType = NSNoBorder;
    self.workspaceActivityText =
        [[NSTextView alloc] initWithFrame:self.workspaceActivityScroll.bounds];
    self.workspaceActivityText.editable = NO;
    self.workspaceActivityText.selectable = YES;
    self.workspaceActivityText.font =
        CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
    self.workspaceActivityText.textColor =
        CMThemeColor(ui::theme::kTextPrimary);
    self.workspaceActivityText.backgroundColor = CMSurfacePanelColor();
    self.workspaceActivityText.textContainerInset =
        NSMakeSize(ui::theme::kSpaceS, ui::theme::kSpaceS);
    self.workspaceActivityText.minSize = NSZeroSize;
    self.workspaceActivityText.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    self.workspaceActivityText.verticallyResizable = YES;
    self.workspaceActivityText.horizontallyResizable = NO;
    self.workspaceActivityText.autoresizingMask = NSViewWidthSizable;
    self.workspaceActivityText.textContainer.widthTracksTextView = YES;
    self.workspaceActivityText.accessibilityLabel = NSLocalizedString(
        @"Marqueurs et modifications récentes du montage", nil);
    self.workspaceActivityScroll.documentView = self.workspaceActivityText;
    self.trackHeaderLabels = [NSMutableArray array];
    self.timelineTimecodeLabel = [NSTextField labelWithString:@"00:00:00:00"];
    self.timelineTimecodeLabel.font =
        [NSFont monospacedDigitSystemFontOfSize:14.0
                                         weight:NSFontWeightSemibold];
    self.timelineTimecodeLabel.textColor = [NSColor colorWithRed:0.82
                                                           green:0.84
                                                            blue:0.87
                                                           alpha:1.0];
    self.timelineTimecodeLabel.alignment = NSTextAlignmentLeft;
    [self.metalView addSubview:self.timelineTimecodeLabel];
    self.timelineToolIcons = [NSMutableArray array];
    [self refreshTimelineChrome];
    [self fitTimelineToViewportWidth];

    __weak AppDelegate* weakShortcutTarget = self;
    self.timelineShortcutMonitor = [NSEvent
        addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                     handler:^NSEvent*(NSEvent* event) {
                                       AppDelegate* target = weakShortcutTarget;
                                       if (!target) return event;
                                       // ON3 -- the Agent's window: its
                                       // field keeps the text keys too.
                                       if ([target agentWindowTakesKey:event])
                                           return nil;
                                       if (event.window != target.window)
                                           return event;
                                       NSResponder* responder =
                                           target.window.firstResponder;
                                       if (![responder
                                               isKindOfClass:NSView.class])
                                           return event;
                                       NSView* focusedView = (NSView*)responder;
                                       // D7 -- a field being typed in
                                       // keeps the text keys.
                                       if ([focusedView
                                               isKindOfClass:NSTextView
                                                                 .class] &&
                                           [target
                                               textEditKeyDown:event
                                                    inTextView:(NSTextView*)
                                                                   focusedView])
                                           return nil;
                                       if (![focusedView
                                               isDescendantOf:
                                                   target.inspectorView] ||
                                           [focusedView
                                               isKindOfClass:NSTextView.class])
                                           return event;
                                       // Preserve native Inspector control
                                       // navigation and adjustment. The
                                       // remaining keys use the same
                                       // configurable timeline dispatcher as
                                       // the Metal view, so this does not
                                       // create a second shortcut policy.
                                       switch (event.keyCode) {
                                           case 48:   // Tab
                                           case 53:   // Escape
                                           case 115:  // Home
                                           case 119:  // End
                                           case 123:  // Left
                                           case 124:  // Right
                                           case 125:  // Down
                                           case 126:  // Up
                                               return event;
                                           default:
                                               break;
                                       }
                                       return [target timelineKeyDown:event]
                                                  ? nil
                                                  : event;
                                     }];

    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self reportPendingProjectArchive];
    // The timeline it opens on, as -activateTimelineIdentifier: does for
    // the others (LinkGroupRepair.h).
    [self splitMergedLinkGroups];
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting)
        gUiSmokeWorkersAtWindowShow = self.state->workers.size();
#endif
    // The window is up; which media are missing is checked behind it.
    [self checkSourceAvailabilityInBackground];
    BOOL loadedThumbnail = NO;
    for (const DocumentSource& source : self.state->document.sources) {
        const auto detected = self.state->mediaMetadata.find(source.id);
        const LibraryMedia* media =
            detected == self.state->mediaMetadata.end()
                ? self.state->document.FindLibraryMedia(source.id)
                : &detected->second;
        if (media && media->has_audio &&
            self.state->offlineSourceIds.count(source.id) == 0)
            [self loadOrEnqueueWaveformForMediaIdentifier:
                      [NSString stringWithUTF8String:source.id.c_str()]];
        if (media && self.state->offlineSourceIds.count(source.id) == 0)
            loadedThumbnail |=
                [self stageThumbnailForMediaIdentifier:
                          [NSString stringWithUTF8String:source.id.c_str()]];
    }
    if (loadedThumbnail) [self.mediaCollection reloadData];
    // ON1 -- the page the editor left the window on, Montage the first time.
    // The smoke run lays the window out as it was before pages, every
    // panel open, and writes nothing into the editor's preferences.
    self.rememberWorkspace = YES;
    bool smokeLayout = false;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    smokeLayout = gUiSmokeTesting;
#endif
    if (smokeLayout) {
        self.rememberWorkspace = NO;
        // 301, not 300: before pages the dock's divider was set at the
        // media panel's width plus the centre's, which left it one point
        // wider and the centre an even 977 points -- two viewers of equal
        // width, which "Source toggle restores two equally sized
        // monitors" holds to the point.
        self.workspaceFixedWidths = {320.0, 301.0};
        self.workspaceWidthsFixed = YES;
        [self applyWorkspacePage:ui::workspace::Page::Edit
                          panels:ui::workspace::PanelSet{true, true, true}
                        remember:NO];
    } else {
        CMUiPreferences* preferences = CMUiPreferences.sharedPreferences;
        const ui::workspace::Page page = [preferences workspacePage];
        [self applyWorkspacePage:page
                          panels:[preferences panelsForPage:page]
                        remember:NO];
        // ON3 -- the Agent's window, if it was left shown, without the
        // keyboard: the editor keeps it.
        if ([preferences agentWindowShown]) [self showAgentWindow:YES focus:NO];
    }
    [self refreshAgentToggle];
    [self requestResolvedPosition:{0, 1}];
    [self startVideoOutput];
    self.displayTimer = [NSTimer timerWithTimeInterval:(1.0 / 60.0)
                                                target:self
                                              selector:@selector(displayTick:)
                                              userInfo:nil
                                               repeats:YES];
    // Splitter dragging runs the AppKit event-tracking loop. Common modes keep
    // Metal presenting correctly letterboxed frames throughout the gesture.
    [NSRunLoop.mainRunLoop addTimer:self.displayTimer
                            forMode:NSRunLoopCommonModes];
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting)
        dispatch_async(dispatch_get_main_queue(), ^{
          [self runUiSmokeTests];
        });
    if (!gUiPlayProbeProject.empty())
        dispatch_async(dispatch_get_main_queue(), ^{
          [self runPlayProbe];
        });
#endif
}

- (void)application:(NSApplication*)application
          openFiles:(NSArray<NSString*>*)filenames {
    (void)application;
    if (filenames.count == 0) return;
    NSString* selected = [self resolvedProjectPath:filenames.firstObject];
    if (!self.window && self.documentPath.length == 0) {
        self.documentPath = selected;
        [NSApp replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
        return;
    }
    [NSApp replyToOpenOrPrint:NSApplicationDelegateReplyCancel];
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"Un projet est déjà ouvert";
    alert.informativeText =
        @"Fermez la fenêtre actuelle avant d’ouvrir un autre projet.";
    if (self.window)
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

// PERF13 -- MonitorOcclusion.h's PictureLookup, answered from the frame
// cache as -presentNearestFrameAtDeadline: will read it: the picture
// GetNearest hands back, turned by the rotation the renderer is given.
// Each source is described once per lookup.
- (monitor_occlusion::PictureLookup)occlusionPictures {
    AppState* state = self.state;
    auto described = std::make_shared<
        std::map<Ulid, std::optional<monitor_occlusion::Picture>>>();
    return [state, described](
               const Ulid& sourceId,
               int64_t frame) -> std::optional<monitor_occlusion::Picture> {
        const auto known = described->find(sourceId);
        if (known != described->end()) return known->second;
        std::optional<monitor_occlusion::Picture> picture;
        int width = 0;
        int height = 0;
        bool alpha = false;
        if (state->frameCache && state->offlineSourceIds.count(sourceId) == 0 &&
            state->frameCache->DescribeNearest(sourceId, frame, width, height,
                                               alpha)) {
            const auto media = state->mediaMetadata.find(sourceId);
            const int32_t degrees = media != state->mediaMetadata.end() &&
                                            media->second.metadata_complete
                                        ? media->second.rotation_degrees
                                        : 0;
            picture = monitor_occlusion::UprightPicture(width, height, degrees,
                                                        alpha);
        }
        described->emplace(sourceId, picture);
        return picture;
    };
}

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The Trim Monitor (Resolve's
// two-up, manual p. 1065-1067): while the trim mode holds points and nothing
// plays, Record shows the last frame of A and the first frame of B side by
// side, each in the pane LayoutTrimMonitor gives it, through the same
// textures and the same compositing as the program. What each side shows,
// its counters and its handle are TrimSession.h's; this asks the frames
// and keeps the panes. NO, and nothing kept, when Record is the program.
// FC4 -- the Trim Monitor's frames and words, on Record's display list:
// rectangles of the theme and text of the atlas, nothing else. Over each
// pane, its side and the frames trimmed since the point was selected (in
// the trim green once they moved); under it, what the trim can still take.
- (void)layOutTrimMonitor:(TimelineRenderData&)programData {
    const std::vector<trim::TrimMonitorPane>& panes =
        self.state->trimMonitorPanes;
    for (size_t slot = 0; slot < panes.size(); ++slot) {
        const trim::TrimMonitorPane& pane = panes[slot];
        programData.video_framings[slot] = MonitorFraming{
            0.0f, 0.0f, 1.0f, 1.0f, pane.x, pane.y, pane.width, pane.height};
        programData.video_opacities[slot] = 1.0f;
        programData.video_veils[slot] = MonitorVeil{};
        // EF3 -- and sharp: the cut is judged on the frames themselves.
        if (slot < programData.video_blurs.size())
            programData.video_blurs[slot] = 0.0f;
        // PA8 -- and whole, for the same reason.
        if (slot < programData.video_masks.size())
            programData.video_masks[slot] = MonitorMask{};
    }
    const int32_t sequenceWidth = self.state->document.sequence.width;
    const int32_t sequenceHeight = self.state->document.sequence.height;
    const NSRect bounds = self.programMonitorView.bounds;
    const double layerScale = self.programMonitorView.layer.contentsScale;
    const double zoomPoints =
        self.state->programMonitorZoom > 0.0 && layerScale > 0.0
            ? self.state->programMonitorZoom / layerScale
            : 0.0;
    const ui::caption_overlay::Layout canvas = ui::caption_overlay::Compute(
        sequenceWidth, sequenceHeight, bounds.size.width, bounds.size.height,
        zoomPoints);
    const trim::TrimMonitorText text =
        trim::DescribeTrimMonitorText(self.state->trimMonitorView);
    const double band = ui::theme::kFontSizeSmall + (2.0 * ui::theme::kSpaceXs);
    const ui::theme::Color shade =
        ui::theme::WithAlpha(ui::theme::kSurfaceSunken, 0.78f);
    // The two halves of the sequence frame, whatever the panes' pictures.
    const double half = canvas.canvas_width / 2.0;
    programData.overlays.emplace_back(
        MetalRect{canvas.canvas_x + half - 0.5, canvas.canvas_y, 1.0,
                  canvas.canvas_height, ui::theme::kBorderStrong,
                  ui::theme::kBorderStrong});
    const auto words = [&](double x, double y, double width,
                           const std::string& line, ui::theme::Color color) {
        if (line.empty() || width <= 0.0) return;
        programData.overlays.emplace_back(
            MetalText{x, y, width, ui::theme::kFontSizeSmall, line, color,
                      MetalFontFace::Mono, true});
    };
    const std::array<const trim::TrimMonitorSide*, 2> sides = {
        &self.state->trimMonitorView.outgoing,
        &self.state->trimMonitorView.incoming};
    const std::array<int64_t, 2> shifts = {
        self.state->trimMonitorView.out_shift_frames,
        self.state->trimMonitorView.in_shift_frames};
    const std::array<trim::TrimMonitorPane, 2> sidePanes = {
        self.state->trimMonitorLayout.outgoing,
        self.state->trimMonitorLayout.incoming};
    for (size_t index = 0; index < 2; ++index) {
        const double x = canvas.canvas_x + (index * half);
        // The words hug the picture, above and under it, within the frame;
        // a side without a picture has them at the frame's edges.
        const trim::TrimMonitorPane& pane = sidePanes[index];
        const double frameBottom = canvas.canvas_y + canvas.canvas_height;
        const double paneTop = pane.empty()
                                   ? canvas.canvas_y
                                   : canvas.canvas_y + (pane.y * canvas.scale);
        const double paneBottom =
            pane.empty() ? frameBottom : paneTop + (pane.height * canvas.scale);
        const double top = std::max(canvas.canvas_y, paneTop - band);
        const double bottom = std::min(frameBottom - band, paneBottom);
        programData.overlays.emplace_back(
            MetalRect{x, top, half, band, shade, shade});
        programData.overlays.emplace_back(
            MetalRect{x, bottom, half, band, shade, shade});
        const double inset = ui::theme::kSpaceS;
        const std::string& title =
            index == 0 ? text.outgoing_title : text.incoming_title;
        const std::string& shift =
            index == 0 ? text.outgoing_shift : text.incoming_shift;
        const std::string& handle =
            index == 0 ? text.outgoing_handle : text.incoming_handle;
        const double titleWidth = std::max(0.0, half * 0.6 - inset);
        words(x + inset, top + ui::theme::kSpaceXs, titleWidth, title,
              ui::theme::kTextPrimary);
        words(x + (half * 0.6), top + ui::theme::kSpaceXs,
              std::max(0.0, (half * 0.4) - inset), shift,
              shifts[index] != 0 ? ui::theme::kTrimPoint
                                 : ui::theme::kTextSecondary);
        if (sides[index]->present)
            words(x + inset, bottom + ui::theme::kSpaceXs,
                  std::max(0.0, half - (2.0 * inset)), handle,
                  ui::theme::kTextSecondary);
    }
}

- (BOOL)resolveTrimMonitorSlots {
    self.state->trimMonitorPanes.clear();
    self.state->trimMonitorView = {};
    self.state->trimMonitorLayout = {};
    if (!self.state->trimSession.Active() || self.state->playbackDirection != 0)
        return NO;
    const Document& shown = [self trimShownDocument];
    const trim::TrimMonitorView view =
        trim::DescribeTrimMonitor(shown, self.state->trimSession);
    if (!view.active) return NO;
    const int32_t sequenceWidth = shown.sequence.width;
    const int32_t sequenceHeight = shown.sequence.height;
    const std::array<const trim::TrimMonitorSide*, 2> sides = {&view.outgoing,
                                                               &view.incoming};
    std::array<const DocumentClip*, 2> clips = {nullptr, nullptr};
    std::array<std::pair<int64_t, int64_t>, 2> sizes = {};
    for (size_t index = 0; index < sides.size(); ++index) {
        const trim::TrimMonitorSide& side = *sides[index];
        if (!side.present || !side.has_source || side.source_frame < 0)
            continue;
        const DocumentClip* clip = shown.FindClip(side.clip_id);
        // A motion clip is drawn, not decoded, from its composition's own
        // frames: the monitor leaves its pane empty rather than guess.
        if (clip == nullptr || IsMotionClip(*clip)) continue;
        int64_t width = 0;
        int64_t height = 0;
        std::string error;
        // A rush never probed has no known size: fitted as the sequence.
        if (!ClipDisplayedSize(shown, *clip, width, height, error)) {
            width = sequenceWidth;
            height = sequenceHeight;
        }
        clips[index] = clip;
        sizes[index] = {width, height};
    }
    const trim::TrimMonitorLayout layout = trim::LayoutTrimMonitor(
        sequenceWidth, sequenceHeight, sizes[0].first, sizes[0].second,
        sizes[1].first, sizes[1].second);
    const std::array<trim::TrimMonitorPane, 2> panes = {layout.outgoing,
                                                        layout.incoming};
    for (size_t index = 0; index < sides.size(); ++index) {
        if (clips[index] == nullptr || panes[index].empty()) continue;
        const DocumentClip& clip = *clips[index];
        self.state->requested.push_back(
            {true, clip.source_id, sides[index]->source_frame, 1.0f, clip.id});
        self.state->trimMonitorPanes.push_back(panes[index]);
        self.state->readSourceIds.insert(clip.source_id);
        [self requestFrame:sides[index]->source_frame ofSource:clip.source_id];
    }
    self.state->trimMonitorView = view;
    self.state->trimMonitorLayout = layout;
    self.state->overlayDirty = true;
    return YES;
}

- (void)requestResolvedPosition:(RationalTime)position {
    self.state->requestedPosition = position;
    if (self.state->playbackReader.Running())
        self.state->playbackReader.SetPosition(position);
    self.timelineTimecodeLabel.stringValue =
        TimelineTimecode(position, [self playheadFrameRate]);
    self.state->requested.clear();
    self.state->hiddenSlots.clear();
    self.state->readSourceIds.clear();
    // FC4 -- in trim mode, at rest, Record is the Trim Monitor.
    if ([self resolveTrimMonitorSlots]) {
        self.offlineMediaLabel.hidden = YES;
        return;
    }
    // PERF13 -- MonitorOcclusion.h. In playback, a track an opaque shot
    // above hides here is neither composited nor decoded: under a cutaway,
    // the interview cost the external drive a second 4K stream nobody saw.
    // At rest everything is decoded as before -- a few frames, and a step
    // across the cutaway's end finds the interview's frame ready.
    self.state->hiddenTrackIds.clear();
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    const bool hide = gUiPlayProbeHide;
#else
    const bool hide = true;
#endif
    if (self.state->playbackDirection != 0 && hide)
        self.state->hiddenTrackIds = monitor_occlusion::HiddenTracks(
            self.state->document, *self.state->timeline, position,
            [self occlusionPictures], self.state->programMonitorZoom > 0.0);
    for (const Ulid& trackId : self.state->videoTrackIds) {
        const bool trackHidden = self.state->hiddenTrackIds.count(trackId) != 0;
        for (const ResolvedLayer& layer :
             self.state->timeline->ResolveTrackLayers(trackId, position)) {
            const ResolvedSlot slot{true, layer.frame.source_id,
                                    layer.frame.source_frame, layer.opacity,
                                    layer.frame.clip_id};
            // A motion layer costs no decoder, and the motion canvas picks
            // what it edits among the layers composited: it stays.
            const DocumentClip* clip =
                trackHidden ? self.state->document.FindClip(layer.frame.clip_id)
                            : nullptr;
            if (clip != nullptr && !IsMotionClip(*clip)) {
                self.state->hiddenSlots.push_back(slot);
                continue;
            }
            self.state->requested.push_back(slot);
            self.state->readSourceIds.insert(layer.frame.source_id);
            [self requestFrame:layer.frame.source_frame
                      ofSource:layer.frame.source_id
                          plan:[self readPlanForClip:layer.frame.clip_id
                                             onTrack:trackId
                                                  at:position
                                            onScreen:YES]];
        }
    }
    const auto topmost = std::find_if(
        self.state->requested.rbegin(), self.state->requested.rend(),
        [](const ResolvedSlot& slot) { return slot.active; });
    self.offlineMediaLabel.hidden =
        topmost == self.state->requested.rend() ||
        self.state->offlineSourceIds.count(topmost->sourceId) == 0;
    // REM5 -- a selection follows the frame shown.
    if (!self.state->motionCanvas.selected.empty()) {
        [self motionCanvasRefresh];
    }
    // R2c -- and the Inspector's diamonds the playhead's frame.
    [self.inspectorView showPlayheadAt:position];
    [self warmUpcomingDecoders:position];
}

// PERF-2026-09. Playback crosses cuts into sources whose decoder has never
// been asked for anything, and the first request arrives one tick before the
// frame is due -- far too late for a seek and a decode. The layer then has no
// frame at all, and until PlaybackPresentation.h's hold the compositor
// uncovered the track underneath for those ticks. Ask the incoming decoder
// while the outgoing clip is still on screen instead, and only for sources no
// visible layer is using: a worker holds one target frame, so warming a
// source that is being shown right now would steal it from the current image.
- (void)warmUpcomingDecoders:(RationalTime)position {
    self.state->upcomingSourceIds.clear();
    if (self.state->playbackDirection != 0 && self.state->timeline) {
        const int speed = std::abs(self.state->playbackDirection);
        // One second of timeline at nominal speed, and proportionally
        // further ahead when playing faster -- the decoder gets the same
        // wall-clock warning either way.
        int64_t cutSeconds = read_ahead::kCutLookaheadSeconds;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        cutSeconds = gUiPlayProbeReadAhead ? gUiPlayProbeCutSeconds : 1;
#endif
        const RationalTime lookahead{speed * cutSeconds, 1};
        const auto onScreen = [&](const Ulid& sourceId) {
            return std::any_of(
                self.state->requested.begin(), self.state->requested.end(),
                [&](const ResolvedSlot& slot) {
                    return slot.active && slot.sourceId == sourceId;
                });
        };
        const monitor_occlusion::PictureLookup pictures =
            [self occlusionPictures];
        const bool zoomed = self.state->programMonitorZoom > 0.0;
        for (const ResolvedFrame& upcoming :
             self.state->timeline->ResolveUpcoming(
                 position, self.state->playbackDirection, lookahead)) {
            if (onScreen(upcoming.source_id)) continue;
            // PERF13 -- a cut on a track hidden now that is still hidden
            // when playback reaches it shows nothing: the frame the track
            // is shown again at is asked for below instead.
            if (self.state->playbackDirection > 0 &&
                !self.state->hiddenTrackIds.empty()) {
                const DocumentTrack* track =
                    self.state->document.FindTrackForClip(upcoming.clip_id);
                const DocumentClip* clip =
                    self.state->document.FindClip(upcoming.clip_id);
                if (track != nullptr && clip != nullptr &&
                    self.state->hiddenTrackIds.count(track->id) != 0 &&
                    monitor_occlusion::HiddenTracks(
                        self.state->document, *self.state->timeline,
                        clip->timeline_in, pictures, zoomed)
                            .count(track->id) != 0)
                    continue;
            }
            // PERF10 -- a source not open yet is opened now, a second
            // ahead. PERF12 -- the last resort: openDecodersAhead has
            // usually opened it long before.
            self.state->upcomingSourceIds.insert(upcoming.source_id);
            self.state->readSourceIds.insert(upcoming.source_id);
            DecodeWorker::ReadPlan plan;
            if (const DocumentClip* entered =
                    self.state->document.FindClip(upcoming.clip_id)) {
                const DocumentTrack* track =
                    self.state->document.FindTrackForClip(upcoming.clip_id);
                const RationalTime entry =
                    self.state->playbackDirection > 0
                        ? entered->timeline_in
                        : entered->timeline_in.add(entered->duration)
                              .sub(RationalTime{
                                  self.state->document.sequence.frame_rate.den,
                                  self.state->document.sequence.frame_rate
                                      .num});
                if (track != nullptr)
                    plan = [self readPlanForClip:upcoming.clip_id
                                         onTrack:track->id
                                              at:entry
                                        onScreen:NO];
            }
            [self requestFrame:upcoming.source_frame
                      ofSource:upcoming.source_id
                          plan:plan];
        }
        // PERF13 -- a hidden track shown again within the same window: its
        // frame there is asked for now, as a cut's is. Asked only once
        // shown, it would be a whole seek and decode late, and the monitor
        // would show the picture its decoder last held, from before the
        // cutaway, meanwhile. PERF14 -- and earlier than a cut, since the
        // cutaway it comes back from reads the same disk until its end.
        int64_t reappearSeconds = read_ahead::kReappearanceLookaheadSeconds;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        if (!gUiPlayProbeReadAhead)
            reappearSeconds = 1;
        else
            reappearSeconds = gUiPlayProbeReappearSeconds;
#endif
        const RationalTime reappearance{speed * reappearSeconds, 1};
        for (const auto& [trackId, shownAt] : monitor_occlusion::NextShown(
                 self.state->document, *self.state->timeline, position,
                 self.state->playbackDirection, reappearance,
                 self.state->hiddenTrackIds, pictures, zoomed)) {
            for (const ResolvedLayer& layer :
                 self.state->timeline->ResolveTrackLayers(trackId, shownAt)) {
                const DocumentClip* clip =
                    self.state->document.FindClip(layer.frame.clip_id);
                if (clip == nullptr || IsMotionClip(*clip) ||
                    onScreen(layer.frame.source_id))
                    continue;
                self.state->upcomingSourceIds.insert(layer.frame.source_id);
                self.state->readSourceIds.insert(layer.frame.source_id);
                [self requestFrame:layer.frame.source_frame
                          ofSource:layer.frame.source_id
                              plan:[self readPlanForClip:layer.frame.clip_id
                                                 onTrack:trackId
                                                      at:shownAt
                                                onScreen:NO]];
            }
        }
    }
    // PERF14 -- the window of decoded pictures is shared among the sources
    // read, in playback; at rest, among the decoders open, as before.
    if (self.state->frameCache) {
        bool share = self.state->playbackDirection != 0;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        share = share && gUiPlayProbeReadAhead;
#endif
        self.state->frameCache->SetReadSources(share ? self.state->readSourceIds
                                                     : std::set<Ulid>{});
    }
    [self openDecodersAhead:position];
}

- (void)requestSourcePosition:(RationalTime)position {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source) return;
    if (position < RationalTime{0, source->duration.rate})
        position = {0, source->duration.rate};
    if (position > source->duration) position = source->duration;
    const int64_t totalFrames =
        source->duration.to_frames(source->rate.num, source->rate.den);
    const int64_t frame = std::clamp<int64_t>(
        position.to_frames(source->rate.num, source->rate.den), 0,
        std::max<int64_t>(0, totalFrames - 1));
    position = {frame * static_cast<int64_t>(source->rate.den),
                source->rate.num};
    if (position > source->duration) position = source->duration;
    self.state->sourceMonitorPosition = position;
    [self requestFrame:frame ofSource:source->id];
    self.state->overlayDirty = true;
}

- (void)updateSourceZoneLabel {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    const MediaRate rate = source ? source->rate : [self playheadFrameRate];
    NSString* inText = self.state->sourceIn
                           ? TimelineTimecode(*self.state->sourceIn, rate)
                           : @"—";
    NSString* outText = self.state->sourceOut
                            ? TimelineTimecode(*self.state->sourceOut, rate)
                            : @"—";
    self.sourceMonitorZoneLabel.stringValue =
        [NSString stringWithFormat:@"IN %@    OUT %@", inText, outText];
}

- (void)scrubSourceMonitorEvent:(NSEvent*)event {
    // Out of lot, 29 October 2026 -- a scrub takes the playhead from the
    // player.
    if (self.state->sourcePlaybackDirection != 0)
        [self setSourcePlaybackDirection:0];
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source || self.sourceMonitorView.bounds.size.width <= 0.0) return;
    const NSPoint point =
        [self.sourceMonitorView convertPoint:event.locationInWindow
                                    fromView:nil];
    const long double fraction = std::clamp<long double>(
        point.x / self.sourceMonitorView.bounds.size.width, 0.0L, 1.0L);
    const int64_t frames =
        source->duration.to_frames(source->rate.num, source->rate.den);
    [self requestSourcePosition:{static_cast<int64_t>(
                                     std::llround(fraction * frames)) *
                                     static_cast<int64_t>(source->rate.den),
                                 source->rate.num}];
}

- (void)refreshTimelineChrome {
    if (!self.metalView || !self.state) return;
    for (NSTextField* label in self.trackHeaderLabels)
        [label removeFromSuperview];
    [self.trackHeaderLabels removeAllObjects];

    if (self.offlineMediaLabel)
        self.offlineMediaLabel.frame =
            NSMakeRect(0.0, 0.0, self.programMonitorView.bounds.size.width,
                       self.programMonitorView.bounds.size.height);
    // Labels now live in the ordered Metal atlas display list. Keeping the
    // former AppKit overlays hidden avoids two coordinate systems drifting
    // apart when the timeline is resized.
    self.timelineTimecodeLabel.hidden = YES;
}

- (void)refreshBinControlsSelecting:(NSString*)selectedBinId {
    if (!self.binOutline || !self.mediaTable) return;
    NSString* requested = selectedBinId ?: self.selectedBinId ?: @"__all__";
    self.selectedBinId = requested;
    self.updatingBinControls = YES;
    [self.binOutline reloadData];
    [self.binOutline expandItem:nil expandChildren:YES];
    const NSInteger row = [self.binOutline rowForItem:requested];
    if (row >= 0)
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
    self.updatingBinControls = NO;
    [self rebuildMediaList];
    // Bin selection/membership is shared between the Media and Audio tabs --
    // both filter the same document.library (see MediaPanelModel.h) -- so
    // keep the Audio tab's list current from this one central place.
    if (self.audioTable) [self rebuildAudioList];
}

- (void)binSelectionChanged:(id)sender {
    (void)sender;
    if (self.updatingBinControls) return;
    const NSInteger row = self.binOutline.selectedRow;
    if (row < 0) return;
    NSString* selected = [self.binOutline itemAtRow:row];
    if ([selected isEqualToString:self.selectedBinId]) return;
    self.selectedBinId = selected;
    [self rebuildMediaList];
}

- (NSArray<NSString*>*)childBinIds:(NSString*)parentId {
    NSMutableArray<NSString*>* result = [NSMutableArray array];
    const std::string parent(parentId.UTF8String ?: "");
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins)
        if (bin.parent_id == parent) bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins)
        [result addObject:[NSString stringWithUTF8String:bin->id.c_str()]];
    return result;
}

- (NSInteger)outlineView:(NSOutlineView*)outlineView
    numberOfChildrenOfItem:(id)item {
    (void)outlineView;
    if (!item) return 2;
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) return 0;
    if ([identifier isEqualToString:@"__root__"])
        return [self childBinIds:@""].count;
    return [self childBinIds:identifier].count;
}

- (id)outlineView:(NSOutlineView*)outlineView
            child:(NSInteger)index
           ofItem:(id)item {
    (void)outlineView;
    if (!item) {
        if (index == 0) return @"__all__";
        return @"__root__";
    }
    if ([item isEqualToString:@"__root__"])
        return [self childBinIds:@""][index];
    return [self childBinIds:item][index];
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) return NO;
    if ([identifier isEqualToString:@"__root__"])
        return [self childBinIds:@""].count > 0;
    return [self childBinIds:identifier].count > 0;
}

- (id<NSPasteboardWriting>)outlineView:(NSOutlineView*)outlineView
               pasteboardWriterForItem:(id)item {
    (void)outlineView;
    NSString* identifier = item;
    if ([identifier hasPrefix:@"__"] ||
        !self.state->document.FindBin(identifier.UTF8String ?: ""))
        return nil;
    NSPasteboardItem* pasteboardItem = [[NSPasteboardItem alloc] init];
    [pasteboardItem setString:identifier forType:kCutmachineBinPasteboardType];
    return pasteboardItem;
}

- (NSString*)dropTargetBinForItem:(id)item {
    NSString* identifier = [item isKindOfClass:NSString.class] ? item : nil;
    if (!identifier || [identifier isEqualToString:@"__all__"] ||
        [identifier isEqualToString:@"__root__"])
        return @"";
    return self.state->document.FindBin(identifier.UTF8String ?: "")
               ? identifier
               : nil;
}

- (BOOL)bin:(NSString*)binId canMoveInto:(NSString*)parentId {
    if ([binId isEqualToString:parentId]) return NO;
    const DocumentBin* cursor =
        self.state->document.FindBin(parentId.UTF8String ?: "");
    while (cursor) {
        if (cursor->id == (binId.UTF8String ?: "")) return NO;
        cursor = self.state->document.FindBin(cursor->parent_id);
    }
    return YES;
}

- (NSDragOperation)outlineView:(NSOutlineView*)outlineView
                  validateDrop:(id<NSDraggingInfo>)info
                  proposedItem:(id)item
            proposedChildIndex:(NSInteger)index {
    NSString* target = [self dropTargetBinForItem:item];
    if (!target) return NSDragOperationNone;
    [outlineView setDropItem:item dropChildIndex:NSOutlineViewDropOnItemIndex];
    NSPasteboard* pasteboard = info.draggingPasteboard;
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (movingBin)
        return [self bin:movingBin canMoveInto:target] ? NSDragOperationMove
                                                       : NSDragOperationNone;
    if ([pasteboard stringForType:kCutmachineMediaPasteboardType])
        return NSDragOperationMove;
    if ([pasteboard stringForType:kCutmachineTimelinePasteboardType])
        return NSDragOperationMove;
    if ([pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL ]])
        return NSDragOperationCopy;
    (void)index;
    return NSDragOperationNone;
}

- (BOOL)outlineView:(NSOutlineView*)outlineView
         acceptDrop:(id<NSDraggingInfo>)info
               item:(id)item
         childIndex:(NSInteger)index {
    (void)outlineView;
    (void)index;
    NSString* target = [self dropTargetBinForItem:item];
    if (!target) return NO;
    NSPasteboard* pasteboard = info.draggingPasteboard;
    NSArray<NSURL*>* urls = [pasteboard
        readObjectsForClasses:@[ NSURL.class ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count > 0) return [self importMediaURLs:urls intoBin:target];

    NSString* media = [pasteboard stringForType:kCutmachineMediaPasteboardType];
    NSString* timeline =
        [pasteboard stringForType:kCutmachineTimelinePasteboardType];
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (timeline) {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        EditError error = EditError::None;
        std::string message;
        if (!projectLog.Apply(
                candidate,
                ProjectOperation{SetProjectTimelineBinOperation{
                    timeline.UTF8String ?: "", target.UTF8String ?: ""}},
                error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Déplacement refusé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                           CMStringFromUtf8(message)];
            return NO;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        NSString* selected = target.length == 0 ? @"__root__" : target;
        [self refreshBinControlsSelecting:selected];
        return YES;
    }
    Operation operation;
    if (media)
        operation = SetMediaBinOperation{media.UTF8String ?: "",
                                         target.UTF8String ?: ""};
    else if (movingBin && [self bin:movingBin canMoveInto:target])
        operation = MoveBinOperation{movingBin.UTF8String ?: "",
                                     target.UTF8String ?: ""};
    else
        return NO;

    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Déplacement impossible (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    NSString* selected = target.length == 0 ? @"__root__" : target;
    [self refreshBinControlsSelecting:selected];
    if (target.length > 0) [self.binOutline expandItem:target];
    return YES;
}

- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item {
    (void)outlineView;
    // PRD37 -- its name follows the row's selection (CMListCellView).
    CMListCellView* cell = [[CMListCellView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, tableColumn.width, 26.0)];
    NSImageView* icon =
        [[NSImageView alloc] initWithFrame:NSMakeRect(2.0, 6.0, 14.0, 14.0)];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    NSTextField* label = [NSTextField labelWithString:@""];
    label.frame = NSMakeRect(
        21.0, 4.0, std::max<CGFloat>(20.0, tableColumn.width - 23.0), 18.0);
    label.autoresizingMask = NSViewWidthSizable;
    label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
    label.textColor = CMThemeColor(ui::theme::ListRowLabel(false));
    NSString* identifier = item;
    if ([identifier isEqualToString:@"__all__"]) {
        label.stringValue = @"Tous";
        icon.image = SystemSymbol(@"square.grid.2x2", @"Tous les objets", 12.0);
        icon.contentTintColor = CMThemeColor(ui::theme::kTextTertiary);
    } else if ([identifier isEqualToString:@"__root__"]) {
        label.stringValue = @"Projet";
        icon.image = SystemSymbol(@"folder", @"Racine du projet", 12.0);
        icon.contentTintColor = CMAccentColor();
    } else {
        const DocumentBin* bin =
            self.state->document.FindBin(identifier.UTF8String ?: "");
        label.stringValue =
            bin ? [NSString stringWithUTF8String:bin->name.c_str()]
                : @"Chutier manquant";
        icon.image = SystemSymbol(@"folder", @"Chutier", 12.0);
        icon.contentTintColor = CMAccentColor();
        if (bin) {
            label.editable = YES;
            label.selectable = YES;
            label.delegate = self;
            label.identifier = [@"bin:" stringByAppendingString:identifier];
        }
    }
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    cell.imageView = icon;
    cell.textField = label;
    [cell addSubview:icon];
    [cell addSubview:label];
    return cell;
}

// PRD37 -- ROADMAP.md. A bin is selected as a rush is, in the theme's row
// (UiTheme.h kListRowSelected), not AppKit's blue.
- (NSTableRowView*)outlineView:(NSOutlineView*)outlineView
                rowViewForItem:(id)item {
    (void)outlineView;
    (void)item;
    return [[CMIndustrialTableRowView alloc] initWithFrame:NSZeroRect];
}

- (void)outlineViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == self.binOutline)
        [self binSelectionChanged:self.binOutline];
}

- (void)rebuildMediaList {
    self.visibleMediaIds = [NSMutableArray array];
    NSString* query = self.mediaSearchField.stringValue.lowercaseString ?: @"";
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    if (selected != "__all__") {
        const Ulid parent = selected == "__root__" ? Ulid{} : selected;
        const auto childBins = ui::media_panel::DirectChildBins(
            self.state->document.bins, parent, query.UTF8String ?: "");
        for (const DocumentBin* bin : childBins)
            [self.visibleMediaIds
                addObject:[NSString stringWithUTF8String:bin->id.c_str()]];
    }
    for (const DocumentSequence& sequence : self.state->project.timelines) {
        const auto placement =
            self.state->project.timeline_bin_ids.find(sequence.id);
        const Ulid binId =
            placement == self.state->project.timeline_bin_ids.end()
                ? Ulid{}
                : placement->second;
        if (selected != "__all__" &&
            ((selected == "__root__" && !binId.empty()) ||
             (selected != "__root__" && binId != selected)))
            continue;
        NSString* sequenceName =
            [NSString stringWithUTF8String:sequence.name.c_str()];
        if (query.length == 0 ||
            [sequenceName.lowercaseString rangeOfString:query].location !=
                NSNotFound) {
            [self.visibleMediaIds
                addObject:[NSString stringWithUTF8String:sequence.id.c_str()]];
        }
    }
    std::vector<const LibraryMedia*> mediaItems;
    for (const LibraryMedia& media : self.state->document.library) {
        if (selected != "__all__" &&
            ((selected == "__root__" && !media.bin_id.empty()) ||
             (selected != "__root__" && media.bin_id != selected)))
            continue;
        const std::string displayName =
            RushDisplayName(self.state->project, media);
        NSString* searchable =
            [NSString stringWithFormat:@"%@ %@ %@ %@ %@ %dx%d",
                                       CMStringFromUtf8(displayName),
                                       CMStringFromUtf8(media.filename),
                                       CMStringFromUtf8(media.path),
                                       CMStringFromUtf8(media.codec),
                                       CMStringFromUtf8(media.orientation),
                                       media.width, media.height];
        if (query.length > 0 &&
            [searchable.lowercaseString rangeOfString:query].location ==
                NSNotFound)
            continue;
        mediaItems.push_back(&media);
    }
    const Project* project = &self.state->project;
    std::stable_sort(
        mediaItems.begin(), mediaItems.end(),
        [project](const LibraryMedia* left, const LibraryMedia* right) {
            return RushDisplayName(*project, *left) <
                   RushDisplayName(*project, *right);
        });
    for (const LibraryMedia* media : mediaItems)
        [self.visibleMediaIds
            addObject:[NSString stringWithUTF8String:media->id.c_str()]];
    [self.mediaTable reloadData];
    [self.mediaCollection reloadData];
    const NSUInteger count = self.visibleMediaIds.count;
    self.binSummaryLabel.stringValue = [NSString
        stringWithFormat:@"%lu objet%@ affiché%@", (unsigned long)count,
                         count == 1 ? @"" : @"s", count == 1 ? @"" : @"s"];
    self.assignMediaButton.enabled = YES;
}

- (void)mediaViewChanged:(id)sender {
    (void)sender;
    const BOOL icons = self.mediaViewToggle.selectedSegment == 1;
    self.mediaListScroll.hidden = icons;
    self.mediaIconScroll.hidden = !icons;
    [self rebuildMediaList];
}

- (NSString*)selectedMediaId {
    return [self selectedMediaIds].firstObject;
}

- (NSString*)selectedBrowserObjectId {
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSIndexPath* path = self.mediaCollection.selectionIndexPaths.anyObject;
        return path && path.item < self.visibleMediaIds.count
                   ? self.visibleMediaIds[path.item]
                   : nil;
    }
    const NSInteger row = self.mediaTable.selectedRow;
    return row >= 0 && row < (NSInteger)self.visibleMediaIds.count
               ? self.visibleMediaIds[row]
               : nil;
}

- (NSArray<NSString*>*)selectedMediaIds {
    NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
    if (self.mediaViewToggle.selectedSegment == 1) {
        NSArray<NSIndexPath*>* paths =
            [self.mediaCollection.selectionIndexPaths.allObjects
                sortedArrayUsingComparator:^NSComparisonResult(
                    NSIndexPath* left, NSIndexPath* right) {
                  if (left.item < right.item) return NSOrderedAscending;
                  if (left.item > right.item) return NSOrderedDescending;
                  return NSOrderedSame;
                }];
        for (NSIndexPath* path in paths) {
            if (path.item >= self.visibleMediaIds.count) continue;
            NSString* identifier = self.visibleMediaIds[path.item];
            if (!self.state->document.FindBin(identifier.UTF8String ?: ""))
                [identifiers addObject:identifier];
        }
    } else {
        [self.mediaTable.selectedRowIndexes
            enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
              (void)stop;
              if (row >= self.visibleMediaIds.count) return;
              NSString* identifier = self.visibleMediaIds[row];
              if (!self.state->document.FindBin(identifier.UTF8String ?: ""))
                  [identifiers addObject:identifier];
            }];
    }
    return identifiers;
}

// ---- F2.3 Media panel: Média/Audio/Légendes tab switching -----------------
// See MediaPanelModel.h -- ui::media_panel::Tab is the pure-C++ source of
// truth for which tab exists and in what order; this method only toggles
// AppKit visibility and lazily rebuilds whichever tab's content just became
// visible (nothing here mutates the Document).

- (void)mediaTabChanged:(CMTabStripView*)sender {
    const NSInteger index = sender.selectedIndex;
    const auto& tabs = ui::media_panel::AllTabs();
    if (index < 0 || index >= static_cast<NSInteger>(tabs.size())) return;
    const ui::media_panel::Tab tab = tabs[index];
    self.mediaTabContentMedia.hidden = tab != ui::media_panel::Tab::Media;
    self.mediaTabContentAudio.hidden = tab != ui::media_panel::Tab::Audio;
    self.mediaTabContentCaptions.hidden = tab != ui::media_panel::Tab::Captions;
    self.effectsPanelView.hidden = tab != ui::media_panel::Tab::Effects;
    [CMUiPreferences.sharedPreferences
        setLastMediaPanelTab:
            [NSString
                stringWithUTF8String:ui::media_panel::TabIdentifier(tab)]];
    switch (tab) {
        case ui::media_panel::Tab::Media:
            [self rebuildMediaList];
            break;
        case ui::media_panel::Tab::Audio:
            [self rebuildAudioList];
            break;
        case ui::media_panel::Tab::Captions:
            [self rebuildCaptionStylesList];
            break;
        case ui::media_panel::Tab::Effects:
            break;
    }
}

// ---- F2.3 Media panel: Audio tab -------------------------------------------
// A filtered view onto the same document.library the Media tab browses --
// see MediaPanelModel.h's file comment on why "audio-capable"
// (LibraryMedia::has_audio) stands in for "audio-only source" here. No
// ingest logic is duplicated: this tab reads the library Ingest.cc already
// populated and mutates it only through the SetMediaBinOperation the Media
// tab applies. It filters by the bin chosen in the Media tab, whose list is
// not on screen here, so its summary names that bin.

- (void)buildAudioTabContentWithWidth:(double)width height:(double)height {
    self.mediaTabContentAudio =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
    self.mediaTabContentAudio.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentAudio];

    NSTextField* audioTitle = CMMakeSectionHeader(@"SOURCES AUDIO");
    audioTitle.frame = NSMakeRect(14.0, height - 34.0, width - 28.0, 18.0);
    audioTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentAudio addSubview:audioTitle];

    self.audioSearchField = [[NSSearchField alloc]
        initWithFrame:NSMakeRect(12.0, height - 70.0, width - 24.0, 26.0)];
    self.audioSearchField.placeholderString = @"Rechercher nom, codec…";
    self.audioSearchField.target = self;
    self.audioSearchField.action = @selector(audioSearchChanged:);
    self.audioSearchField.continuous = YES;
    self.audioSearchField.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentAudio addSubview:self.audioSearchField];

    self.audioTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, width - 24.0, 500.0)];
    for (NSArray<NSString*>* definition in @[
             @[ @"name", @"Nom", @"140" ], @[ @"format", @"Format", @"90" ],
             @[ @"duration", @"Durée", @"60" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        [self.audioTable addTableColumn:column];
    }
    self.audioTable.dataSource = self;
    self.audioTable.delegate = self;
    self.audioTable.allowsMultipleSelection = YES;
    self.audioTable.rowHeight = 26.0;
    self.audioTable.usesAlternatingRowBackgroundColors = NO;
    self.audioTable.backgroundColor = CMSurfacePanelColor();
    self.audioTable.target = self;
    self.audioTable.doubleAction = @selector(openSelectedAudioInSourceMonitor:);
    self.audioScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, 84.0, width - 24.0, height - 164.0)];
    self.audioScroll.autoresizingMask =
        NSViewHeightSizable | NSViewWidthSizable;
    self.audioScroll.documentView = self.audioTable;
    self.audioScroll.hasVerticalScroller = YES;
    self.audioScroll.hasHorizontalScroller = YES;
    self.audioScroll.borderType = NSNoBorder;
    self.audioScroll.drawsBackground = YES;
    self.audioScroll.backgroundColor = CMSurfacePanelColor();
    [self.mediaTabContentAudio addSubview:self.audioScroll];

    NSButton* openAudioButton = CMMakeStyledButton(
        @"SOURCE", self, @selector(openSelectedAudioInSourceMonitor:));
    openAudioButton.frame = NSMakeRect(12.0, 48.0, 140.0, 28.0);
    openAudioButton.autoresizingMask = NSViewMaxYMargin;
    openAudioButton.toolTip = @"Ouvrir la source dans le moniteur Source";
    [self.mediaTabContentAudio addSubview:openAudioButton];

    NSButton* assignAudioButton = CMMakeStyledButton(
        @"DÉPLACER…", self, @selector(assignSelectedAudioToBinPressed:));
    assignAudioButton.frame = NSMakeRect(width - 152.0, 48.0, 140.0, 28.0);
    assignAudioButton.autoresizingMask = NSViewMaxYMargin | NSViewMinXMargin;
    assignAudioButton.toolTip = @"Déplacer les sources vers un chutier";
    [self.mediaTabContentAudio addSubview:assignAudioButton];

    self.audioSummaryLabel = [NSTextField labelWithString:@""];
    self.audioSummaryLabel.frame = NSMakeRect(14.0, 8.0, width - 28.0, 32.0);
    self.audioSummaryLabel.autoresizingMask =
        NSViewMaxYMargin | NSViewWidthSizable;
    self.audioSummaryLabel.font = [NSFont systemFontOfSize:11.0];
    self.audioSummaryLabel.textColor = NSColor.secondaryLabelColor;
    self.audioSummaryLabel.maximumNumberOfLines = 2;
    self.audioSummaryLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [self.mediaTabContentAudio addSubview:self.audioSummaryLabel];
}

- (void)audioSearchChanged:(id)sender {
    (void)sender;
    [self rebuildAudioList];
}

- (void)rebuildAudioList {
    if (!self.state) return;
    const std::string selected(self.selectedBinId.UTF8String ?: "__all__");
    const bool anyBin = selected == "__all__";
    const bool wantRoot = selected == "__root__";
    const Ulid wantBinId = anyBin || wantRoot ? Ulid{} : selected;
    const std::string search(self.audioSearchField.stringValue.UTF8String
                                 ?: "");
    const auto matches = ui::media_panel::FilterAudioSources(
        self.state->document.library, anyBin, wantRoot, wantBinId, search);
    // F2.3 -- every tab change rebuilds this list, and reloadData dropped
    // the selection: a source chosen here was gone after a look at the
    // Captions tab (agentic QA of 23 September 2026). Kept by identity, so
    // a source the new filter hides simply leaves the selection.
    NSSet<NSString*>* previouslySelected =
        [NSSet setWithArray:[self selectedAudioIds]];
    self.visibleAudioIds = [NSMutableArray array];
    for (const LibraryMedia* media : matches)
        [self.visibleAudioIds
            addObject:[NSString stringWithUTF8String:media->id.c_str()]];
    [self.audioTable reloadData];
    NSMutableIndexSet* reselected = [NSMutableIndexSet indexSet];
    for (NSUInteger row = 0; row < self.visibleAudioIds.count; ++row)
        if ([previouslySelected containsObject:self.visibleAudioIds[row]])
            [reselected addIndex:row];
    if (reselected.count > 0)
        [self.audioTable selectRowIndexes:reselected byExtendingSelection:NO];
    NSString* scope = @"tous les chutiers";
    if (wantRoot) {
        scope = @"la racine du projet";
    } else if (!anyBin) {
        const DocumentBin* bin = self.state->document.FindBin(wantBinId);
        // Not %s: it reads the name in the system C-string encoding, not
        // UTF-8, and garbles every accented bin name.
        NSString* name = bin != nullptr
                             ? [NSString stringWithUTF8String:bin->name.c_str()]
                             : nil;
        scope = name != nil
                    ? [NSString stringWithFormat:@"le chutier « %@ »", name]
                    : @"un chutier";
    }
    const NSUInteger count = self.visibleAudioIds.count;
    self.audioSummaryLabel.stringValue = [NSString
        stringWithFormat:@"%lu source%@ audio dans %@ (chutier choisi dans "
                         @"l’onglet Média)",
                         (unsigned long)count, count == 1 ? @"" : @"s", scope];
}

- (NSString*)selectedAudioId {
    return [self selectedAudioIds].firstObject;
}

- (NSArray<NSString*>*)selectedAudioIds {
    NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
    [self.audioTable.selectedRowIndexes
        enumerateIndexesUsingBlock:^(NSUInteger row, BOOL* stop) {
          (void)stop;
          if (row < self.visibleAudioIds.count)
              [identifiers addObject:self.visibleAudioIds[row]];
        }];
    return identifiers;
}

- (void)openSelectedAudioInSourceMonitor:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedAudioId];
    if (!identifier) return;
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (void)assignSelectedAudioToBinPressed:(id)sender {
    (void)sender;
    [self moveMediaIdsToBin:[self selectedAudioIds]];
}

// ---- F2.3 Media panel: Captions tab ----------------------------------------
// Lists sequence.caption_styles with the number of cartons in each, creates
// and deletes styles, and dresses the cartons selected on the timeline in
// one of them, or gives them back the default look. A style dresses only
// cartons, the clips of caption tracks: nothing else is drawn as a caption,
// by the monitor or the export (MediaPanelModel.h, DescribeCaptionSelection
// and RestyleCartons). Every change goes through EditLog::Apply, a restyle
// as one BatchOperation: one undo gives every carton back.

// VQA1 -- keeps the caption style table exactly as wide as its clip view:
// the count column keeps the width its "Cartons" header needs and the style
// name takes what is left. The automatic table style resolves to the inset
// style, whose side padding -tile adds to the frame, so the padding is
// measured after a first tile instead of being assumed.
- (void)layoutCaptionStyleColumns {
    NSTableView* table = self.captionStyleTable;
    if (!table) return;
    NSTableColumn* style = [table tableColumnWithIdentifier:@"style"];
    NSTableColumn* clips = [table tableColumnWithIdentifier:@"clips"];
    const double available =
        NSWidth(self.captionStyleScroll.contentView.bounds);
    const double spacing = 2.0 * table.intercellSpacing.width;
    style.width = std::max(40.0, available - clips.width - spacing);
    [table tile];
    const double overflow = NSWidth(table.frame) - available;
    if (overflow > 0.0) {
        style.width = std::max(40.0, style.width - overflow);
        [table tile];
    }
}

- (void)buildCaptionsTabContentWithWidth:(double)width height:(double)height {
    self.mediaTabContentCaptions =
        [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, width, height)];
    self.mediaTabContentCaptions.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    [self.mediaPanel addSubview:self.mediaTabContentCaptions];

    NSTextField* stylesTitle = CMMakeSectionHeader(@"STYLES DE LÉGENDE");
    stylesTitle.frame = NSMakeRect(14.0, height - 34.0, width - 28.0, 18.0);
    stylesTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:stylesTitle];

    // F2.3 -- 140 pt rather than 220 since the style's look is set below:
    // everything still fits over the window's minimum height.
    self.captionStyleTable = [[ContextTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, width - 24.0, 140.0)];
    self.captionStyleTable.autoresizingMask = NSViewWidthSizable;
    // VQA1 -- uniform resizing shrank the count column below its header and
    // clipped "Cartons"; with any AppKit policy the columns were stretched
    // while the pane was still wide and the table never shrank back with
    // its clip view, leaving the column past the visible edge. Columns are
    // therefore laid out by -layoutCaptionStyleColumns, not by AppKit.
    self.captionStyleTable.columnAutoresizingStyle =
        NSTableViewNoColumnAutoresizing;
    for (NSArray<NSString*>* definition in @[
             @[ @"style", @"Style", @"196" ], @[ @"clips", @"Cartons", @"76" ]
         ]) {
        NSTableColumn* column =
            [[NSTableColumn alloc] initWithIdentifier:definition[0]];
        column.title = definition[1];
        column.width = definition[2].doubleValue;
        column.resizingMask = NSTableColumnNoResizing;
        [self.captionStyleTable addTableColumn:column];
    }
    self.captionStyleTable.dataSource = self;
    self.captionStyleTable.delegate = self;
    self.captionStyleTable.rowHeight = 26.0;
    self.captionStyleTable.usesAlternatingRowBackgroundColors = NO;
    self.captionStyleTable.backgroundColor = CMSurfacePanelColor();
    self.captionStyleScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, height - 184.0, width - 24.0, 140.0)];
    self.captionStyleScroll.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionStyleScroll.documentView = self.captionStyleTable;
    self.captionStyleScroll.hasVerticalScroller = YES;
    self.captionStyleScroll.borderType = NSNoBorder;
    self.captionStyleScroll.drawsBackground = YES;
    self.captionStyleScroll.backgroundColor = CMSurfacePanelColor();
    [self.mediaTabContentCaptions addSubview:self.captionStyleScroll];
    NSClipView* captionStyleClip = self.captionStyleScroll.contentView;
    captionStyleClip.postsFrameChangedNotifications = YES;
    __weak AppDelegate* weakSelf = self;
    [NSNotificationCenter.defaultCenter
        addObserverForName:NSViewFrameDidChangeNotification
                    object:captionStyleClip
                     queue:nil
                usingBlock:^(NSNotification*) {
                  [weakSelf layoutCaptionStyleColumns];
                }];
    [self layoutCaptionStyleColumns];

    self.addCaptionStyleButton = CMMakeStyledButton(
        @"+ STYLE", self, @selector(createCaptionStylePressed:));
    self.addCaptionStyleButton.frame =
        NSMakeRect(12.0, height - 220.0, 140.0, 28.0);
    self.addCaptionStyleButton.autoresizingMask = NSViewMinYMargin;
    self.addCaptionStyleButton.toolTip = @"Nouveau style de légende";
    [self.mediaTabContentCaptions addSubview:self.addCaptionStyleButton];

    self.removeCaptionStyleButton = CMMakeStyledButton(
        @"− STYLE", self, @selector(deleteCaptionStylePressed:));
    self.removeCaptionStyleButton.frame =
        NSMakeRect(width - 152.0, height - 220.0, 140.0, 28.0);
    self.removeCaptionStyleButton.autoresizingMask =
        NSViewMinYMargin | NSViewMinXMargin;
    self.removeCaptionStyleButton.toolTip =
        @"Supprimer le style sélectionné, s’il n’habille aucun carton ; un "
        @"plan vidéo qui le porte sans l’afficher le perd";
    [self.mediaTabContentCaptions addSubview:self.removeCaptionStyleButton];

    // F2.3 -- the look of the selected style: font, size, text colour. Each
    // change is one UpdateCaptionStyleOperation (RestyleCaptionStyle), so
    // every carton wearing the style follows, and one undo takes it back.
    NSTextField* lookTitle = CMMakeSectionHeader(@"ASPECT DU STYLE");
    lookTitle.frame = NSMakeRect(14.0, height - 254.0, width - 28.0, 18.0);
    lookTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:lookTitle];
    const auto lookLabel = [&](NSString* title, double x, double y) {
        NSTextField* label = [NSTextField labelWithString:title];
        label.frame = NSMakeRect(x, y + 3.0, 52.0, 18.0);
        label.autoresizingMask = NSViewMinYMargin;
        label.font = [NSFont systemFontOfSize:11.0];
        label.textColor = NSColor.secondaryLabelColor;
        [self.mediaTabContentCaptions addSubview:label];
    };
    lookLabel(@"Police", 14.0, height - 286.0);
    self.captionFontPopup = [[NSPopUpButton alloc]
        initWithFrame:NSMakeRect(68.0, height - 288.0, width - 80.0, 26.0)
            pullsDown:NO];
    self.captionFontPopup.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionFontPopup.controlSize = NSControlSizeSmall;
    self.captionFontPopup.font = [NSFont systemFontOfSize:11.0];
    // "system" is what the monitor and the export both draw in Helvetica
    // Neue (CaptionFontName); every other entry is an installed family.
    CMFillCaptionFontPopup(self.captionFontPopup);
    self.captionFontPopup.target = self;
    self.captionFontPopup.action = @selector(captionLookChanged:);
    self.captionFontPopup.toolTip = @"Police des cartons de ce style";
    [self.mediaTabContentCaptions addSubview:self.captionFontPopup];

    lookLabel(@"Taille", 14.0, height - 318.0);
    self.captionSizeField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(68.0, height - 316.0, 52.0, 22.0)];
    self.captionSizeField.autoresizingMask = NSViewMinYMargin;
    self.captionSizeField.font =
        [NSFont monospacedDigitSystemFontOfSize:11.0
                                         weight:NSFontWeightRegular];
    NSNumberFormatter* sizeFormat = [[NSNumberFormatter alloc] init];
    sizeFormat.numberStyle = NSNumberFormatterNoStyle;
    sizeFormat.allowsFloats = NO;
    sizeFormat.minimum = @(ui::media_panel::kCaptionTabMinimumFontSize);
    sizeFormat.maximum = @(ui::media_panel::kCaptionTabMaximumFontSize);
    self.captionSizeField.formatter = sizeFormat;
    self.captionSizeField.target = self;
    self.captionSizeField.action = @selector(captionLookChanged:);
    self.captionSizeField.toolTip =
        @"Taille en pixels à la résolution de la séquence";
    [self.mediaTabContentCaptions addSubview:self.captionSizeField];
    self.captionSizeStepper = [[NSStepper alloc]
        initWithFrame:NSMakeRect(122.0, height - 318.0, 19.0, 26.0)];
    self.captionSizeStepper.autoresizingMask = NSViewMinYMargin;
    self.captionSizeStepper.minValue =
        ui::media_panel::kCaptionTabMinimumFontSize;
    self.captionSizeStepper.maxValue =
        ui::media_panel::kCaptionTabMaximumFontSize;
    self.captionSizeStepper.increment = 2.0;
    self.captionSizeStepper.valueWraps = NO;
    self.captionSizeStepper.target = self;
    self.captionSizeStepper.action = @selector(captionLookChanged:);
    [self.mediaTabContentCaptions addSubview:self.captionSizeStepper];

    lookLabel(@"Couleur", 160.0, height - 318.0);
    self.captionColorWell = [[NSColorWell alloc]
        initWithFrame:NSMakeRect(214.0, height - 318.0, 44.0, 26.0)];
    self.captionColorWell.autoresizingMask = NSViewMinYMargin;
    // One edit when the colour is chosen, not one per step of the drag.
    self.captionColorWell.continuous = NO;
    self.captionColorWell.target = self;
    self.captionColorWell.action = @selector(captionLookChanged:);
    self.captionColorWell.toolTip =
        @"Couleur du texte ; l’opacité enregistrée est gardée";
    [self.mediaTabContentCaptions addSubview:self.captionColorWell];

    NSTextField* selectionTitle = CMMakeSectionHeader(@"CARTONS SÉLECTIONNÉS");
    selectionTitle.frame = NSMakeRect(14.0, height - 352.0, width - 28.0, 18.0);
    selectionTitle.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self.mediaTabContentCaptions addSubview:selectionTitle];

    self.captionSelectionLabel = [NSTextField labelWithString:@""];
    self.captionSelectionLabel.frame =
        NSMakeRect(14.0, height - 404.0, width - 28.0, 48.0);
    self.captionSelectionLabel.autoresizingMask =
        NSViewMinYMargin | NSViewWidthSizable;
    self.captionSelectionLabel.font = [NSFont systemFontOfSize:11.0];
    self.captionSelectionLabel.textColor = NSColor.secondaryLabelColor;
    self.captionSelectionLabel.maximumNumberOfLines = 3;
    self.captionSelectionLabel.lineBreakMode = NSLineBreakByWordWrapping;
    [self.mediaTabContentCaptions addSubview:self.captionSelectionLabel];

    self.applyCaptionStyleButton = CMMakeStyledButton(
        @"APPLIQUER LE STYLE", self, @selector(applyCaptionStylePressed:));
    self.applyCaptionStyleButton.frame =
        NSMakeRect(12.0, height - 442.0, 140.0, 28.0);
    self.applyCaptionStyleButton.autoresizingMask = NSViewMinYMargin;
    self.applyCaptionStyleButton.toolTip =
        @"Habiller les cartons sélectionnés du style choisi, texte conservé";
    [self.mediaTabContentCaptions addSubview:self.applyCaptionStyleButton];

    self.clearCaptionButton = CMMakeStyledButton(
        @"STYLE PAR DÉFAUT", self, @selector(clearCaptionPressed:));
    self.clearCaptionButton.frame =
        NSMakeRect(width - 152.0, height - 442.0, 140.0, 28.0);
    self.clearCaptionButton.autoresizingMask =
        NSViewMinYMargin | NSViewMinXMargin;
    self.clearCaptionButton.toolTip =
        @"Rendre aux cartons sélectionnés l’aspect par défaut, texte conservé";
    [self.mediaTabContentCaptions addSubview:self.clearCaptionButton];
}

- (void)rebuildCaptionStylesList {
    if (!self.state) return;
    const Ulid selectedStyle = [self selectedCaptionStyleId];
    const auto summaries =
        ui::media_panel::SummarizeCaptionStyles(self.state->document.sequence);
    self.visibleCaptionStyleIds = [NSMutableArray array];
    for (const auto& summary : summaries)
        [self.visibleCaptionStyleIds
            addObject:[NSString stringWithUTF8String:summary.style_id.c_str()]];
    [self.captionStyleTable reloadData];
    // A reload loses the table's selection; keep the style chosen, which the
    // buttons below act on.
    for (NSUInteger row = 0; row < self.visibleCaptionStyleIds.count; ++row)
        if (selectedStyle ==
            (self.visibleCaptionStyleIds[row].UTF8String ?: ""))
            [self.captionStyleTable
                    selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
    [self updateCaptionSelectionLabel];
}

// F2.3 -- the look controls show the selected style, and are off without
// one.
- (void)updateCaptionStyleControls {
    const Ulid styleId = [self selectedCaptionStyleId];
    const CaptionStyle* style =
        self.state && !styleId.empty()
            ? self.state->document.FindCaptionStyle(styleId)
            : nullptr;
    for (NSControl* control in @[
             self.captionFontPopup, self.captionSizeField,
             self.captionSizeStepper, self.captionColorWell
         ])
        control.enabled = style != nullptr;
    if (!style) {
        self.captionSizeField.stringValue = @"";
        return;
    }
    // A family set through MCP that this Mac does not have: shown as it is,
    // so choosing nothing changes nothing.
    CMSelectCaptionFont(self.captionFontPopup, style->font_family);
    self.captionSizeField.integerValue = style->font_size;
    self.captionSizeStepper.integerValue = style->font_size;
    self.captionColorWell.color =
        CMCaptionColor(style->color, NSColor.whiteColor);
}

- (void)captionLookChanged:(id)sender {
    if (!self.state) return;
    const Ulid styleId = [self selectedCaptionStyleId];
    const CaptionStyle* stored =
        styleId.empty() ? nullptr
                        : self.state->document.FindCaptionStyle(styleId);
    if (!stored) return;
    if (sender == self.captionSizeStepper)
        self.captionSizeField.integerValue =
            self.captionSizeStepper.integerValue;
    ui::media_panel::CaptionStyleLook look;
    NSString* family = self.captionFontPopup.selectedItem.representedObject;
    look.font_family = family.UTF8String ?: "";
    look.font_size = static_cast<int32_t>(self.captionSizeField.integerValue);
    look.color = stored->color;
    // The well shows the stored colour through NSColor, which rounds: only
    // a colour actually chosen replaces the stored spelling.
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    if (CMChosenCaptionChannels(self.captionColorWell.color, stored->color, red,
                                green, blue))
        look.color = ui::media_panel::CaptionColorFromWell(stored->color, red,
                                                           green, blue);
    std::string refusal;
    const auto update = ui::media_panel::RestyleCaptionStyle(
        self.state->document.sequence, styleId, look, refusal);
    if (!update) {
        if (!refusal.empty())
            self.captionSelectionLabel.stringValue =
                [NSString stringWithFormat:@"Aspect refusé : %@",
                                           CMStringFromUtf8(refusal)];
        [self updateCaptionStyleControls];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{*update}
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Aspect refusé : %@", CMStringFromUtf8(message)];
        [self updateCaptionStyleControls];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
}

- (void)updateCaptionSelectionLabel {
    const Ulid styleId = [self selectedCaptionStyleId];
    self.removeCaptionStyleButton.enabled = !styleId.empty();
    [self updateCaptionStyleControls];
    if (!self.state || !self.state->interaction) {
        self.captionSelectionLabel.stringValue = @"Aucune timeline chargée.";
        self.applyCaptionStyleButton.enabled = NO;
        self.clearCaptionButton.enabled = NO;
        return;
    }
    const ui::media_panel::CaptionSelection selection =
        ui::media_panel::DescribeCaptionSelection(
            self.state->document, self.state->interaction->SelectedClipIds(),
            styleId);
    self.applyCaptionStyleButton.enabled =
        !styleId.empty() && selection.in_style < selection.cartons.size();
    self.clearCaptionButton.enabled = !selection.cartons.empty();
    if (selection.cartons.empty()) {
        self.captionSelectionLabel.stringValue =
            selection.other_clips == 0
                ? @"Sélectionnez des cartons sur une piste de légendes."
                : @"Aucun carton dans la sélection : un style n’habille que "
                  @"les cartons d’une piste de légendes.";
        return;
    }
    const size_t cartons = selection.cartons.size();
    NSMutableString* text = [NSMutableString
        stringWithFormat:@"%zu carton%@ sélectionné%@", cartons,
                         cartons == 1 ? @"" : @"s", cartons == 1 ? @"" : @"s"];
    if (!styleId.empty())
        [text appendFormat:@" · %zu déjà dans ce style", selection.in_style];
    if (selection.other_clips > 0)
        [text appendFormat:@" · %zu autre%@ clip%@ ignoré%@",
                           selection.other_clips,
                           selection.other_clips == 1 ? @"" : @"s",
                           selection.other_clips == 1 ? @"" : @"s",
                           selection.other_clips == 1 ? @"" : @"s"];
    if (styleId.empty())
        [text appendString:@". Choisissez un style dans la liste."];
    self.captionSelectionLabel.stringValue = text;
}

- (Ulid)selectedCaptionStyleId {
    const NSInteger row = self.captionStyleTable.selectedRow;
    if (row < 0 ||
        row >= static_cast<NSInteger>(self.visibleCaptionStyleIds.count))
        return {};
    return Ulid(self.visibleCaptionStyleIds[row].UTF8String ?: "");
}

- (void)createCaptionStylePressed:(id)sender {
    (void)sender;
    if (!self.state) return;
    const CaptionStyle style;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            AddCaptionStyleOperation { style, -1 }
        }
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue =
            [NSString stringWithFormat:@"Création refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    [self updateSelectionInfo];
    // The new style is the one the next "apply" is meant for.
    const NSUInteger row = [self.visibleCaptionStyleIds
        indexOfObject:[NSString stringWithUTF8String:style.id.c_str()]];
    if (row != NSNotFound)
        [self.captionStyleTable
                selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
}

- (void)deleteCaptionStylePressed:(id)sender {
    (void)sender;
    if (!self.state) return;
    const Ulid styleId = [self selectedCaptionStyleId];
    if (styleId.empty()) return;
    int32_t cartons = 0;
    int32_t others = 0;
    for (const auto& summary :
         ui::media_panel::SummarizeCaptionStyles(self.state->document.sequence))
        if (summary.style_id == styleId) {
            cartons = summary.carton_count;
            others = summary.other_clip_count;
        }
    if (cartons > 0) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Ce style habille %d carton%@ : donnez-leur "
                             @"un autre style avant de le supprimer.",
                             cartons, cartons == 1 ? @"" : @"s"];
        return;
    }
    // F2.3 -- pictures that carry the style lose it in the same edit.
    const auto removal =
        ui::media_panel::CaptionStyleRemoval(self.state->document, styleId);
    if (!removal) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"%d plans portent ce style : trop pour une "
                             @"seule édition.",
                             others];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:*removal
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    [self updateSelectionInfo];
    if (others > 0)
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Style supprimé, et retiré de %d plan%@ qui le "
                             @"portai%@ sans l’afficher.",
                             others, others == 1 ? @"" : @"s",
                             others == 1 ? @"t" : @"ent"];
}

// Dresses the selected cartons in `styleId`, or in the default look when it
// is empty, as one undoable edit.
- (void)restyleSelectedCartons:(const Ulid&)styleId {
    if (!self.state || !self.state->interaction) return;
    const ui::media_panel::CaptionSelection selection =
        ui::media_panel::DescribeCaptionSelection(
            self.state->document, self.state->interaction->SelectedClipIds(),
            styleId);
    if (selection.cartons.size() > kMaxBatchOperations) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"%zu cartons sélectionnés : au plus %zu en une "
                             @"fois.",
                             selection.cartons.size(), kMaxBatchOperations];
        return;
    }
    const auto batch = ui::media_panel::RestyleCartons(
        self.state->document, selection.cartons, styleId);
    if (!batch) return;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{*batch}
                                          error:error
                                        message:message]) {
        self.captionSelectionLabel.stringValue = [NSString
            stringWithFormat:@"Style refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
}

- (void)applyCaptionStylePressed:(id)sender {
    (void)sender;
    const Ulid styleId = [self selectedCaptionStyleId];
    if (!styleId.empty()) [self restyleSelectedCartons:styleId];
}

- (void)clearCaptionPressed:(id)sender {
    (void)sender;
    [self restyleSelectedCartons:Ulid{}];
}

- (NSInteger)collectionView:(NSCollectionView*)collectionView
     numberOfItemsInSection:(NSInteger)section {
    (void)collectionView;
    (void)section;
    return self.visibleMediaIds.count;
}

- (NSCollectionViewItem*)collectionView:(NSCollectionView*)collectionView
    itemForRepresentedObjectAtIndexPath:(NSIndexPath*)indexPath {
    MediaIconItem* item = [collectionView makeItemWithIdentifier:@"media-icon"
                                                    forIndexPath:indexPath];
    if (indexPath.item >= self.visibleMediaIds.count) return item;
    NSString* identifier = self.visibleMediaIds[indexPath.item];
    item.representedObject = identifier;
    ((MediaIconView*)item.view).indexPath = indexPath;
    item.textField.editable = YES;
    item.textField.selectable = YES;
    item.textField.delegate = self;
    item.textField.identifier =
        [@"browser:" stringByAppendingString:identifier];
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    if (bin) {
        item.textField.stringValue =
            [NSString stringWithUTF8String:bin->name.c_str()];
        item.imageView.image = SystemSymbol(@"folder.fill", @"Chutier", 38.0);
        item.imageView.contentTintColor = CMThemeColor(ui::theme::kAccent);
        item.imageView.layer.backgroundColor = NSColor.clearColor.CGColor;
        item.view.toolTip = @"Double-cliquer pour ouvrir ce chutier";
        return item;
    }
    const DocumentSequence* timeline =
        self.state->project.FindTimeline(identifier.UTF8String ?: "");
    if (timeline) {
        const DocumentSequence& sequence = *timeline;
        item.textField.stringValue =
            [NSString stringWithUTF8String:sequence.name.c_str()];
        item.imageView.image =
            SystemSymbol(@"rectangle.stack.fill", @"Séquence", 34.0);
        item.imageView.contentTintColor = CMAccentColor();
        item.imageView.layer.backgroundColor = NSColor.clearColor.CGColor;
        item.view.toolTip = [NSString
            stringWithFormat:@"Séquence · %dx%d · %d/%d fps · %lu pistes",
                             sequence.width, sequence.height,
                             sequence.frame_rate.num, sequence.frame_rate.den,
                             (unsigned long)sequence.tracks.size()];
        return item;
    }
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    if (!media) return item;
    item.textField.stringValue = [NSString
        stringWithUTF8String:RushDisplayName(self.state->project, *media)
                                 .c_str()];
    const bool offline = self.state->offlineSourceIds.count(media->id) != 0;
    NSImage* thumbnail = offline ? nil : self.mediaThumbnails[identifier];
    if (thumbnail) {
        item.imageView.contentTintColor = nil;
        item.imageView.image = thumbnail;
    } else {
        NSImage* symbol = [NSImage
            imageWithSystemSymbolName:(offline ? @"exclamationmark.triangle"
                                               : @"film")
             accessibilityDescription:(offline ? @"Média offline"
                                               : @"Média vidéo")];
        symbol.size = NSMakeSize(44.0, 44.0);
        item.imageView.image = symbol;
        item.imageView.contentTintColor =
            offline ? CMThemeColor(ui::theme::kError) : CMTextSecondaryColor();
    }
    item.imageView.layer.backgroundColor = CMSurfaceRaisedColor().CGColor;
    item.view.toolTip =
        offline ? [NSString stringWithFormat:@"%@ · MÉDIA OFFLINE",
                                             CMStringFromUtf8(media->filename)]
                : [NSString stringWithFormat:@"%@ · %@ · %dx%d · %@",
                                             CMStringFromUtf8(media->filename),
                                             CMStringFromUtf8(media->codec),
                                             media->width, media->height,
                                             TimeString(media->duration)];
    if (!offline && !media->proxy_path.empty())
        item.view.toolTip =
            [item.view.toolTip stringByAppendingString:@" · PROXY disponible"];
    return item;
}

- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)collectionView
       pasteboardWriterForItemAtIndexPath:(NSIndexPath*)indexPath {
    (void)collectionView;
    if (indexPath.item >= self.visibleMediaIds.count) return nil;
    if (self.state->document.FindBin(
            self.visibleMediaIds[indexPath.item].UTF8String ?: ""))
        return nil;
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    NSString* identifier = self.visibleMediaIds[indexPath.item];
    const NSPasteboardType type =
        self.state->project.FindTimeline(identifier.UTF8String ?: "")
            ? kCutmachineTimelinePasteboardType
            : kCutmachineMediaPasteboardType;
    [item setString:identifier forType:type];
    return item;
}

- (BOOL)collectionView:(NSCollectionView*)collectionView
    canDragItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths
                   withEvent:(NSEvent*)event {
    (void)collectionView;
    (void)event;
    for (NSIndexPath* path in indexPaths) {
        if (path.item >= self.visibleMediaIds.count) continue;
        const std::string identifier(self.visibleMediaIds[path.item].UTF8String
                                         ?: "");
        if (self.state->document.FindLibraryMedia(identifier) ||
            self.state->project.FindTimeline(identifier))
            return YES;
    }
    return NO;
}

- (NSString*)iconGridDropTargetAtIndexPath:(NSIndexPath*)indexPath
                             dropOperation:
                                 (NSCollectionViewDropOperation)dropOperation {
    if (dropOperation == NSCollectionViewDropOn && indexPath &&
        indexPath.item < self.visibleMediaIds.count) {
        NSString* identifier = self.visibleMediaIds[indexPath.item];
        if (self.state->document.FindBin(identifier.UTF8String ?: ""))
            return identifier;
    }
    if (self.state->document.FindBin(self.selectedBinId.UTF8String ?: ""))
        return self.selectedBinId;
    return @"";
}

- (NSDragOperation)collectionView:(NSCollectionView*)collectionView
                     validateDrop:(id<NSDraggingInfo>)draggingInfo
                proposedIndexPath:(NSIndexPath**)proposedDropIndexPath
                    dropOperation:
                        (NSCollectionViewDropOperation*)proposedDropOperation {
    if (collectionView != self.mediaCollection || !proposedDropIndexPath ||
        !*proposedDropIndexPath)
        return NSDragOperationNone;
    NSString* target =
        [self iconGridDropTargetAtIndexPath:*proposedDropIndexPath
                              dropOperation:*proposedDropOperation];

    NSPasteboard* pasteboard = draggingInfo.draggingPasteboard;
    NSString* movingMedia =
        [pasteboard stringForType:kCutmachineMediaPasteboardType];
    if (movingMedia) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(movingMedia.UTF8String ?: "");
        if (!media || media->bin_id == (target.UTF8String ?: ""))
            return NSDragOperationNone;
        return NSDragOperationMove;
    }
    NSString* movingTimeline =
        [pasteboard stringForType:kCutmachineTimelinePasteboardType];
    if (movingTimeline) {
        const Ulid timelineId(movingTimeline.UTF8String ?: "");
        if (!self.state->project.FindTimeline(timelineId))
            return NSDragOperationNone;
        const auto placement =
            self.state->project.timeline_bin_ids.find(timelineId);
        const Ulid current =
            placement == self.state->project.timeline_bin_ids.end()
                ? Ulid{}
                : placement->second;
        return current == (target.UTF8String ?: "") ? NSDragOperationNone
                                                    : NSDragOperationMove;
    }
    NSString* movingBin =
        [pasteboard stringForType:kCutmachineBinPasteboardType];
    if (movingBin) {
        if (![self bin:movingBin canMoveInto:target])
            return NSDragOperationNone;
        return NSDragOperationMove;
    }
    if ([pasteboard availableTypeFromArray:@[ NSPasteboardTypeFileURL ]])
        return NSDragOperationCopy;
    return NSDragOperationNone;
}

- (BOOL)collectionView:(NSCollectionView*)collectionView
            acceptDrop:(id<NSDraggingInfo>)draggingInfo
             indexPath:(NSIndexPath*)indexPath
         dropOperation:(NSCollectionViewDropOperation)dropOperation {
    if (collectionView != self.mediaCollection) return NO;
    NSString* target = [self iconGridDropTargetAtIndexPath:indexPath
                                             dropOperation:dropOperation];
    // Reuse the sidebar's operation-backed drop path: media classification,
    // bin nesting, Finder ingest, persistence, and refresh stay identical for
    // both representations of the same project architecture.
    return [self outlineView:self.binOutline
                  acceptDrop:draggingInfo
                        item:target.length > 0 ? target : @"__root__"
                  childIndex:NSOutlineViewDropOnItemIndex];
}

- (BOOL)tableView:(NSTableView*)tableView
    writeRowsWithIndexes:(NSIndexSet*)rowIndexes
            toPasteboard:(NSPasteboard*)pasteboard {
    if (tableView != self.mediaTable || rowIndexes.count != 1) return NO;
    const NSUInteger row = rowIndexes.firstIndex;
    if (row >= self.visibleMediaIds.count) return NO;
    if (self.state->document.FindBin(self.visibleMediaIds[row].UTF8String
                                         ?: ""))
        return NO;
    NSString* identifier = self.visibleMediaIds[row];
    const NSPasteboardType type =
        self.state->project.FindTimeline(identifier.UTF8String ?: "")
            ? kCutmachineTimelinePasteboardType
            : kCutmachineMediaPasteboardType;
    [pasteboard declareTypes:@[ type ] owner:nil];
    return [pasteboard setString:identifier forType:type];
}

- (void)openIconBin:(NSClickGestureRecognizer*)recognizer {
    if (recognizer.state != NSGestureRecognizerStateEnded) return;
    const NSPoint point = [recognizer locationInView:self.mediaCollection];
    NSIndexPath* path = [self.mediaCollection indexPathForItemAtPoint:point];
    if (!path || path.item >= self.visibleMediaIds.count) return;
    NSString* identifier = self.visibleMediaIds[path.item];
    if (!self.state->document.FindBin(identifier.UTF8String ?: "")) return;
    self.mediaCollection.selectionIndexPaths = [NSSet setWithObject:path];
    self.selectedBinId = identifier;
    const NSInteger row = [self.binOutline rowForItem:identifier];
    if (row >= 0) {
        self.updatingBinControls = YES;
        [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
        self.updatingBinControls = NO;
    }
    [self rebuildMediaList];
}

- (void)openIconItem:(id)sender {
    NSIndexPath* path = nil;
    if ([sender isKindOfClass:NSGestureRecognizer.class]) {
        NSGestureRecognizer* recognizer = sender;
        if (recognizer.state != NSGestureRecognizerStateEnded) return;
        const NSPoint point = [recognizer locationInView:self.mediaCollection];
        path = [self.mediaCollection indexPathForItemAtPoint:point];
    } else {
        path = self.mediaCollection.selectionIndexPaths.anyObject;
    }
    if (!path || path.item >= self.visibleMediaIds.count) return;
    NSString* identifier = self.visibleMediaIds[path.item];
    if (self.state->document.FindBin(identifier.UTF8String ?: "")) return;
    self.mediaCollection.selectionIndexPaths = [NSSet setWithObject:path];
    [self openSelectedMediaInSourceMonitor:sender];
}

- (void)openSelectedMediaInSourceMonitor:(id)sender {
    (void)sender;
    NSString* browserObject = [self selectedBrowserObjectId];
    if (browserObject &&
        self.state->document.FindBin(browserObject.UTF8String ?: "")) {
        [self refreshBinControlsSelecting:browserObject];
        return;
    }
    NSString* identifier = [self selectedMediaId];
    if (!identifier) {
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        if (clip)
            identifier =
                [NSString stringWithUTF8String:clip->source_id.c_str()];
    }
    if (!identifier) return;
    if (self.state->project.FindTimeline(identifier.UTF8String ?: "")) {
        [self activateTimelineIdentifier:identifier];
        return;
    }
    [self openMediaIdentifierInSourceMonitor:identifier];
}

- (BOOL)activateTimelineIdentifier:(NSString*)identifier {
    const Ulid timelineId(identifier.UTF8String ?: "");
    if (!self.state->project.FindTimeline(timelineId)) return NO;
    [self setPlaybackDirection:0];
    // FC4 -- edit points belong to the timeline they were selected on.
    if (timelineId != self.state->activeTimelineId) {
        [self leaveTrimMode];
        self.state->dynamicTrimMode = false;
        self.state->retimeClipIds.clear();
    }
    self.state->sourceMonitor = false;
    self.state->sourceMonitorActive = false;
    self.state->sourceIn.reset();
    self.state->sourceOut.reset();
    if (timelineId == self.state->activeTimelineId) {
        [self requestResolvedPosition:self.state->requestedPosition];
        return YES;
    }

    const HistoryDomain historyBeforeNavigation = self.state->lastHistoryDomain;
    std::string message;
    if (![self persistEdits:message]) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Changement de timeline impossible : %@",
                             CMStringFromUtf8(message)];
        return NO;
    }
    self.state->lastHistoryDomain = historyBeforeNavigation;
    const Ulid previousId = self.state->activeTimelineId;
    self.state->timelinePositions[previousId] = self.state->requestedPosition;
    self.state->timelineEditLogs[previousId] = self.state->editLog;
    self.state->timelineTargetTracks[previousId] = self.state->targetedTrackIds;
    // PRD37 -- ROADMAP.md. Each timeline keeps its own view, as it keeps
    // its playhead (TimelineView.h, OpenTimelineView).
    self.state->timelineViews[previousId] =
        RememberTimelineView(self.state->viewport, self.state->timelineFit);

    EditLog nextLog;
    const auto storedLog = self.state->timelineEditLogs.find(timelineId);
    if (storedLog != self.state->timelineEditLogs.end())
        nextLog = storedLog->second;
    self.state->activeTimelineId = timelineId;
    self.state->document = self.state->project.MakeDocument(timelineId);
    self.state->editLog = std::move(nextLog);
    self.state->timelineEditLogs[timelineId] = self.state->editLog;
    self.state->targetedTrackIds.clear();
    const auto storedTargets =
        self.state->timelineTargetTracks.find(timelineId);
    if (storedTargets != self.state->timelineTargetTracks.end()) {
        for (const Ulid& id : storedTargets->second)
            if (self.state->document.FindTrack(id))
                self.state->targetedTrackIds.insert(id);
    } else {
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            self.state->targetedTrackIds.insert(track.id);
    }
    self.state->timelineTargetTracks[timelineId] = self.state->targetedTrackIds;
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->sourceRendered = {};
    self.state->rendered.clear();
    RationalTime position{0, self.state->document.sequence.frame_rate.num};
    const auto storedPosition = self.state->timelinePositions.find(timelineId);
    if (storedPosition != self.state->timelinePositions.end())
        position = storedPosition->second;
    // PRD37 -- the view it was left with, or all of it the first time.
    const auto storedView = self.state->timelineViews.find(timelineId);
    self.state->timelineFit = OpenTimelineView(
        self.state->viewport,
        storedView != self.state->timelineViews.end() ? &storedView->second
                                                      : nullptr,
        self.state->timeline->Duration(), self.metalView.bounds.size.width);
    self.programMonitorTitleLabel.stringValue =
        CMRecordTitle(self.state->document.sequence.name);
    [self refreshTimelineAfterEditFromPosition:position];
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.window.title = CMWindowTitle(self.state->project.name,
                                      self.state->document.sequence.name);
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Timeline active : %@",
                         CMStringFromUtf8(self.state->document.sequence.name)];
    self.state->overlayDirty = true;
    [self splitMergedLinkGroups];
    return YES;
}

// Out of lot -- the author, 4 November 2026: « J'ai encore le problème des blocs
// fusionnés que je peux pas bouger ». A timeline cut before #213 binds a
// block's stretches under one link, so one click took the whole block
// (LinkGroupRepair.h). Split as the timeline opens, through the journal: one
// undo step (two past 256 links), said on the status line.
- (void)splitMergedLinkGroups {
    const LinkGroupRepair repair = PlanLinkGroupRepair(
        self.state->document, [] { return GenerateUlid(); });
    if (repair.Empty()) return;
    size_t steps = 0;
    for (BatchOperation& batch : LinkGroupRepairBatches(repair)) {
        EditError error = EditError::None;
        std::string message;
        if (![self applyAndPersistTimelineOperation:Operation{std::move(batch)}
                                              error:error
                                            message:message]) {
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Liens fusionnés non séparés : %@",
                                           CMStringFromUtf8(message)];
            break;
        }
        ++steps;
    }
    if (steps == 0) return;
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Liens séparés : %lu bloc(s) monté(s) avant le 30 "
                         @"octobre, chaque morceau garde sa paire image-son "
                         @"(⌘Z pour annuler).",
                         static_cast<unsigned long>(repair.groups)];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. A timeline's name, asked on
// the window's form panel (FormPanel.h). An empty name closed the alert and
// created nothing; it now keeps the panel open and says so.
- (void)createTimelinePressed:(id)sender {
    (void)sender;
    NSTextField* nameField =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    nameField.identifier = @"timeline.new.name";
    nameField.placeholderString = @"Nom de la timeline";
    nameField.stringValue = [NSString
        stringWithFormat:@"Timeline %lu",
                         (unsigned long)self.state->project.timelines.size() +
                             1];
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Nouvelle timeline"
              message:@"La nouvelle timeline utilisera le format de la "
                      @"timeline active."
                 form:nameField];
    [panel addButtonWithTitle:@"Créer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = nameField;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      return CMTrimmedName(nameField.stringValue).length == 0
                 ? kTimelineNameMissing
                 : nil;
    };
    [panel
        presentInWindow:self.window
             completion:^(NSModalResponse response) {
               if (response != NSAlertFirstButtonReturn) return;
               [self createTimelineNamed:CMTrimmedName(nameField.stringValue)];
             }];
}

- (void)createTimelineNamed:(NSString*)trimmed {
    std::string message;
    if (![self persistEdits:message]) return;
    Project candidate = self.state->project;
    ProjectEditLog projectLog = self.state->projectEditLog;
    const DocumentSequence& current = self.state->document.sequence;
    EditError editError = EditError::None;
    ProjectOperation operation = AddProjectTimelineOperation{
        trimmed.UTF8String ?: "New Timeline", current.width, current.height,
        current.frame_rate};
    if (!projectLog.Apply(candidate, std::move(operation), editError,
                          message)) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Création refusée (%@) : %@",
                             CMStringFromUtf8(EditErrorName(editError)),
                             CMStringFromUtf8(message)];
        return;
    }
    const auto& applied = std::get<AddProjectTimelineOperation>(
        projectLog.AppliedEntries().back().op);
    const Ulid createdId = applied.timeline_id;
    if (![self commitProjectCandidate:candidate
                              editLog:self.state->editLog
                           projectLog:projectLog
                              message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Création impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->project = std::move(candidate);
    self.state->projectEditLog = std::move(projectLog);
    self.state->lastHistoryDomain = HistoryDomain::Project;
    [self rebuildMediaList];
    [self
        activateTimelineIdentifier:[NSString
                                       stringWithUTF8String:createdId.c_str()]];
}

- (void)createTimelineFromSelectedMedia:(id)sender {
    (void)sender;
    std::vector<Ulid> mediaIds;
    for (NSString* identifier in [self selectedMediaIds]) {
        const Ulid mediaId(identifier.UTF8String ?: "");
        if (self.state->document.FindLibraryMedia(mediaId))
            mediaIds.push_back(mediaId);
    }
    if (mediaIds.empty()) {
        self.binSummaryLabel.stringValue =
            @"Sélectionnez au moins un rush pour créer une timeline.";
        NSBeep();
        return;
    }

    // PRD26 -- on the form panel, as « Nouvelle timeline ».
    NSTextField* nameField =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 320, 24)];
    nameField.identifier = @"timeline.from_selection.name";
    nameField.stringValue = [NSString
        stringWithFormat:@"Timeline %lu",
                         (unsigned long)self.state->project.timelines.size() +
                             1];
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Nouvelle timeline avec la sélection"
              message:[NSString
                          stringWithFormat:
                              @"Les %lu rushes seront placés bout à bout.",
                              (unsigned long)mediaIds.size()]
                 form:nameField];
    [panel addButtonWithTitle:@"Créer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = nameField;
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      return CMTrimmedName(nameField.stringValue).length == 0
                 ? kTimelineNameMissing
                 : nil;
    };
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self createTimelineNamed:CMTrimmedName(nameField.stringValue)
                                  fromMedia:mediaIds];
                }];
}

- (void)createTimelineNamed:(NSString*)trimmed
                  fromMedia:(const std::vector<Ulid>&)mediaIds {
    const DocumentSequence& current = self.state->document.sequence;
    Project finalProject = self.state->project;
    AddProjectTimelineOperation enriched{trimmed.UTF8String ?: "New Timeline",
                                         current.width, current.height,
                                         current.frame_rate};
    enriched.timeline_id = GenerateUlid();
    enriched.video_track_id = GenerateUlid();
    enriched.audio_track_id = GenerateUlid();
    DocumentSequence createdTimeline;
    createdTimeline.id = enriched.timeline_id;
    createdTimeline.name = enriched.name;
    createdTimeline.width = enriched.width;
    createdTimeline.height = enriched.height;
    createdTimeline.frame_rate = enriched.frame_rate;
    createdTimeline.tracks = {{enriched.video_track_id, "video", 0, {}},
                              {enriched.audio_track_id, "audio", 1, {}}};
    finalProject.timelines.push_back(std::move(createdTimeline));
    EditError editError = EditError::None;
    std::string message;
    DocumentSequence* timeline =
        finalProject.FindTimeline(enriched.timeline_id);
    if (!timeline) return;
    DocumentTrack* videoTrack = nullptr;
    DocumentTrack* audioTrack = nullptr;
    for (DocumentTrack& track : timeline->tracks) {
        if (track.id == enriched.video_track_id) videoTrack = &track;
        if (track.id == enriched.audio_track_id) audioTrack = &track;
    }
    if (!videoTrack || !audioTrack) return;

    RationalTime position{0, 1};
    for (const Ulid& mediaId : mediaIds) {
        const auto foundMedia = std::find_if(
            finalProject.rushes.begin(), finalProject.rushes.end(),
            [&](const LibraryMedia& value) { return value.id == mediaId; });
        const LibraryMedia* media =
            foundMedia == finalProject.rushes.end() ? nullptr : &*foundMedia;
        const DocumentSource* source = self.state->document.FindSource(mediaId);
        if (!media || !source) continue;
        const Ulid linkGroup = media->has_audio ? GenerateUlid() : Ulid{};
        DocumentClip video;
        video.source_id = mediaId;
        video.source_in = {0, source->duration.rate};
        video.duration = source->duration;
        video.timeline_in = position;
        video.include_audio = false;
        video.link_group_id = linkGroup;
        // A rush without sound is laid unlinked, without an anchor either:
        // Document::Validate refuses an anchor outside a link group, and the
        // timeline was refused whole (window test, 9 October 2026).
        if (!linkGroup.empty()) video.sync_anchor_clip_id = video.id;
        videoTrack->clips.push_back(video);
        if (media->has_audio) {
            DocumentClip audio;
            audio.source_id = mediaId;
            audio.source_in = video.source_in;
            audio.duration = video.duration;
            audio.timeline_in = video.timeline_in;
            audio.include_audio = true;
            audio.link_group_id = linkGroup;
            audio.sync_anchor_clip_id = video.id;
            audioTrack->clips.push_back(std::move(audio));
        }
        position = position.add(source->duration);
    }
    if (!finalProject.Validate(message)) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Timeline invalide : %@",
                                       CMStringFromUtf8(message)];
        return;
    }

    enriched.exact_project_result =
        ExactProjectState{finalProject.SaveToString()};
    Project candidate = self.state->project;
    ProjectEditLog projectLog = self.state->projectEditLog;
    if (!projectLog.Apply(candidate, ProjectOperation{std::move(enriched)},
                          editError, message) ||
        ![self commitProjectCandidate:candidate
                              editLog:self.state->editLog
                           projectLog:projectLog
                              message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Création impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->project = std::move(candidate);
    self.state->projectEditLog = std::move(projectLog);
    self.state->lastHistoryDomain = HistoryDomain::Project;
    [self rebuildMediaList];
    [self activateTimelineIdentifier:[NSString
                                         stringWithUTF8String:timeline->id
                                                                  .c_str()]];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    const SEL action = item.action;
    // ON9 -- the loop, checked while on.
    if (action == @selector(menuToggleLoop:)) {
        item.state =
            self.loopPlayback ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    // ON1, ON2 -- the page shown and the panels open on it, checked.
    if (action == @selector(showMediaPage:) ||
        action == @selector(showEditPage:) ||
        action == @selector(showColorPage:)) {
        const ui::workspace::Page page =
            action == @selector(showMediaPage:)  ? ui::workspace::Page::Media
            : action == @selector(showEditPage:) ? ui::workspace::Page::Edit
                                                 : ui::workspace::Page::Color;
        item.state = self.workspacePage == page ? NSControlStateValueOn
                                                : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(toggleMediaPanel:) || action == @selector
                                                      (toggleInspectorPanel:)) {
        const ui::workspace::PanelSet panels = self.workspacePanels;
        item.state = (action == @selector(toggleMediaPanel:) ? panels.media
                                                             : panels.inspector)
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(toggleAgentWindow:)) {
        item.state =
            self.agentShown ? NSControlStateValueOn : NSControlStateValueOff;
        return self.agentWindow != nil;
    }
    // G6 -- checked when on; the split and the forgetting need a still.
    if (action == @selector(toggleGradeBypass:)) {
        if (!self.state) return NO;
        item.state = self.state->monitorCompare.bypass_grade
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(toggleReferenceSplit:)) {
        if (!self.state) return NO;
        item.state = self.state->monitorCompare.split ? NSControlStateValueOn
                                                      : NSControlStateValueOff;
        return [self referenceStillFits];
    }
    if (action == @selector(forgetReferenceStill:))
        return self.state && self.state->referenceStill != nullptr;
    // GR11 -- one graded shot to copy from; a copied grade and a selection
    // to paste on.
    if (action == @selector(copyClipGrade:)) {
        if (!self.state || !self.state->interaction) return NO;
        const std::vector<Ulid> selected =
            self.state->interaction->SelectedClipIds();
        if (selected.size() != 1) return NO;
        const DocumentClip* clip =
            self.state->document.FindClip(selected.front());
        return clip != nullptr &&
               ClipGradeRefusal(self.state->document, *clip).empty();
    }
    if (action == @selector(pasteClipGrade:))
        return self.state && self.state->interaction &&
               self.state->gradeClipboard.has_value() &&
               !self.state->interaction->SelectedClipIds().empty();
    if (action == @selector(grabReferenceStill:))
        return self.state && self.state->programRenderer != nullptr;
    // G3 -- a choice is checked when every selected picture reads so, and
    // shown mixed when some do.
    if (action == @selector(setSelectedMediaColorInput:) ||
        action == @selector(rereadSelectedMediaColorInput:)) {
        if (!self.state) return NO;
        size_t pictures = 0;
        size_t matching = 0;
        for (NSString* identifier in [self selectedMediaIds]) {
            const LibraryMedia* media = self.state->document.FindLibraryMedia(
                identifier.UTF8String ?: "");
            if (!media || (media->metadata_complete && !media->has_video))
                continue;
            ++pictures;
            if (static_cast<NSInteger>(
                    ui::media_panel::ColorInputChoiceOf(*media)) == item.tag)
                ++matching;
        }
        if (action == @selector(setSelectedMediaColorInput:))
            item.state = matching == 0          ? NSControlStateValueOff
                         : matching == pictures ? NSControlStateValueOn
                                                : NSControlStateValueMixed;
        return pictures > 0;
    }
    if (action == @selector(exportFinalVideo:))
        return self.state && !self.state->exportRunning;
    if (action == @selector(menuUndo:))
        return self.state && (self.state->editLog.AppliedCount() > 0 ||
                              self.state->projectEditLog.AppliedCount() > 0);
    if (action == @selector(menuRedo:))
        return self.state && (self.state->editLog.UndoneCount() > 0 ||
                              self.state->projectEditLog.UndoneCount() > 0);
    if (action == @selector(menuCopyTimelineClips:))
        return self.state && self.state->interaction &&
               !self.state->interaction->SelectedClipIds().empty();
    if (action == @selector(menuToggleClipsEnabled:))
        return self.state && self.state->interaction &&
               !ToggleClipsEnabled(self.state->document,
                                   self.state->interaction->SelectedClipIds())
                    .clips.empty();
    if (action == @selector(menuPasteTimelineClips:))
        return self.state && !self.state->timelineClipboard.empty();
    // D7 -- one carton selected, and nothing else.
    if (action == @selector(menuEditCartonText:))
        return self.state && self.state->interaction &&
               ui::media_panel::SelectedCartonForText(
                   self.state->document,
                   self.state->interaction->SelectedClipIds())
                   .has_value();
    if (action == @selector(menuAddCrossDissolve:))
        return self.state && self.state->interaction;
    if (action == @selector(menuRemoveCrossDissolve:))
        return self.state && !self.state->document.sequence.transitions.empty();
    // SY2 -- actionable whenever something is selected: the handler says,
    // in words, what the selection lacks, which a greyed item cannot.
    if (action == @selector(menuAutoAlignByWaveform:))
        return self.state && self.state->interaction &&
               !self.state->soundAlignmentRunning &&
               !self.state->interaction->SelectedClipIds().empty();
    if (action == @selector(menuActivateMulticamAngle:) ||
        action == @selector(menuClearMulticamAngle:)) {
        const BOOL clear = action == @selector(menuClearMulticamAngle:);
        SetMulticamActiveAngleOperation operation;
        return [self multicamSwitchForSelection:clear operation:operation];
    }
    if (action == @selector(menuInsertSource:) || action == @selector
                                                      (menuOverwriteSource:))
        return self.state && self.state->sourceMonitor &&
               self.state->document.FindSource(self.state->sourceMonitorId) !=
                   nullptr;
    if (action == @selector(menuToggleSnapping:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->interaction->SnappingEnabled()
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleLinkedSelection:)) {
        if (!self.state || !self.state->interaction) return NO;
        item.state = self.state->linkedSelection ? NSControlStateValueOn
                                                 : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleProxies:)) {
        if (!self.state) return NO;
        item.state = self.state->proxiesEnabled ? NSControlStateValueOn
                                                : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(menuToggleAutomaticProxies:)) {
        if (!self.state) return NO;
        item.state = self.state->automaticProxiesEnabled
                         ? NSControlStateValueOn
                         : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(renameBinPressed:) || action == @selector
                                                      (deleteBinPressed:))
        return self.state &&
               self.state->document.FindBin(self.selectedBinId.UTF8String
                                                ?: "") != nullptr;
    if (action == @selector(assignMediaToBinPressed:))
        return self.state &&
               self.state->document.FindLibraryMedia(
                   [self selectedMediaId].UTF8String ?: "") != nullptr;
    if (action == @selector(relinkSelectedMedia:))
        return self.state && self.state->document.FindLibraryMedia(
                                 [self selectedMediaId].UTF8String ?: "");
    if (action == @selector(batchRelinkOfflineMedia:))
        return self.state && !self.state->offlineSourceIds.empty() &&
               self.state->pendingBatchRelinks.empty();
    if (action == @selector(transcribeTimeline:)) {
        if (!self.state) return NO;
        for (const auto& batch : self.state->pendingTimelineTranscriptions)
            if (batch.second.timeline_id == self.state->activeTimelineId)
                return NO;
        // Keep the command actionable. The handler can explain an empty or
        // muted timeline; a disabled menu item cannot explain a false
        // negative caused by stale library probe metadata.
        return YES;
    }
    if (action == @selector(transcribeSelectedMedia:)) {
        const LibraryMedia* media =
            self.state ? self.state->document.FindLibraryMedia(
                             [self selectedMediaId].UTF8String ?: "")
                       : nullptr;
        return media && media->has_audio;
    }
    if (action == @selector(generateSelectedMediaProxy:) ||
        action == @selector(regenerateSelectedMediaThumbnail:))
        return self.state && self.state->document.FindLibraryMedia(
                                 [self selectedMediaId].UTF8String ?: "");
    if (action == @selector(removeSelectedMediaProxy:)) {
        const LibraryMedia* media =
            self.state ? self.state->document.FindLibraryMedia(
                             [self selectedMediaId].UTF8String ?: "")
                       : nullptr;
        return media && !media->proxy_path.empty();
    }
    if (action == @selector(removeContextTrackPressed:))
        return self.state &&
               self.state->document.FindTrack(self.state->contextTrackId);
    if (action == @selector(removeEmptyTracksPressed:))
        return self.state &&
               std::any_of(self.state->document.sequence.tracks.begin(),
                           self.state->document.sequence.tracks.end(),
                           [](const DocumentTrack& track) {
                               return track.clips.empty();
                           });
    if (action == @selector(menuCutSelectedAtPlayhead:)) {
        if (!self.state || !self.state->interaction) return NO;
        const DocumentClip* clip = self.state->document.FindClip(
            self.state->interaction->SelectedClipId());
        return clip && self.state->requestedPosition > clip->timeline_in &&
               self.state->requestedPosition <
                   clip->timeline_in.add(clip->duration);
    }
    return YES;
}

- (void)openMediaIdentifierInSourceMonitor:(NSString*)identifier {
    const Ulid sourceId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(sourceId);
    const LibraryMedia* media = self.state->document.FindLibraryMedia(sourceId);
    if (!source || !media) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré pour devenir une source montable.";
        return;
    }
    // ON2 -- Montage shows Source only once a rush is loaded into it.
    if (!self.state->sourceMonitorVisible && self.sourceMonitorToggleButton)
        [self toggleSourceMonitor:self.sourceMonitorToggleButton];
    [self setPlaybackDirection:0];
    [self setSourcePlaybackDirection:0];
    if (self.state->sourceMonitorId != sourceId) {
        self.state->sourceIn.reset();
        self.state->sourceOut.reset();
    }
    self.state->sourceMonitor = true;
    self.state->sourceMonitorActive = true;
    self.state->sourceMonitorId = sourceId;
    self.state->sourceMonitorPosition = {0, source->duration.rate};
    self.state->sourceRendered = {};
    self.sourceMonitorTitleLabel.stringValue = [NSString
        stringWithFormat:@"SOURCE — %@", CMStringFromUtf8(media->filename)];
    [self updateSourceZoneLabel];
    [self.window makeFirstResponder:self.sourceMonitorView];
    if (self.state->offlineSourceIds.count(sourceId) != 0) {
        self.sourceOfflineMediaLabel.hidden = NO;
        self.state->overlayDirty = true;
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"MONITEUR SOURCE    %@    MÉDIA OFFLINE",
                             CMStringFromUtf8(media->filename)];
        return;
    }
    self.sourceOfflineMediaLabel.hidden = YES;
    [self requestFrame:0 ofSource:sourceId];
    self.state->overlayDirty = true;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"MONITEUR SOURCE    %@    durée %@    %d/%d fps",
                         CMStringFromUtf8(media->filename),
                         TimeString(source->duration), source->rate.num,
                         source->rate.den];
}

- (void)monitorZoomChanged:(NSPopUpButton*)sender {
    const double zoom =
        sender.selectedItem.tag <= 0 ? 0.0 : sender.selectedItem.tag / 100.0;
    if (sender == self.sourceMonitorZoomPopup)
        self.state->sourceMonitorZoom = zoom;
    else if (sender == self.programMonitorZoomPopup)
        self.state->programMonitorZoom = zoom;
    else
        return;
    // Zoomed, the monitor hides nothing, and the reader reads every layer.
    if (sender == self.programMonitorZoomPopup) [self syncPlaybackReader];
    self.state->overlayDirty = true;
}

- (void)programVideoScopeChanged:(NSPopUpButton*)sender {
    self.state->programVideoScope = VideoScopeModeFromPreference(
        static_cast<int32_t>(sender.selectedItem.tag));
    [NSUserDefaults.standardUserDefaults
        setInteger:static_cast<NSInteger>(self.state->programVideoScope)
            forKey:kProgramVideoScopeDefaultsKey];
    self.state->overlayDirty = true;
}

// PRD37 -- ROADMAP.md. The strip along Record's bottom edge that its
// controls cover (the scope selector, the zoom, SOURCE ON), in the
// drawable's pixels: the scope panel is laid out above it
// (VideoScopes.h, LayoutVideoScopePanel).
- (double)programScopeBottomInset {
    double top = 0.0;
    for (NSView* control in @[
             self.programVideoScopePopup, self.programMonitorZoomPopup,
             self.sourceMonitorToggleButton
         ])
        if (control != nil && !control.hidden)
            top = std::max(top, static_cast<double>(NSMaxY(control.frame)));
    const double scale = self.programMonitorView.layer.contentsScale > 0.0
                             ? self.programMonitorView.layer.contentsScale
                             : 1.0;
    return top * scale;
}

#if defined(CUTMACHINE_UI_SMOKE_TEST)
// The scope panel as Record lays it out, in the monitor view's points (y
// up), for the window test to hold against the controls it must not cover.
- (NSRect)programScopePanelFrame {
    const NSRect bounds = self.programMonitorView.bounds;
    const double scale = self.programMonitorView.layer.contentsScale > 0.0
                             ? self.programMonitorView.layer.contentsScale
                             : 1.0;
    const double height = bounds.size.height * scale;
    const VideoScopePanel panel = LayoutVideoScopePanel(
        bounds.size.width * scale, height, [self programScopeBottomInset]);
    return NSMakeRect(panel.x / scale,
                      (height - (panel.y + panel.height)) / scale,
                      panel.width / scale, panel.height / scale);
}
#endif

- (void)toggleSourceMonitor:(NSButton*)sender {
    if (!self.applyingWorkspace &&
        self.workspacePage == ui::workspace::Page::Media &&
        self.state->sourceMonitorVisible) {
        return;
    }
    // Removing the pane from the split is deterministic. Merely hiding an
    // arranged subview leaves NSSplitView free to restore its autosaved
    // divider on the next layout pass, which made SOURCE OFF appear inert.
    const bool makeVisible = !self.state->sourceMonitorVisible;
    if (!makeVisible) {
        self.state->sourceMonitorPanelWidth =
            self.sourceMonitorPanel.frame.size.width;
        self.state->sourceMonitorVisible = false;
        self.state->sourceMonitorActive = false;
        [self.monitorSplitView removeArrangedSubview:self.sourceMonitorPanel];
        [self.sourceMonitorPanel removeFromSuperview];
        sender.state = NSControlStateValueOff;
        sender.title = @"SOURCE OFF";
        sender.image =
            SystemSymbol(@"rectangle", @"Moniteur Record seul", 11.0);
    } else {
        self.state->sourceMonitorVisible = true;
        [self.monitorSplitView insertArrangedSubview:self.sourceMonitorPanel
                                             atIndex:0];
        [self.monitorSplitView adjustSubviews];
        const double available = self.monitorSplitView.bounds.size.width -
                                 self.monitorSplitView.dividerThickness;
        [self.monitorSplitView setPosition:available * 0.5 ofDividerAtIndex:0];
        self.state->sourceMonitorActive = self.state->sourceMonitor;
        sender.state = NSControlStateValueOn;
        sender.title = @"SOURCE ON";
        sender.image = SystemSymbol(@"rectangle.split.2x1",
                                    @"Afficher le moniteur Source", 11.0);
    }
    [self.monitorSplitView adjustSubviews];
    self.state->overlayDirty = true;
    // ON2 -- the page keeps what the hand chose for Source.
    if (!self.applyingWorkspace && self.workspaceBar) {
        ui::workspace::PanelSet panels = self.workspacePanels;
        panels.source = self.state->sourceMonitorVisible;
        self.workspacePanels = panels;
        if (self.rememberWorkspace)
            [CMUiPreferences.sharedPreferences setPanels:panels
                                                 forPage:self.workspacePage];
        [self layOutPageContents];
    }
}

#pragma mark - ON1, ON2 pages

// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. A page from the bar, the menu
// or its shortcut: the panels the editor left open on it.
- (void)showWorkspacePage:(ui::workspace::Page)page {
    [self applyWorkspacePage:page
                      panels:[CMUiPreferences.sharedPreferences
                                 panelsForPage:page]
                    remember:self.rememberWorkspace];
}

- (void)toggleWorkspacePanel:(ui::workspace::Side)side {
    ui::workspace::PanelSet panels = self.workspacePanels;
    if (side == ui::workspace::Side::Media)
        panels.media = !panels.media;
    else
        panels.inspector = !panels.inspector;
    [self applyWorkspacePage:self.workspacePage
                      panels:panels
                    remember:self.rememberWorkspace];
}

- (void)applyWorkspacePage:(ui::workspace::Page)page
                    panels:(const ui::workspace::PanelSet&)requestedPanels
                  remember:(BOOL)remember {
    ui::workspace::PanelSet panels = requestedPanels;
    if (page == ui::workspace::Page::Media) {
        panels.source = true;
    }
    self.workspacePage = page;
    self.workspacePanels = panels;
    if (remember) {
        [CMUiPreferences.sharedPreferences setWorkspacePage:page];
        [CMUiPreferences.sharedPreferences setPanels:panels forPage:page];
    }
    self.applyingWorkspace = YES;
    [self layOutWorkspacePanes];
    if (self.state && self.state->sourceMonitorVisible != panels.source &&
        self.sourceMonitorToggleButton)
        [self toggleSourceMonitor:self.sourceMonitorToggleButton];
    [self layOutPageContents];
    self.inspectorView.colorWorkspace = page == ui::workspace::Page::Color;
    // Couleur is the grade: its dock opens on the Inspector.
    if (panels.inspector && page == ui::workspace::Page::Color)
        [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    self.applyingWorkspace = NO;
    [self.workspaceBar showPage:page panels:panels];
    if (self.state) {
        [self refreshTimelineChrome];
        self.state->overlayDirty = true;
    }
}

// The panes the page shows, in their order, each removed from the split or
// put back rather than hidden -- a hidden pane leaves NSSplitView free to
// give it back its old divider (see -toggleSourceMonitor:) -- then the
// dividers where WorkspacePages.h puts them.
- (void)layOutWorkspacePanes {
    NSSplitView* split = self.workspaceSplitView;
    if (!split || !self.mediaPanel || !self.editorSplitView ||
        !self.rightDockPanel)
        return;
    const ui::workspace::PanelSet panels = self.workspacePanels;
    NSMutableArray<NSView*>* wanted = [NSMutableArray array];
    if (panels.media) [wanted addObject:self.mediaPanel];
    [wanted addObject:self.editorSplitView];
    if (panels.inspector) [wanted addObject:self.rightDockPanel];
    for (NSView* pane in [split.arrangedSubviews copy]) {
        if ([wanted containsObject:pane]) continue;
        [split removeArrangedSubview:pane];
        [pane removeFromSuperview];
    }
    for (NSUInteger index = 0; index < wanted.count; ++index) {
        if ([split.arrangedSubviews containsObject:wanted[index]]) continue;
        [split insertArrangedSubview:wanted[index] atIndex:index];
    }
    // The centre gives way when the window is resized; a side panel keeps
    // the width it was given.
    for (NSUInteger index = 0; index < split.arrangedSubviews.count; ++index)
        [split setHoldingPriority:split.arrangedSubviews[index] ==
                                          self.editorSplitView
                                      ? NSLayoutPriorityDefaultLow
                                      : NSLayoutPriorityDefaultHigh
                forSubviewAtIndex:static_cast<NSInteger>(index)];
    [split adjustSubviews];
    const CGFloat width = split.bounds.size.width;
    const CGFloat divider = split.dividerThickness;
    // The widths the hand gave this page -- not during the smoke run, whose
    // layout must not depend on the editor's preferences.
    const ui::workspace::PanelWidths chosen =
        self.workspaceWidthsFixed ? self.workspaceFixedWidths
        : self.rememberWorkspace  ? [CMUiPreferences.sharedPreferences
                                       panelWidthsForPage:self.workspacePage]
                                  : ui::workspace::PanelWidths{};
    const ui::workspace::PanelWidths widths = ui::workspace::SidePanelWidths(
        self.workspacePage, panels, width, divider, chosen);
    NSInteger dividerIndex = 0;
    if (panels.media) {
        [split setPosition:widths.media ofDividerAtIndex:dividerIndex];
        ++dividerIndex;
    }
    if (panels.inspector)
        [split setPosition:width - widths.inspector - divider
            ofDividerAtIndex:dividerIndex];
    [self layOutMediaBrowser];
}

// ON5 -- a broad media page gives the bins readable names as well as a
// grid. Keep the compact browser's frames on the other pages.
- (void)layOutMediaBrowser {
    if (self.binSidebarScroll == nil) {
        return;
    }
    const double width = self.mediaTabContentMedia.bounds.size.width;
    const double sidebar =
        ui::workspace::BinSidebarWidth(self.workspacePage, width);
    NSRect frame = self.binSidebarScroll.frame;
    frame.size.width = sidebar;
    self.binSidebarScroll.frame = frame;
    self.binOutline.outlineTableColumn.width = sidebar;
    const CGFloat x = NSMaxX(frame) + ui::theme::kSpaceS;
    for (NSView* browser in @[ self.mediaListScroll, self.mediaIconScroll ]) {
        frame = browser.frame;
        frame.origin.x = x;
        frame.size.width = std::max<CGFloat>(0, width - x - ui::theme::kSpaceS);
        browser.frame = frame;
    }
    frame = self.mediaSearchField.frame;
    frame.origin.x = x;
    frame.size.width = std::max<CGFloat>(0, width - x - 80.0);
    self.mediaSearchField.frame = frame;
    [self.mediaCollection.collectionViewLayout invalidateLayout];
}

// ON4, ON5, ON7 -- re-use the same live views; a page changes only where
// they are placed. In particular no renderer or clean feed is replaced.
- (void)layOutPageContents {
    if (self.workspaceActivityScroll == nil) {
        return;
    }
    const auto page = self.workspacePage;
    const bool reference = self.workspaceWidthsFixed;
    const bool timeline = reference || ui::workspace::ShowsTimeline(page);
    const bool record = reference || ui::workspace::ShowsRecord(page);
    const bool activity = !reference && ui::workspace::ShowsActivity(page);
    NSSplitView* editor = self.editorSplitView;
    if (!timeline && [editor.arrangedSubviews containsObject:self.metalView]) {
        self.workspaceMonitorHeight = self.monitorSplitView.frame.size.height;
        [editor removeArrangedSubview:self.metalView];
        [self.metalView removeFromSuperview];
    } else if (timeline &&
               ![editor.arrangedSubviews containsObject:self.metalView]) {
        [editor addArrangedSubview:self.metalView];
        [editor adjustSubviews];
        [editor setPosition:self.workspaceMonitorHeight ofDividerAtIndex:0];
    }
    NSMutableArray<NSView*>* wanted = [NSMutableArray array];
    if (self.state->sourceMonitorVisible) {
        [wanted addObject:self.sourceMonitorPanel];
    }
    if (record) {
        [wanted addObject:self.programMonitorPanel];
    }
    if (activity) {
        [wanted addObject:self.workspaceActivityScroll];
    }
    NSSplitView* monitors = self.monitorSplitView;
    for (NSView* view in [monitors.arrangedSubviews copy]) {
        if ([wanted containsObject:view]) {
            continue;
        }
        [monitors removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    for (NSUInteger index = 0; index < wanted.count; ++index) {
        if (![monitors.arrangedSubviews containsObject:wanted[index]]) {
            [monitors insertArrangedSubview:wanted[index] atIndex:index];
        }
    }
    [editor adjustSubviews];
    [monitors adjustSubviews];
    if (activity) {
        const CGFloat edge = monitors.bounds.size.width -
                             ui::workspace::kActivityWidth -
                             monitors.dividerThickness;
        [monitors setPosition:std::max<CGFloat>(0, edge)
             ofDividerAtIndex:static_cast<NSInteger>(wanted.count) - 2];
    }
    if (record && self.state->sourceMonitorVisible) {
        const CGFloat available = monitors.bounds.size.width -
                                  (activity ? ui::workspace::kActivityWidth +
                                                  monitors.dividerThickness
                                            : 0);
        [monitors setPosition:(available - monitors.dividerThickness) * 0.5
             ofDividerAtIndex:0];
    }
    [self refreshWorkspaceActivity];
}

- (void)refreshWorkspaceActivity {
    if (self.workspaceActivityText == nil || self.state == nullptr) {
        return;
    }
    NSMutableString* text = [NSMutableString stringWithString:@"MARQUEURS\n\n"];
    for (const DocumentMarker& marker : self.state->document.sequence.markers) {
        [text appendFormat:@"%@ · %@\n",
                           TimelineTimecode(marker.time,
                                            [self playheadFrameRate]),
                           CMStringFromUtf8(marker.name)];
    }
    if (self.state->document.sequence.markers.empty()) {
        [text appendString:@"Aucun marqueur\n"];
    }
    [text appendString:@"\nMODIFICATIONS RÉCENTES\n\n"];
    const auto changes = ui::workspace::RecentChanges(self.state->editLog);
    for (const std::string& change : changes) {
        [text appendFormat:@"%@\n\n", CMStringFromUtf8(change)];
    }
    if (changes.empty()) {
        [text appendString:@"Aucune modification\n"];
    }
    // Selection and playhead changes do not replace the field, so reading
    // and copying the trace keeps the selection and scroll position.
    if (![self.workspaceActivityText.string isEqualToString:text]) {
        self.workspaceActivityText.string = text;
    }
}

- (void)workspaceBar:(CMWorkspaceBarView*)bar
       didChoosePage:(ui::workspace::Page)page {
    (void)bar;
    [self showWorkspacePage:page];
}

- (void)workspaceBar:(CMWorkspaceBarView*)bar
      didTogglePanel:(ui::workspace::Side)side {
    (void)bar;
    [self toggleWorkspacePanel:side];
}

- (void)showMediaPage:(id)sender {
    (void)sender;
    [self showWorkspacePage:ui::workspace::Page::Media];
}

- (void)showEditPage:(id)sender {
    (void)sender;
    [self showWorkspacePage:ui::workspace::Page::Edit];
}

- (void)showColorPage:(id)sender {
    (void)sender;
    [self showWorkspacePage:ui::workspace::Page::Color];
}

- (void)toggleMediaPanel:(id)sender {
    (void)sender;
    [self toggleWorkspacePanel:ui::workspace::Side::Media];
}

- (void)toggleInspectorPanel:(id)sender {
    (void)sender;
    [self toggleWorkspacePanel:ui::workspace::Side::Inspector];
}

- (void)workspaceBarDidToggleAgent:(CMWorkspaceBarView*)bar {
    (void)bar;
    [self toggleAgentWindow:nil];
}

static ui::agent_window::Rect CMAgentRect(NSRect rect) {
    return {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
}

// ON3 -- docs/roadmap/ONGLETS-2026-10.md. The Agent's own window, floating
// over the editor (AgentWindow.h), where it was the right dock's second tab:
// shown and hidden from any page by the bar's toggle, the Affichage menu or
// its key, and opened where it was left. A palette: it hides with the
// application, and the film shown full screen, whose window is above every
// floating one, covers it. Its buttons and its thread leave the keyboard
// where it was; its field takes it.
- (void)buildAgentWindow {
    NSPanel* panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0.0, 0.0,
                                       ui::agent_window::kDefaultWidth,
                                       ui::agent_window::kDefaultHeight)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:YES];
    panel.title = @"Agent";
    panel.floatingPanel = YES;
    panel.hidesOnDeactivate = YES;
    panel.becomesKeyOnlyIfNeeded = YES;
    panel.releasedWhenClosed = NO;
    panel.minSize = NSMakeSize(ui::agent_window::kMinimumWidth,
                               ui::agent_window::kMinimumHeight);
    panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    panel.backgroundColor = CMSurfacePanelColor();
    panel.delegate = self;
    self.chatPanelView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    panel.contentView = self.chatPanelView;
    // Its forms are the editor's, sheets on the editor's window.
    self.chatPanelView.formWindow = self.window;
    __weak AppDelegate* weakSelf = self;
    self.chatPanelView.activityHandler = ^{
      [weakSelf agentDidWrite];
    };
    self.agentWindow = panel;
}

- (void)toggleAgentWindow:(id)sender {
    (void)sender;
    [self showAgentWindow:!self.agentShown focus:YES];
}

// Shown where AgentWindow.h places it: where it was left this session, or
// at the last launch, while that can be reached on a screen. `focus` puts
// the keyboard in its field, for a request; hidden, the keyboard it had
// goes back to the editor.
- (void)showAgentWindow:(BOOL)shown focus:(BOOL)focus {
    NSPanel* panel = self.agentWindow;
    if (!panel) return;
    CMUiPreferences* preferences = CMUiPreferences.sharedPreferences;
    if (shown) {
        std::optional<ui::agent_window::Rect> remembered;
        if (self.agentPlaced)
            remembered = CMAgentRect(panel.frame);
        else if (self.rememberWorkspace)
            remembered = [preferences agentWindowFrame];
        std::vector<ui::agent_window::Rect> screens;
        for (NSScreen* screen in NSScreen.screens)
            screens.push_back(CMAgentRect(screen.visibleFrame));
        const ui::agent_window::Rect place = ui::agent_window::PlaceAgentWindow(
            remembered, CMAgentRect(self.window.frame), screens);
        self.agentPlacing = YES;
        [panel setFrame:NSMakeRect(place.x, place.y, place.width, place.height)
                display:NO];
        self.agentPlacing = NO;
        self.agentPlaced = YES;
        self.agentShown = YES;
        self.agentUnread = NO;
        if (focus) {
            [panel makeKeyAndOrderFront:nil];
            [self.chatPanelView focusInput];
        } else {
            [panel orderFront:nil];
        }
    } else {
        const BOOL hadKeyboard = panel.isKeyWindow;
        self.agentShown = NO;
        [panel orderOut:nil];
        if (hadKeyboard) [self.window makeKeyWindow];
    }
    if (self.rememberWorkspace) {
        [preferences setAgentWindowShown:self.agentShown];
        if (self.agentPlaced)
            [preferences setAgentWindowFrame:CMAgentRect(panel.frame)];
    }
    [self refreshAgentToggle];
}

// A row added to the thread while the window is hidden marks the toggle,
// until the window is shown again.
- (void)agentDidWrite {
    if (self.agentShown || self.agentUnread) return;
    self.agentUnread = YES;
    [self refreshAgentToggle];
}

- (void)refreshAgentToggle {
    NSString* spec = self.shortcutBindings[@"panel.agent"];
    NSString* key = spec ? CMStringFromUtf8(ui::shortcuts::DisplaySpec(
                               spec.UTF8String ?: ""))
                         : @"";
    [self.workspaceBar showAgentShown:self.agentShown
                               unread:self.agentUnread
                                  key:key];
}

// Moved or resized by the hand: where it opens next time.
- (void)agentWindowDidChangeFrame {
    if (self.agentPlacing || !self.agentShown || !self.rememberWorkspace)
        return;
    [CMUiPreferences.sharedPreferences
        setAgentWindowFrame:CMAgentRect(self.agentWindow.frame)];
}

- (void)windowDidMove:(NSNotification*)notification {
    if (notification.object == self.agentWindow)
        [self agentWindowDidChangeFrame];
}

// Its close button hides it, as the toggle does; the editor closed, the
// application quits, the Agent's window with it, still remembered shown.
- (void)windowWillClose:(NSNotification*)notification {
    if (notification.object == self.agentWindow) {
        if (!self.agentShown) return;
        self.agentShown = NO;
        if (self.rememberWorkspace)
            [CMUiPreferences.sharedPreferences setAgentWindowShown:NO];
        [self refreshAgentToggle];
        return;
    }
    if (notification.object == self.window) [self.agentWindow orderOut:nil];
}

// G3 -- docs/roadmap/GRADE-2026-09.md. One undo step for every selected
// media that has a picture and reads otherwise.
- (void)applyMediaColorInputs:
    (const std::vector<std::pair<Ulid, MediaColorInput>>&)inputs {
    const std::optional<Operation> operation =
        ui::media_panel::BuildSetMediaColorInputs(self.state->document, inputs);
    if (!operation) return;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:*operation
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Entrée couleur refusée : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
        return;
    }
    self.state->overlayDirty = true;
}

- (void)setSelectedMediaColorInput:(NSMenuItem*)sender {
    const auto& choices = ui::media_panel::ColorInputChoices();
    if (sender.tag < 0 || static_cast<size_t>(sender.tag) >= choices.size())
        return;
    std::vector<std::pair<Ulid, MediaColorInput>> inputs;
    for (NSString* identifier in [self selectedMediaIds])
        inputs.emplace_back(identifier.UTF8String ?: "",
                            choices[static_cast<size_t>(sender.tag)].input);
    [self applyMediaColorInputs:inputs];
}

- (void)rereadSelectedMediaColorInput:(id)sender {
    (void)sender;
    const std::filesystem::path projectDirectory =
        std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""))
            .parent_path();
    std::vector<std::pair<Ulid, MediaColorInput>> inputs;
    for (NSString* identifier in [self selectedMediaIds]) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
        if (!media || (media->metadata_complete && !media->has_video)) continue;
        std::filesystem::path path(media->path);
        if (path.is_relative()) path = projectDirectory / path;
        MediaFileColor color;
        std::string reason;
        if (!ReadFileColorFacts(path.lexically_normal().string(), color,
                                reason)) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Lecture impossible : %@",
                                           CMStringFromUtf8(reason)];
            NSBeep();
            return;
        }
        // A file that declares nothing leaves the media as it is, as
        // set_media_color_input's from_file does.
        if (DeclaresColorInput(color.input))
            inputs.emplace_back(media->id, color.input);
    }
    [self applyMediaColorInputs:inputs];
}

// --- G6 -- docs/roadmap/GRADE-2026-09.md: comparing on Record -------------

- (NSURL*)referenceStillURL {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    // The smoke run keeps its still in its own folder: it must never replace
    // the one a person grabbed.
    if (gUiSmokeTesting && !gUiSmokeRoot.empty())
        return [NSURL
            fileURLWithPath:CMStringFromUtf8(
                                (gUiSmokeRoot / "Reference.png").string())];
#endif
    NSURL* support = [NSFileManager.defaultManager
                         URLsForDirectory:NSApplicationSupportDirectory
                                inDomains:NSUserDomainMask]
                         .firstObject;
    return [[support URLByAppendingPathComponent:@"CUTMACHINE"]
        URLByAppendingPathComponent:@"Reference.png"];
}

// The preferences, and the still they name when its file still reads as
// one this build wrote: 8-bit RGBA, unmodified since.
- (void)loadMonitorCompare {
    self.state->monitorCompare =
        [CMUiPreferences.sharedPreferences monitorCompare];
    self.state->referenceStill = nullptr;
    NSString* path = [CMUiPreferences.sharedPreferences referenceStillPath];
    NSData* data = path ? [NSData dataWithContentsOfFile:path] : nil;
    NSBitmapImageRep* bitmap =
        data ? [NSBitmapImageRep imageRepWithData:data] : nil;
    if (bitmap && bitmap.bitsPerSample == 8 && bitmap.samplesPerPixel == 4 &&
        !bitmap.isPlanar && bitmap.bitmapData != nullptr &&
        bitmap.bytesPerRow == bitmap.pixelsWide * 4) {
        auto still = std::make_shared<ReferenceStill>();
        still->width = static_cast<int32_t>(bitmap.pixelsWide);
        still->height = static_cast<int32_t>(bitmap.pixelsHigh);
        still->rgba.assign(bitmap.bitmapData,
                           bitmap.bitmapData + still->ByteSize());
        self.state->referenceStill = std::move(still);
    }
    if (!self.state->referenceStill) self.state->monitorCompare.split = false;
}

- (void)saveMonitorCompare {
    [CMUiPreferences.sharedPreferences
        setMonitorCompare:self.state->monitorCompare];
    self.state->overlayDirty = true;
    [self refreshCompareBadge];
}

- (BOOL)referenceStillFits {
    return self.state && self.state->referenceStill &&
           ReferenceFits(*self.state->referenceStill,
                         self.state->document.sequence.width,
                         self.state->document.sequence.height);
}

- (void)refreshCompareBadge {
    if (!self.programCompareBadge || !self.state) return;
    NSMutableArray<NSString*>* parts = [NSMutableArray array];
    if (self.state->monitorCompare.bypass_grade)
        [parts addObject:@"SANS ÉTALONNAGE"];
    if (self.state->monitorCompare.split && [self referenceStillFits])
        [parts addObject:@"RÉFÉRENCE ◀ │ ▶ ACTUEL"];
    self.programCompareBadge.stringValue =
        [parts componentsJoinedByString:@"  ·  "];
    self.programCompareBadge.hidden = parts.count == 0;
}

- (void)toggleGradeBypass:(id)sender {
    (void)sender;
    if (!self.state) return;
    self.state->monitorCompare.bypass_grade =
        !self.state->monitorCompare.bypass_grade;
    [self saveMonitorCompare];
}

- (void)toggleReferenceSplit:(id)sender {
    (void)sender;
    if (!self.state) return;
    if (!self.state->monitorCompare.split && ![self referenceStillFits]) {
        NSBeep();
        return;
    }
    self.state->monitorCompare.split = !self.state->monitorCompare.split;
    [self saveMonitorCompare];
}

// The next Record render grabs itself (see -presentNearestFrameAtDeadline:).
- (void)grabReferenceStill:(id)sender {
    (void)sender;
    if (!self.state) return;
    self.state->grabReferenceRequested = true;
    self.state->overlayDirty = true;
}

- (void)forgetReferenceStill:(id)sender {
    (void)sender;
    if (!self.state) return;
    NSString* path = [CMUiPreferences.sharedPreferences referenceStillPath];
    if (path) [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    [CMUiPreferences.sharedPreferences setReferenceStillPath:nil];
    self.state->referenceStill = nullptr;
    self.state->monitorCompare.split = false;
    [self saveMonitorCompare];
}

// What Record just showed, kept as the reference: written where the
// preferences say, then shown split.
- (BOOL)keepReferenceStill:(ReferenceStill)still {
    NSBitmapImageRep* bitmap =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                pixelsWide:still.width
                                                pixelsHigh:still.height
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSDeviceRGBColorSpace
                                               bytesPerRow:still.width * 4
                                              bitsPerPixel:32];
    if (!bitmap || bitmap.bitmapData == nullptr) return NO;
    std::copy(still.rgba.begin(), still.rgba.end(), bitmap.bitmapData);
    NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                       properties:@{}];
    NSURL* url = [self referenceStillURL];
    [NSFileManager.defaultManager
               createDirectoryAtURL:url.URLByDeletingLastPathComponent
        withIntermediateDirectories:YES
                         attributes:nil
                              error:nil];
    if (!png || ![png writeToURL:url atomically:YES]) return NO;
    [CMUiPreferences.sharedPreferences setReferenceStillPath:url.path];
    self.state->referenceStill =
        std::make_shared<const ReferenceStill>(std::move(still));
    self.state->monitorCompare.split = true;
    [self saveMonitorCompare];
    return YES;
}

- (void)revealSelectedMediaInFinder:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    if (!media) return;
    std::filesystem::path path(media->path);
    if (path.is_relative()) {
        path = std::filesystem::absolute(
                   std::filesystem::path(self.documentPath.UTF8String ?: ""))
                   .parent_path() /
               path;
    }
    NSURL* url = [NSURL
        fileURLWithPath:[NSString stringWithUTF8String:path.lexically_normal()
                                                           .c_str()]];
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
}

- (void)relinkSelectedMedia:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* existing =
        self.state->document.FindLibraryMedia(mediaId);
    if (!existing) return;
    for (const auto& pending : self.state->pendingRelinks) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue =
                @"Une reconnexion est déjà en cours pour ce média.";
            return;
        }
    }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title =
        [NSString stringWithFormat:@"Reconnecter %@",
                                   CMStringFromUtf8(existing->filename)];
    panel.prompt = @"Reconnecter";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.resolvesAliases = YES;
    panel.allowedContentTypes = @[ UTTypeMovie ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::error_code pathError;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(
        std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""),
        pathError);
    if (pathError) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Fichier inaccessible : %@",
                                       CMStringFromUtf8(pathError.message())];
        return;
    }
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    // LIEN1 -- docs/roadmap/IMPORT-2026-10.md. The file as it was picked,
    // links kept: a rush picked through a link beside the package keeps it.
    const std::string stored = StoredMediaPath(
        std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""), base);
    auto result = std::make_shared<RelinkProbeResult>();
    result->media.id = mediaId;
    result->media.path = stored;
    result->media.filename = absolute.filename().string();
    const Document snapshot = self.state->document;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Relink, "Relink " + existing->filename,
        [absolute, mediaId, snapshot, result](MediaTaskContext& context,
                                              std::string& taskError) {
            if (context.Cancelled()) {
                taskError = "relink cancelled";
                return false;
            }
            context.SetProgress(0.1, "Analyse du remplacement");
            if (!ProbeMediaMetadata(absolute.string(), result->media,
                                    taskError)) {
                result->error = taskError;
                return false;
            }
            // COLOR-T -- the file moves; how its picture is read does not.
            if (const LibraryMedia* original =
                    snapshot.FindLibraryMedia(mediaId))
                KeepMediaReading(*original, result->media);
            if (context.Cancelled()) {
                taskError = "relink cancelled";
                return false;
            }
            context.SetProgress(0.8, "Vérification de compatibilité");
            if (!ValidateRelinkCandidate(snapshot, mediaId, result->media,
                                         taskError)) {
                result->error = taskError;
                return false;
            }
            context.SetProgress(1.0, "Remplacement compatible");
            return true;
        });
    self.state->pendingRelinks.emplace(
        taskId, PendingRelink{mediaId, absolute, stored, result});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Vérification de %@…",
                                   CMStringFromUtf8(absolute.filename())];
    [self refreshMediaTaskStatus];
}

- (void)batchRelinkOfflineMedia:(id)sender {
    (void)sender;
    if (!self.state || self.state->offlineSourceIds.empty() ||
        !self.state->pendingBatchRelinks.empty())
        return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Dossier contenant les médias originaux";
    panel.prompt = @"Rechercher et reconnecter";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.resolvesAliases = YES;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::error_code pathError;
    // LIEN1 -- docs/roadmap/IMPORT-2026-10.md. The folder as it was picked,
    // links kept: what the relinked rushes store (StoredMediaPath). A rush
    // found under a link beside the package keeps the link in its path.
    const std::filesystem::path folder =
        std::filesystem::absolute(
            std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""),
            pathError)
            .lexically_normal();
    if (pathError) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Dossier inaccessible : %@",
                                       CMStringFromUtf8(pathError.message())];
        return;
    }
    const Document snapshot = self.state->document;
    const std::vector<Ulid> offlineIds(self.state->offlineSourceIds.begin(),
                                       self.state->offlineSourceIds.end());
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    auto result = std::make_shared<BatchRelinkResult>();
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Relink, "Relink dossier " + folder.filename().string(),
        [folder, base, snapshot, offlineIds, result](MediaTaskContext& context,
                                                     std::string& taskError) {
            const auto keyFor = [](std::string value) {
                std::transform(
                    value.begin(), value.end(), value.begin(),
                    [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                return value;
            };
            std::map<std::string, std::vector<std::filesystem::path>> files;
            std::error_code scanError;
            std::filesystem::recursive_directory_iterator iterator(
                folder,
                std::filesystem::directory_options::skip_permission_denied,
                scanError),
                end;
            while (!scanError && iterator != end) {
                if (context.Cancelled()) {
                    taskError = "batch relink cancelled";
                    return false;
                }
                std::error_code fileError;
                if (IsVolumeSystemDirectory(iterator->path()))
                    iterator.disable_recursion_pending();
                else if (iterator->is_regular_file(fileError) && !fileError &&
                         !IsVolumeLitter(iterator->path()))
                    files[keyFor(iterator->path().filename().string())]
                        .push_back(iterator->path());
                iterator.increment(scanError);
            }
            if (scanError) {
                taskError =
                    "unable to scan relink folder: " + scanError.message();
                return false;
            }
            for (size_t index = 0; index < offlineIds.size(); ++index) {
                if (context.Cancelled()) {
                    taskError = "batch relink cancelled";
                    return false;
                }
                const LibraryMedia* original =
                    snapshot.FindLibraryMedia(offlineIds[index]);
                if (!original) continue;
                const auto found = files.find(keyFor(original->filename));
                if (found == files.end()) {
                    ++result->unmatched;
                } else if (found->second.size() != 1) {
                    ++result->ambiguous;
                } else {
                    std::error_code canonicalError;
                    const std::filesystem::path absolute =
                        std::filesystem::weakly_canonical(found->second.front(),
                                                          canonicalError);
                    if (canonicalError) {
                        ++result->incompatible;
                        result->last_error = canonicalError.message();
                    } else {
                        const std::string stored =
                            StoredMediaPath(found->second.front(), base);
                        LibraryMedia replacement;
                        replacement.id = original->id;
                        replacement.path = stored;
                        replacement.filename = absolute.filename().string();
                        std::string probeError;
                        const bool probed = ProbeMediaMetadata(
                            absolute.string(), replacement, probeError);
                        // COLOR-T -- as the single relink does.
                        if (probed) KeepMediaReading(*original, replacement);
                        if (!probed ||
                            !ValidateRelinkCandidate(snapshot, original->id,
                                                     replacement, probeError)) {
                            ++result->incompatible;
                            result->last_error = probeError;
                        } else {
                            result->replacements.push_back(
                                {original->id, std::move(replacement), stored});
                        }
                    }
                }
                context.SetProgress(
                    static_cast<double>(index + 1) / offlineIds.size(),
                    original->filename);
            }
            return true;
        });
    self.state->pendingBatchRelinks.emplace(taskId, PendingBatchRelink{result});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Recherche de %lu média%@ offline…",
                                   (unsigned long)offlineIds.size(),
                                   offlineIds.size() == 1 ? @"" : @"s"];
    [self refreshMediaTaskStatus];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    if (tableView == self.mediaTable) return self.visibleMediaIds.count;
    if (tableView == self.audioTable) return self.visibleAudioIds.count;
    if (tableView == self.captionStyleTable)
        return self.visibleCaptionStyleIds.count;
    return 0;
}

- (NSTableRowView*)tableView:(NSTableView*)tableView
               rowViewForRow:(NSInteger)row {
    (void)tableView;
    (void)row;
    return [[CMIndustrialTableRowView alloc] initWithFrame:NSZeroRect];
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    if (tableView == self.audioTable) {
        if (row < 0 || row >= (NSInteger)self.visibleAudioIds.count) return nil;
        const LibraryMedia* media = self.state->document.FindLibraryMedia(
            self.visibleAudioIds[row].UTF8String ?: "");
        NSTextField* label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        if (!media) return label;
        if ([tableColumn.identifier isEqualToString:@"name"])
            label.stringValue =
                [NSString stringWithUTF8String:media->filename.c_str()];
        else if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue = [NSString
                stringWithFormat:@"%@ · %d ch", CMStringFromUtf8(media->codec),
                                 media->audio_channels];
        else
            label.stringValue = TimeString(media->duration);
        return label;
    }
    if (tableView == self.captionStyleTable) {
        if (row < 0 || row >= (NSInteger)self.visibleCaptionStyleIds.count)
            return nil;
        const CaptionStyle* style = self.state->document.FindCaptionStyle(
            self.visibleCaptionStyleIds[row].UTF8String ?: "");
        NSTextField* label = [NSTextField labelWithString:@""];
        label.lineBreakMode = NSLineBreakByTruncatingTail;
        if (!style) return label;
        if ([tableColumn.identifier isEqualToString:@"style"]) {
            label.stringValue = [NSString
                stringWithUTF8String:ui::media_panel::DescribeCaptionStyle(
                                         *style)
                                         .c_str()];
        } else {
            const auto summaries = ui::media_panel::SummarizeCaptionStyles(
                self.state->document.sequence);
            int32_t cartons = 0;
            int32_t others = 0;
            for (const auto& summary : summaries)
                if (summary.style_id == style->id) {
                    cartons = summary.carton_count;
                    others = summary.other_clip_count;
                }
            // F2.3 -- cartons only; pictures that carry the style without
            // showing it are counted apart.
            label.stringValue =
                others > 0
                    ? [NSString stringWithFormat:@"%d (+%d)", cartons, others]
                    : [NSString stringWithFormat:@"%d", cartons];
            label.toolTip =
                others > 0
                    ? [NSString
                          stringWithFormat:@"%d plan%@ vidéo porte%@ aussi ce "
                                           @"style, sans l’afficher",
                                           others, others == 1 ? @"" : @"s",
                                           others == 1 ? @"" : @"nt"]
                    : nil;
        }
        return label;
    }
    if (tableView != self.mediaTable || row < 0 ||
        row >= (NSInteger)self.visibleMediaIds.count)
        return nil;
    NSString* identifier = self.visibleMediaIds[row];
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    if ([tableColumn.identifier isEqualToString:@"name"]) {
        NSTableCellView* cell = [[NSTableCellView alloc]
            initWithFrame:NSMakeRect(0, 0, tableColumn.width, 44.0)];
        NSImageView* image =
            [[NSImageView alloc] initWithFrame:NSMakeRect(4, 3, 60, 38)];
        image.imageScaling = NSImageScaleProportionallyUpOrDown;
        image.wantsLayer = YES;
        image.layer.cornerRadius = 0.0;
        image.layer.masksToBounds = YES;
        NSTextField* title = [[BrowserRenameTextField alloc]
            initWithFrame:NSMakeRect(72, 12,
                                     std::max(40.0, tableColumn.width - 76),
                                     20)];
        title.bezeled = NO;
        title.drawsBackground = NO;
        title.autoresizingMask = NSViewWidthSizable;
        title.lineBreakMode = NSLineBreakByTruncatingTail;
        title.editable = YES;
        title.selectable = YES;
        title.delegate = self;
        title.identifier = [@"browser:" stringByAppendingString:identifier];
        cell.imageView = image;
        cell.textField = title;
        [cell addSubview:image];
        [cell addSubview:title];
        if (bin) {
            title.stringValue =
                [NSString stringWithUTF8String:bin->name.c_str()];
            image.image = SystemSymbol(@"folder.fill", @"Chutier", 24.0);
            image.contentTintColor = CMThemeColor(ui::theme::kAccent);
        } else if (media) {
            title.stringValue =
                [NSString stringWithUTF8String:RushDisplayName(
                                                   self.state->project, *media)
                                                   .c_str()];
            const bool offline =
                self.state->offlineSourceIds.count(media->id) != 0;
            image.image = offline ? nil : self.mediaThumbnails[identifier];
            if (image.image) {
                image.contentTintColor = nil;
            } else {
                image.image = SystemSymbol(offline ? @"exclamationmark.triangle"
                                           : media->has_video ? @"film"
                                                              : @"waveform",
                                           offline ? @"Média offline"
                                           : media->has_video ? @"Média vidéo"
                                                              : @"Média audio");
                image.contentTintColor = offline
                                             ? CMThemeColor(ui::theme::kError)
                                             : CMTextSecondaryColor();
            }
        } else if (const DocumentSequence* sequence =
                       self.state->project.FindTimeline(identifier.UTF8String
                                                            ?: "")) {
            title.stringValue =
                [NSString stringWithUTF8String:sequence->name.c_str()];
            image.image =
                SystemSymbol(@"rectangle.stack.fill", @"Séquence", 22.0);
            image.contentTintColor = CMAccentColor();
        }
        return cell;
    }
    NSTextField* label = [NSTextField labelWithString:@""];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    if (bin) {
        if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue = @"Chutier";
        else
            label.stringValue = @"—";
        return label;
    }
    if (!media) {
        const DocumentSequence* found =
            self.state->project.FindTimeline(identifier.UTF8String ?: "");
        if (!found) return label;
        const DocumentSequence& sequence = *found;
        if ([tableColumn.identifier isEqualToString:@"name"])
            label.stringValue = [NSString
                stringWithFormat:@"%@◫ %@",
                                 sequence.id == self.state->activeTimelineId
                                     ? @"● "
                                     : @"",
                                 CMStringFromUtf8(sequence.name)];
        else if ([tableColumn.identifier isEqualToString:@"format"])
            label.stringValue =
                [NSString stringWithFormat:@"Séquence %dx%d", sequence.width,
                                           sequence.height];
        else {
            Document timelineDocument =
                self.state->project.MakeDocument(sequence.id);
            timelineDocument.sequence = sequence;
            label.stringValue =
                TimeString(Timeline(timelineDocument).Duration());
        }
        return label;
    }
    if ([tableColumn.identifier isEqualToString:@"name"])
        label.stringValue = [NSString
            stringWithFormat:@"%@%@", media->proxy_path.empty() ? @"" : @"P · ",
                             CMStringFromUtf8(media->filename)];
    else if ([tableColumn.identifier isEqualToString:@"format"])
        label.stringValue =
            self.state->offlineSourceIds.count(media->id) ? @"Média offline"
            : media->metadata_complete && !media->has_video
                ? [NSString stringWithFormat:@"Audio · %@ · %d ch",
                                             CMStringFromUtf8(media->codec),
                                             media->audio_channels]
            : media->metadata_complete
                ? [NSString stringWithFormat:@"%@%@ %dx%d",
                                             media->proxy_path.empty()
                                                 ? @""
                                                 : @"Proxy · ",
                                             CMStringFromUtf8(media->codec),
                                             media->width, media->height]
                : @"—";
    else
        label.stringValue =
            [NSString stringWithFormat:@"%@", TimeString(media->duration)];
    return label;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    if (notification.object == self.captionStyleTable) {
        [self updateCaptionSelectionLabel];
        return;
    }
    if (notification.object != self.mediaTable) return;
    (void)[self selectedMediaId];
}

- (void)collectionView:(NSCollectionView*)collectionView
    didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths {
    (void)collectionView;
    NSIndexPath* path = indexPaths.anyObject;
    if (path && path.item < self.visibleMediaIds.count) {
        NSString* identifier = self.visibleMediaIds[path.item];
        if (self.state->document.FindBin(identifier.UTF8String ?: "")) {
            self.selectedBinId = identifier;
            const NSInteger row = [self.binOutline rowForItem:identifier];
            if (row >= 0) {
                self.updatingBinControls = YES;
                [self.binOutline
                        selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                    byExtendingSelection:NO];
                self.updatingBinControls = NO;
            }
            [self rebuildMediaList];
            return;
        }
    }
    (void)[self selectedMediaId];
}

- (void)mediaSearchChanged:(id)sender {
    (void)sender;
    [self rebuildMediaList];
}

- (void)importMediaPressed:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Importer des rushes";
    panel.prompt = @"Importer";
    panel.allowsMultipleSelection = YES;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.resolvesAliases = YES;
    NSString* target =
        self.state->document.FindBin(self.selectedBinId.UTF8String ?: "")
            ? self.selectedBinId
            : @"";
    if ([panel runModal] == NSModalResponseOK)
        [self importMediaURLs:panel.URLs intoBin:target];
}

- (void)importSubtitlesPressed:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Importer des sous-titres SubRip";
    panel.prompt = @"Importer";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowedContentTypes = @[ [UTType typeWithFilenameExtension:@"srt"] ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::vector<SubtitleCue> cues;
    std::string message;
    if (!LoadSrt(panel.URL.fileSystemRepresentation ?: "", cues, message)) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Import SRT impossible";
        alert.informativeText =
            [NSString stringWithUTF8String:message.c_str()] ?: @"SRT invalide";
        [alert runModal];
        return;
    }
    int32_t trackIndex = 0;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        trackIndex = std::max(trackIndex, track.index + 1);
    EditError error = EditError::None;
    AddTrackOperation operation = BuildSubtitleTrackEdit(cues, trackIndex);
    if (![self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                          error:error
                                        message:message]) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Import SRT impossible";
        alert.informativeText = [NSString stringWithUTF8String:message.c_str()]
                                    ?: @"Erreur inconnue";
        [alert runModal];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%lu sous-titre%@ importé%@",
                                   (unsigned long)cues.size(),
                                   cues.size() == 1 ? @"" : @"s",
                                   cues.size() == 1 ? @"" : @"s"];
}

- (void)exportSubtitlesPressed:(id)sender {
    (void)sender;
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.title = @"Exporter les sous-titres SubRip";
    panel.prompt = @"Exporter";
    panel.nameFieldStringValue = @"sous-titres.srt";
    panel.allowedContentTypes = @[ [UTType typeWithFilenameExtension:@"srt"] ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    std::string message;
    if (!SaveSrt(self.state->document, panel.URL.fileSystemRepresentation ?: "",
                 message)) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Export SRT impossible";
        alert.informativeText = [NSString stringWithUTF8String:message.c_str()]
                                    ?: @"Erreur inconnue";
        [alert runModal];
        return;
    }
    self.infoLabel.stringValue = @"Sous-titres SRT exportés";
}

- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId {
    const std::string targetBin(binId.UTF8String ?: "");
    if (!targetBin.empty() && !self.state->document.FindBin(targetBin))
        return NO;

    std::vector<std::filesystem::path> candidates;
    for (NSURL* url in urls) {
        if (!url.isFileURL) continue;
        std::filesystem::path path(url.path.UTF8String ?: "");
        std::error_code error;
        if (std::filesystem::is_directory(path, error) && !error) {
            std::filesystem::recursive_directory_iterator iterator(
                path,
                std::filesystem::directory_options::skip_permission_denied,
                error),
                end;
            for (; !error && iterator != end; iterator.increment(error)) {
                if (IsVolumeSystemDirectory(iterator->path())) {
                    iterator.disable_recursion_pending();
                    continue;
                }
                if (iterator->is_regular_file(error) && !error &&
                    !IsVolumeLitter(iterator->path()))
                    candidates.push_back(iterator->path());
            }
        } else if (!error && std::filesystem::is_regular_file(path, error)) {
            candidates.push_back(path);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.empty()) {
        self.binSummaryLabel.stringValue = @"Aucun fichier à importer.";
        return NO;
    }

    auto batch = std::make_shared<ProbedImportBatch>();
    batch->document_path = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    batch->target_bin = targetBin;
    // PRD7 -- docs/roadmap/PRODUIT-2026-10.md. The engine's ingest reads the
    // files (ProbeIngestFile: headers, then the audio level, which decodes
    // the sound), here on the task's thread. The library is read now, on the
    // main thread, for the files it already holds complete: nothing is
    // decoded for those. Cancelling is heard between two files, not inside
    // one measurement. F2.4 -- the same reading as an agent's ingest_media
    // (ProbeIngestFiles, McpLiveBackend.h).
    const std::set<std::string> complete = CompleteLibraryPaths(
        self.state->document, batch->document_path.parent_path());
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Probe, IngestTaskLabel(candidates.size()),
        [batch, candidates, complete](MediaTaskContext& context,
                                      std::string& error) {
            if (ProbeIngestFiles(candidates, complete, batch->files,
                                 &context)) {
                return true;
            }
            error = "import cancelled";
            return false;
        });
    self.state->pendingImports[taskId] = std::move(batch);
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Analyse de %lu fichier%@ en arrière-plan…",
                                   (unsigned long)candidates.size(),
                                   candidates.size() == 1 ? @"" : @"s"];
    [self refreshMediaTaskStatus];
    return YES;
}

- (void)generateSelectedMediaProxy:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    [self enqueueProxyForMediaIdentifier:identifier];
}

- (void)transcribeSelectedMedia:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier || !self.state) return;

    NSString* modelPath = [self transcriptionModelPath];
    if (!modelPath) return;
    [self enqueueTranscriptionForMediaIdentifier:identifier
                                       modelPath:modelPath
                                         batchId:Ulid{}];
}

- (void)transcribeTimeline:(id)sender {
    (void)sender;
    if (!self.state) return;
    NSString* modelPath = [self transcriptionModelPath];
    if (!modelPath) return;

    const bool hasSolo =
        std::any_of(self.state->document.sequence.tracks.begin(),
                    self.state->document.sequence.tracks.end(),
                    [](const DocumentTrack& track) {
                        return track.kind == "audio" && track.solo;
                    });
    std::set<Ulid> mediaIds;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            // A disabled clip is not heard (ClipEnabled.h).
            if (!ClipIsHeard(clip)) continue;
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(clip.source_id);
            if (media && media->has_audio) mediaIds.insert(clip.source_id);
        }
    }
    if (mediaIds.empty()) {
        self.binSummaryLabel.stringValue =
            @"La timeline ne contient aucune piste audio active.";
        return;
    }
    for (const auto& pending : self.state->pendingTranscriptions) {
        if (mediaIds.count(pending.second.media_id)) {
            self.binSummaryLabel.stringValue =
                @"Une source de cette timeline est déjà en transcription.";
            return;
        }
    }

    const Ulid batchId = GenerateUlid();
    PendingTimelineTranscription batch;
    batch.timeline_id = self.state->activeTimelineId;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path transcriptDirectory =
        projectPath.parent_path() / ".cutmachine" / "transcripts";
    WhisperSettings requestedSettings;
    requestedSettings.whisper_model_path = modelPath.UTF8String ?: "";
    for (const Ulid& mediaId : mediaIds) {
        const std::filesystem::path cachedPath =
            transcriptDirectory / (mediaId + ".json");
        Transcript cached;
        std::string cacheError;
        if (LoadAudioTranscript(cachedPath.string(), cached, cacheError) &&
            TranscriptCacheMatches(cached, mediaId, requestedSettings)) {
            batch.transcript_paths.emplace(mediaId, cachedPath);
            continue;
        }
        NSString* identifier = [NSString stringWithUTF8String:mediaId.c_str()];
        const Ulid taskId =
            [self enqueueTranscriptionForMediaIdentifier:identifier
                                               modelPath:modelPath
                                                 batchId:batchId];
        if (taskId.empty()) {
            batch.error = "unable to enqueue every timeline source";
            break;
        }
        batch.task_ids.insert(taskId);
        batch.transcript_paths.emplace(
            mediaId,
            self.state->pendingTranscriptions.at(taskId).absolute_path);
    }
    if (batch.transcript_paths.empty()) {
        self.binSummaryLabel.stringValue =
            @"Impossible de démarrer la transcription de la timeline.";
        return;
    }
    const bool cachedOnly = batch.task_ids.empty();
    self.state->pendingTimelineTranscriptions.emplace(batchId,
                                                      std::move(batch));
    self.binSummaryLabel.stringValue =
        cachedOnly
            ? @"Transcripts en cache · création de S1…"
            : [NSString stringWithFormat:@"Transcription de la timeline · %lu "
                                          "source%@…",
                                         (unsigned long)mediaIds.size(),
                                         mediaIds.size() == 1 ? @"" : @"s"];
}

- (Ulid)enqueueTranscriptionForMediaIdentifier:(NSString*)identifier
                                     modelPath:(NSString*)modelPath
                                       batchId:(const Ulid&)batchId {
    if (!identifier || !modelPath || !self.state) return {};
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!media || !source || !media->has_audio) return {};
    for (const auto& pending : self.state->pendingTranscriptions) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue =
                @"Une transcription est déjà en cours pour ce média.";
            return {};
        }
    }

    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path output =
        base / ".cutmachine" / "transcripts" / (mediaId + ".json");
    WhisperSettings settings;
    settings.whisper_model_path = modelPath.UTF8String ?: "";
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Transcription, "Whisper " + media->filename,
        [input, output, mediaId, rate = source->rate, settings](
            MediaTaskContext& context, std::string& taskError) {
            return GenerateAudioTranscript(input.string(), output.string(),
                                           mediaId, rate, settings, context,
                                           taskError);
        });
    self.state->pendingTranscriptions.emplace(
        taskId, PendingTranscription{mediaId, self.state->activeTimelineId,
                                     batchId, output});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Transcription de %@ en arrière-plan…",
                                   CMStringFromUtf8(media->filename)];
    [self refreshMediaTaskStatus];
    return taskId;
}

- (void)regenerateSelectedMediaThumbnail:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    [self.mediaThumbnails removeObjectForKey:identifier];
    [self enqueueThumbnailForMediaIdentifier:identifier];
}

- (void)enqueueProxyForMediaIdentifier:(NSString*)identifier {
    if (!identifier) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!media || !source) return;
    if (!media->has_video) {
        self.binSummaryLabel.stringValue =
            @"Ce média est audio seul · aucun proxy vidéo à générer.";
        return;
    }
    for (const auto& pending : self.state->pendingProxies) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue = @"Un proxy est déjà en cours.";
            return;
        }
    }
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path output =
        base / ".cutmachine" / "proxies" / (mediaId + ".mov");
    std::error_code error;
    std::string stored = std::filesystem::relative(output, base, error)
                             .lexically_normal()
                             .string();
    if (error) stored = output.string();
    ProxySettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Proxy, "Proxy " + media->filename,
        [input, output, duration = source->duration, settings](
            MediaTaskContext& context, std::string& taskError) {
            return GenerateMediaProxy(input.string(), output.string(), duration,
                                      settings, context, taskError);
        });
    self.state->pendingProxies.emplace(
        taskId, PendingProxy{mediaId, output, std::move(stored)});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Génération du proxy de %@…",
                                   CMStringFromUtf8(media->filename)];
    [self refreshMediaTaskStatus];
}

- (void)removeSelectedMediaProxy:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    const Ulid mediaId(identifier.UTF8String ?: "");
    LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    if (!media || media->proxy_path.empty()) return;
    for (const auto& pending : self.state->pendingProxies)
        if (pending.second.media_id == mediaId)
            self.state->mediaTasks->Cancel(pending.first);
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    const std::string previousProxyPath = media->proxy_path;
    std::filesystem::path stored(previousProxyPath);
    if (stored.is_relative()) stored = base / stored;
    const std::filesystem::path expected =
        base / ".cutmachine" / "proxies" / (mediaId + ".mov");
    media->proxy_path.clear();
    std::string message;
    if (![self persistEdits:message]) {
        media->proxy_path = previousProxyPath;
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Suppression du proxy impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    if (stored.lexically_normal() == expected.lexically_normal()) {
        std::error_code ignored;
        std::filesystem::remove(expected, ignored);
    }
    [self reloadDecodeWorkers];
    [self rebuildMediaList];
    self.binSummaryLabel.stringValue = @"Proxy supprimé · original actif";
}

- (void)menuToggleProxies:(id)sender {
    (void)sender;
    self.state->proxiesEnabled = !self.state->proxiesEnabled;
    [self reloadDecodeWorkers];
    self.infoLabel.stringValue =
        self.state->proxiesEnabled ? @"Proxies activés" : @"Originaux activés";
}

- (void)menuToggleAutomaticProxies:(id)sender {
    (void)sender;
    self.state->automaticProxiesEnabled = !self.state->automaticProxiesEnabled;
    [NSUserDefaults.standardUserDefaults
        setBool:self.state->automaticProxiesEnabled
         forKey:kAutomaticProxyGenerationDefaultsKey];
    self.infoLabel.stringValue =
        self.state->automaticProxiesEnabled
            ? @"Génération automatique des proxies activée"
            : @"Génération automatique des proxies désactivée";
}

- (void)createBinPressed:(id)sender {
    (void)sender;
    const std::string parent =
        self.state->document.FindBin(self.selectedBinId.UTF8String ?: "")
            ? std::string(self.selectedBinId.UTF8String ?: "")
            : std::string{};
    std::string name = "Nouveau chutier";
    for (int suffix = 2;; ++suffix) {
        const bool exists = std::any_of(
            self.state->document.bins.begin(), self.state->document.bins.end(),
            [&](const DocumentBin& bin) {
                return bin.parent_id == parent && bin.name == name;
            });
        if (!exists) break;
        name = "Nouveau chutier " + std::to_string(suffix);
    }
    const Ulid binId = GenerateUlid();
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            AddBinOperation { binId, name, parent }
        }
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Création refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    NSString* identifier = [NSString stringWithUTF8String:binId.c_str()];
    [self refreshBinControlsSelecting:identifier];
    [self beginEditingBin:identifier];
}

- (void)deleteBinPressed:(id)sender {
    (void)sender;
    const std::string binId(self.selectedBinId.UTF8String ?: "");
    if (!self.state->document.FindBin(binId)) return;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveBinOperation { binId, "", "" }
        }
                                          error:error
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshBinControlsSelecting:@"__all__"];
}

- (void)renameBinPressed:(id)sender {
    (void)sender;
    if (!self.state->document.FindBin(self.selectedBinId.UTF8String ?: ""))
        return;
    [self beginEditingBin:self.selectedBinId];
}

- (void)beginEditingBin:(NSString*)binId {
    dispatch_async(dispatch_get_main_queue(), ^{
      const NSInteger row = [self.binOutline rowForItem:binId];
      if (row < 0) return;
      [self.binOutline selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                   byExtendingSelection:NO];
      [self.binOutline editColumn:0 row:row withEvent:nil select:YES];
    });
}

- (void)controlTextDidEndEditing:(NSNotification*)notification {
    NSTextField* field = notification.object;
    if (![field isKindOfClass:NSTextField.class]) return;
    const BOOL sidebarRename = [field.identifier hasPrefix:@"bin:"];
    const BOOL browserRename = [field.identifier hasPrefix:@"browser:"];
    if (!sidebarRename && !browserRename) return;
    const NSUInteger prefixLength = sidebarRename ? 4 : 8;
    NSString* identifier = [field.identifier substringFromIndex:prefixLength];
    const NSInteger movement =
        [notification.userInfo[NSTextMovementUserInfoKey] integerValue];
    if (movement == NSCancelTextMovement) {
        [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
        return;
    }
    const DocumentBin* bin =
        self.state->document.FindBin(identifier.UTF8String ?: "");
    NSString* name = [field.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    const DocumentSequence* timeline =
        self.state->project.FindTimeline(identifier.UTF8String ?: "");
    const LibraryMedia* media =
        self.state->document.FindLibraryMedia(identifier.UTF8String ?: "");
    const std::string currentName =
        bin        ? bin->name
        : timeline ? timeline->name
        : media    ? RushDisplayName(self.state->project, *media)
                   : std::string{};
    if (name.length == 0 || currentName.empty() ||
        currentName == (name.UTF8String ?: "")) {
        [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (bin) {
        if (![self applyAndPersistTimelineOperation:Operation {
                RenameBinOperation {
                    identifier.UTF8String ?: "", name.UTF8String ?: ""
                }
            }
                                              error:error
                                            message:message]) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Renommage refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
    } else {
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Apply(
                candidate,
                ProjectOperation{RenameProjectItemOperation{
                    identifier.UTF8String ?: "", name.UTF8String ?: ""}},
                error, message) ||
            ![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Renommage refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        if (identifier.UTF8String &&
            self.state->activeTimelineId == identifier.UTF8String) {
            self.state->document =
                self.state->project.MakeDocument(self.state->activeTimelineId);
            self.programMonitorTitleLabel.stringValue =
                CMRecordTitle(self.state->document.sequence.name);
            self.window.title = CMWindowTitle(
                self.state->project.name, self.state->document.sequence.name);
        }
        self.state->overlayDirty = true;
    }
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
}

- (void)assignMediaToBinPressed:(id)sender {
    (void)sender;
    [self moveMediaIdsToBin:[self selectedMediaIds]];
}

// Shared "choose a destination bin, then apply SetMediaBinOperation to every
// id" flow. Factored out of -assignMediaToBinPressed so F2.3's Audio tab
// (ROADMAP.md) -- which lists a different filtered slice of the same
// document.library, see MediaPanelModel.h -- can offer the identical
// "Déplacer vers un chutier" gesture without a second alert/menu
// implementation.
- (void)moveMediaIdsToBin:(NSArray<NSString*>*)mediaIdentifiers {
    NSMutableArray<NSString*>* mediaIds = [NSMutableArray array];
    for (NSString* identifier in mediaIdentifiers)
        if (self.state->document.FindLibraryMedia(identifier.UTF8String ?: ""))
            [mediaIds addObject:identifier];
    if (mediaIds.count == 0) return;
    const LibraryMedia* selected = self.state->document.FindLibraryMedia(
        mediaIds.firstObject.UTF8String ?: "");

    NSPopUpButton* destinations =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 26)];
    destinations.identifier = @"media.move.destination";
    [destinations addItemWithTitle:@"Sans chutier"];
    destinations.lastItem.representedObject = @"";
    std::vector<const DocumentBin*> bins;
    for (const DocumentBin& bin : self.state->document.bins)
        bins.push_back(&bin);
    std::stable_sort(bins.begin(), bins.end(),
                     [](const DocumentBin* left, const DocumentBin* right) {
                         return left->name < right->name;
                     });
    for (const DocumentBin* bin : bins) {
        std::string path = bin->name;
        const DocumentBin* parent =
            self.state->document.FindBin(bin->parent_id);
        while (parent) {
            path = parent->name + " / " + path;
            parent = self.state->document.FindBin(parent->parent_id);
        }
        [destinations
            addItemWithTitle:[NSString stringWithUTF8String:path.c_str()]];
        destinations.lastItem.representedObject =
            [NSString stringWithUTF8String:bin->id.c_str()];
        if (bin->id == selected->bin_id)
            [destinations selectItem:destinations.lastItem];
    }
    if (selected->bin_id.empty()) [destinations selectItemAtIndex:0];
    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel.
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:mediaIds.count == 1 ? @"Déplacer le rush"
                                          : @"Déplacer les rushes"
              message:[NSString
                          stringWithFormat:@"Choisissez le chutier de "
                                           @"destination pour %lu "
                                           @"rush%@.",
                                           (unsigned long)mediaIds.count,
                                           mediaIds.count == 1 ? @"" : @"es"]
                 form:destinations];
    [panel addButtonWithTitle:@"Déplacer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = destinations;
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  [self moveMediaIds:mediaIds
                               toBin:destinations.selectedItem.representedObject
                                         ?: @""];
                }];
}

- (void)moveMediaIds:(NSArray<NSString*>*)mediaIds toBin:(NSString*)bin {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    for (NSString* media in mediaIds) {
        if (!stagedLog.Apply(stagedDocument,
                             Operation{SetMediaBinOperation{
                                 media.UTF8String ?: "", bin.UTF8String ?: ""}},
                             error, message)) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Classement refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
    }
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    [self refreshBinControlsSelecting:bin.length == 0 ? @"__root__" : bin];
}

- (MediaRate)playheadFrameRate {
    return PlayheadFrameRate(self.state->document);
}

// PRD27 -- what VoiceOver reads as the timeline's value (UiAccessibility.h):
// the playhead at the rate its timecode is shown, the selected clips by the
// names the timeline draws on them, the clicked one first.
- (NSString*)timelineAccessibilityValue {
    if (!self.state || !self.state->interaction) return nil;
    std::vector<ui::accessibility::SelectedClip> clips;
    const auto add = [&](const Ulid& clipId) {
        const DocumentClip* clip = self.state->document.FindClip(clipId);
        if (clip == nullptr) return;
        clips.push_back(
            {TimelineDrawnClipName(self.state->document, self.state->project,
                                   clip, clip->source_id),
             clip->link_group_id});
    };
    const Ulid& primary = self.state->interaction->PrimarySelectedClipId();
    if (!primary.empty()) add(primary);
    for (const Ulid& clipId : self.state->interaction->SelectedClipIds())
        if (clipId != primary) add(clipId);
    return CMStringFromUtf8(ui::accessibility::TimelineValue(
        self.state->requestedPosition, [self playheadFrameRate],
        ui::accessibility::SelectedShots(clips)));
}

- (int32_t)playheadInputRate {
    return self.state->playheadResolution == PlayheadResolution::Sample
               ? 48000
               : [self playheadFrameRate].num;
}

- (double)timelineHeight {
    return self.metalView.bounds.size.height;
}

- (double)videoHeight {
    return 0.0;
}

- (NSPoint)timelinePointForEvent:(NSEvent*)event {
    const NSPoint point = [self.metalView convertPoint:event.locationInWindow
                                              fromView:nil];
    return NSMakePoint(point.x, self.metalView.bounds.size.height - point.y -
                                    [self videoHeight]);
}

- (TimelineTool)effectiveTool {
    return self.state->spaceHand ? TimelineTool::Hand : self.state->tool;
}

// FC2 -- docs/roadmap/FILMCRAFT-2026-10.md. What is under the pointer, for
// the hover and the click alike (TimelineHitAt, TimelineView.h).
- (TimelinePointerTarget)timelineTargetAt:(NSPoint)point event:(NSEvent*)event {
    TimelinePointerTool tool = TimelinePointerTool::Select;
    switch ([self effectiveTool]) {
        case TimelineTool::Select:
            tool = TimelinePointerTool::Select;
            break;
        case TimelineTool::Hand:
            tool = TimelinePointerTool::Hand;
            break;
        case TimelineTool::Zoom:
            tool = TimelinePointerTool::Zoom;
            break;
        case TimelineTool::Cut:
            tool = TimelinePointerTool::Cut;
            break;
        case TimelineTool::Trim:
            tool = TimelinePointerTool::Trim;
            break;
    }
    const NSEventModifierFlags flags = event.modifierFlags;
    const TimelinePointerModifiers modifiers{
        (flags & NSEventModifierFlagShift) != 0,
        (flags & NSEventModifierFlagOption) != 0,
        (flags & NSEventModifierFlagCommand) != 0};
    return TimelineHitAt(self.state->document, self.state->viewport, point.x,
                         point.y, self.metalView.bounds.size.width,
                         [self timelineHeight], self.state->duration, tool,
                         modifiers, self.state->retimeClipIds);
}

- (void)applyToolCursor {
    switch ([self effectiveTool]) {
        case TimelineTool::Select:
            [NSCursor.arrowCursor set];
            break;
        case TimelineTool::Hand:
            [(self.state->navigationDragging ? NSCursor.closedHandCursor
                                             : NSCursor.openHandCursor) set];
            break;
        case TimelineTool::Zoom:
            [NSCursor.crosshairCursor set];
            break;
        case TimelineTool::Cut:
            [NSCursor.crosshairCursor set];
            break;
        case TimelineTool::Trim:
            [NSCursor.resizeLeftRightCursor set];
            break;
    }
}

- (void)setTimelineTool:(TimelineTool)tool {
    self.state->tool = tool;
    for (NSInteger index = 0; index < self.timelineToolIcons.count; ++index) {
        self.timelineToolIcons[index].contentTintColor =
            index == static_cast<NSInteger>(tool) ? CMAccentColor()
                                                  : NSColor.secondaryLabelColor;
    }
    self.state->interaction->CancelDrag();
    self.state->navigationDragging = false;
    self.state->scrubDragging = false;
    self.state->editDragging = false;
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    // FC4 -- the trim mode's style follows the tool: Selection resizes,
    // Trim ripples. A dynamic trim keeps the style it started with.
    if (!self.state->trimSession.Dynamic().active) [self syncTrimStyle];
    [self applyToolCursor];
    [self refreshTrimBar];
    [self updateSelectionInfo];
    if (self.state->trimSession.Active()) [self showTrimStatus];
    self.state->overlayDirty = true;
}

// Moves the playhead by `amount` frames (or samples, in Sample resolution)
// from its current position. Shared by the Left/Right-arrow keyboard
// shortcut and the Transport panel's frame-step buttons (F2.5) so both
// paths compute the same delta and land on the same
// -requestTimelinePosition: -> QuantizePlayheadPosition machinery every
// other playhead move already uses.
- (void)zoomTimelineAtPlayheadOut:(BOOL)zoomOut {
    self.state->zoomAnchor = ZoomTimelineAtPlayhead(
        self.state->viewport, self.state->requestedPosition,
        zoomOut ? (1.0 / 1.5) : 1.5, self.state->duration,
        self.metalView.bounds.size.width, self.state->zoomAnchor);
    self.state->overlayDirty = true;
}

// Shift+Left/Right, as in Resolve (21.1 manual, p. 870): one timecode second
// of frames, or 48,000 samples when the playhead is on the sample grid.
- (void)stepPlayheadSeconds:(int64_t)direction {
    const int64_t amount =
        self.state->playheadResolution == PlayheadResolution::Sample
            ? 48000
            : NominalTimecodeFps([self playheadFrameRate]);
    [self stepPlayheadFrames:direction * amount];
}

- (void)stepPlayheadFrames:(int64_t)amount {
    const RationalTime delta =
        self.state->playheadResolution == PlayheadResolution::Sample
            ? RationalTime{amount, 48000}
            : RationalTime{amount * [self playheadFrameRate].den,
                           [self playheadFrameRate].num};
    [self requestTimelinePosition:self.state->requestedPosition.add(delta)];
}

- (void)requestTimelinePosition:(RationalTime)position {
    if (position < RationalTime{0, 1}) position = {0, position.rate};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) {
        if (self.state->playheadResolution == PlayheadResolution::Sample) {
            quantized = {self.state->duration.to_frames(48000), 48000};
        } else {
            const MediaRate rate = [self playheadFrameRate];
            quantized = {
                self.state->duration.to_frames(rate.num, rate.den) * rate.den,
                rate.num};
        }
    }
    [self requestResolvedPosition:quantized];
    if (self.state->playbackDirection == 0 && self.state->audioPlayback) {
        std::string error;
        if (!self.state->audioPlayback->ScrubAt(quantized, error))
            std::fprintf(stderr, "Audio scrub failed: %s\n", error.c_str());
    }
    self.state->overlayDirty = true;
}

- (BOOL)timelineDropMedia:(NSString*)mediaId atViewPoint:(NSPoint)point {
    return [self timelineDropMediaIds:mediaId ? @[ mediaId ] : @[]
                          atViewPoint:point];
}

// EF1 -- docs/roadmap/EFFETS-2026-10.md. Where an effect over the timeline
// would land: the track and the time under the pointer, handed to
// EffectsLibrary.h, which picks the cut.
- (ui::effects::DropTarget)effectDropTargetForId:(NSString*)effectId
                                     atViewPoint:(NSPoint)point {
    ui::effects::DropTarget target;
    const ui::effects::Effect* effect =
        ui::effects::FindEffect(effectId.UTF8String ?: "");
    if (effect == nullptr || !self.state) {
        target.refusal = "Effet inconnu";
        return target;
    }
    const double timelineY =
        self.metalView.bounds.size.height - point.y - [self videoHeight];
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger trackIndex =
        timelineY < kTimelineRulerHeight
            ? -1
            : static_cast<NSInteger>((timelineY - kTimelineRulerHeight) /
                                     self.state->viewport.track_height);
    if (point.x < self.state->viewport.header_width || trackIndex < 0 ||
        trackIndex >= static_cast<NSInteger>(tracks.size())) {
        target.refusal = "Déposez l’effet sur une piste";
        return target;
    }
    try {
        const MediaRate rate = self.state->document.sequence.frame_rate;
        RationalTime time = self.state->viewport.XToTime(point.x, rate.num);
        if (time.value < 0) time = {0, time.rate};
        return ui::effects::FindDropTarget(self.state->document, *effect,
                                           tracks[trackIndex]->id, time);
    } catch (const std::exception& exception) {
        target.refusal = exception.what();
        return target;
    }
}

- (NSDragOperation)timelineDragEffectId:(NSString*)effectId
                            atViewPoint:(NSPoint)point {
    const ui::effects::DropTarget target = [self effectDropTargetForId:effectId
                                                           atViewPoint:point];
    self.state->effectDropTarget = target;
    self.state->overlayDirty = true;
    if (!target.valid) {
        self.infoLabel.stringValue = CMStringFromUtf8(target.refusal);
        return NSDragOperationNone;
    }
    return NSDragOperationCopy;
}

- (void)timelineEffectDragEnded {
    if (!self.state || !self.state->effectDropTarget) return;
    self.state->effectDropTarget.reset();
    self.state->overlayDirty = true;
}

// One drop, one operation, one undo: the one EffectsLibrary.h builds.
- (BOOL)applyEffectWithId:(NSString*)effectId
                 toTarget:(const ui::effects::DropTarget&)target {
    const ui::effects::Effect* effect =
        ui::effects::FindEffect(effectId.UTF8String ?: "");
    if (effect == nullptr) return NO;
    ui::effects::DropEdit edit =
        ui::effects::EditForDrop(self.state->document, *effect, target);
    if (!edit.operation) {
        self.infoLabel.stringValue = CMStringFromUtf8(edit.refusal);
        return NO;
    }
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(*edit.operation)
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@ refusé : %@", CMStringFromUtf8(effect->title),
                             CMStringFromUtf8(message)];
        return NO;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    // EF3 -- the Inspector shows what the drop set: a blurred clip's
    // « Flou », a cut's « Fondu ».
    [self updateSelectionInfo];
    self.infoLabel.stringValue = CMStringFromUtf8(edit.summary);
    self.state->overlayDirty = true;
    return YES;
}

- (BOOL)timelineDropEffectId:(NSString*)effectId atViewPoint:(NSPoint)point {
    const ui::effects::DropTarget target = [self effectDropTargetForId:effectId
                                                           atViewPoint:point];
    [self timelineEffectDragEnded];
    if (!target.valid) {
        self.infoLabel.stringValue = CMStringFromUtf8(target.refusal);
        return NO;
    }
    return [self applyEffectWithId:effectId toTarget:target];
}

// EF1 -- a double click in the list: the cut nearest the playhead, on the
// selected clip's track when it takes the effect, otherwise on the first
// track that has a clip there and does.
- (void)effectsPanel:(CMEffectsPanelView*)panel
    applyEffectWithId:(NSString*)effectId {
    (void)panel;
    const ui::effects::Effect* effect =
        ui::effects::FindEffect(effectId.UTF8String ?: "");
    if (effect == nullptr || !self.state) return;
    const RationalTime playhead = self.state->requestedPosition;
    std::vector<Ulid> candidates;
    const Ulid selected = self.state->interaction->SelectedClipId();
    // EF3 -- a clip effect goes on the selected clip when it takes it,
    // wherever the playhead is; otherwise on the clip under the playhead,
    // the topmost picture track first (below).
    if (ui::effects::IsClipEffect(*effect) && !selected.empty()) {
        const ui::effects::DropTarget target = ui::effects::ClipEffectTarget(
            self.state->document, *effect, selected);
        if (target.valid) {
            [self applyEffectWithId:effectId toTarget:target];
            return;
        }
    }
    if (const DocumentTrack* track =
            selected.empty() ? nullptr
                             : self.state->document.FindTrackForClip(selected))
        candidates.push_back(track->id);
    for (const DocumentTrack* track :
         TimelineTracksInDisplayOrder(self.state->document))
        candidates.push_back(track->id);
    std::string refusal = "Aucun raccord sous la tête de lecture";
    for (const Ulid& trackId : candidates) {
        const ui::effects::DropTarget target = ui::effects::FindDropTarget(
            self.state->document, *effect, trackId, playhead);
        if (target.valid) {
            [self applyEffectWithId:effectId toTarget:target];
            return;
        }
        if (trackId == candidates.front()) refusal = target.refusal;
    }
    self.infoLabel.stringValue = CMStringFromUtf8(refusal);
}

- (BOOL)timelineDropMediaIds:(NSArray<NSString*>*)mediaIds
                 atViewPoint:(NSPoint)point {
    if (mediaIds.count == 0) return NO;
    const double timelineY =
        self.metalView.bounds.size.height - point.y - [self videoHeight];
    if (point.x < self.state->viewport.header_width ||
        timelineY < kTimelineRulerHeight)
        return NO;
    // Out of lot -- the author, 29 October 2026. Resolve's drop (manual 21.1,
    // p. 891-893): an overwrite, Cmd+Shift an insert, Option the picture
    // alone, Shift the sound alone. It was always an insert, which opened
    // every sync-locked track: a cutaway dropped over an interview pushed
    // the interview along.
    const NSEventModifierFlags modifiers = NSEvent.modifierFlags;
    const bool insert = (modifiers & NSEventModifierFlagCommand) != 0 &&
                        (modifiers & NSEventModifierFlagShift) != 0;
    const bool pictureOnly = (modifiers & NSEventModifierFlagOption) != 0;
    const bool soundOnly =
        !insert && (modifiers & NSEventModifierFlagShift) != 0;
    // What each media brings: its picture unless it is known to have none,
    // its sound when it is known to have one.
    const auto partsOf = [&](const Ulid& sourceId) {
        const auto detected = self.state->mediaMetadata.find(sourceId);
        const LibraryMedia* media =
            detected == self.state->mediaMetadata.end()
                ? self.state->document.FindLibraryMedia(sourceId)
                : &detected->second;
        const bool known = media && media->metadata_complete;
        return std::make_pair(!(known && !media->has_video) && !soundOnly,
                              known && media->has_audio && !pictureOnly);
    };
    bool anyPicture = false;
    bool anySound = false;
    for (NSString* mediaId in mediaIds) {
        const auto [picture, sound] = partsOf(Ulid(mediaId.UTF8String ?: ""));
        anyPicture = anyPicture || picture;
        anySound = anySound || sound;
    }
    std::string refusal;
    const std::optional<TimelineDropTracks> targets =
        TimelineDropTracksAt(self.state->document, self.state->viewport,
                             timelineY, anyPicture, anySound, refusal);
    if (!targets) {
        if (!refusal.empty())
            self.binSummaryLabel.stringValue = CMStringFromUtf8(refusal);
        return NO;
    }
    const Ulid firstSourceId(mediaIds.firstObject.UTF8String ?: "");
    const DocumentSource* firstSource =
        self.state->document.FindSource(firstSourceId);
    if (!firstSource) {
        self.binSummaryLabel.stringValue =
            @"Ce média doit être réingéré avant son montage.";
        return NO;
    }
    RationalTime timelineIn;
    try {
        timelineIn =
            self.state->viewport.XToTime(point.x, firstSource->duration.rate);
        if (timelineIn.value < 0) timelineIn = {0, timelineIn.rate};
    } catch (const std::exception& exception) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Drop refusé : %@",
                                       CMStringFromUtf8(exception.what())];
        return NO;
    }
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    // The tracks the drop creates and its edits are one step: one Cmd+Z
    // takes the whole drop back, its new tracks with it.
    const InterventionMark mark =
        MarkIntervention(self.state->activeTimelineId, stagedLog);
    Ulid videoTrackId = targets->video_track_id;
    Ulid audioTrackId = targets->audio_track_id;
    std::vector<Ulid> createdTrackIds;
    const auto createTrack = [&](const std::string& kind, Ulid& trackId) {
        int32_t index = 0;
        for (const DocumentTrack& track : stagedDocument.sequence.tracks)
            index = std::max(index, track.index + 1);
        trackId = GenerateUlid();
        if (!stagedLog.Apply(stagedDocument,
                             Operation{AddTrackOperation{trackId, kind, index}},
                             error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Création de la piste refusée (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return false;
        }
        createdTrackIds.push_back(trackId);
        return true;
    };
    if (targets->create_video_track && !createTrack("video", videoTrackId))
        return NO;
    if (targets->create_audio_track && !createTrack("audio", audioTrackId))
        return NO;
    std::vector<Ulid> primaryClipIds;
    std::vector<Ulid> linkedClipIds;
    for (NSString* mediaId in mediaIds) {
        const Ulid sourceId(mediaId.UTF8String ?: "");
        const DocumentSource* source = stagedDocument.FindSource(sourceId);
        if (!source) {
            self.binSummaryLabel.stringValue =
                @"Un des médias doit être réingéré avant son montage.";
            return NO;
        }
        // EditLog::Apply replaces the staged Document with a validated copy.
        // Keep exact values, never pointers into it, across each application.
        const RationalTime sourceDuration = source->duration;
        const int32_t sourceRate = sourceDuration.rate;
        const auto [hasPicture, hasSound] = partsOf(sourceId);
        const bool picture = hasPicture && !videoTrackId.empty();
        const bool sound = hasSound && !audioTrackId.empty();
        // A still dropped with Shift, a sound with Option: nothing of this
        // one to edit, and no room left for it.
        if (!picture && !sound) continue;
        std::optional<DeleteGapOperation> sourceEdit =
            TimelineSourceEditOperation(
                stagedDocument, sourceId, {0, sourceRate}, sourceDuration,
                timelineIn, picture ? videoTrackId : audioTrackId, insert,
                picture && sound ? std::vector<Ulid>{audioTrackId}
                                 : std::vector<Ulid>{});
        if (!sourceEdit ||
            !stagedLog.Apply(stagedDocument, Operation{std::move(*sourceEdit)},
                             error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Montage refusé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
        // A plain selection takes the picture, or the sound edited alone.
        const Ulid primaryTrackId = picture ? videoTrackId : audioTrackId;
        for (const Ulid& trackId :
             {picture ? videoTrackId : Ulid{}, sound ? audioTrackId : Ulid{}}) {
            const DocumentTrack* track = stagedDocument.FindTrack(trackId);
            if (!track) continue;
            for (const DocumentClip& clip : track->clips) {
                if (clip.source_id == sourceId &&
                    clip.timeline_in == timelineIn &&
                    clip.source_in == RationalTime{0, sourceRate} &&
                    clip.duration == sourceDuration) {
                    if (trackId == primaryTrackId)
                        primaryClipIds.push_back(clip.id);
                    linkedClipIds.push_back(clip.id);
                    break;
                }
            }
        }
        timelineIn = timelineIn.add(sourceDuration);
    }
    if (linkedClipIds.empty()) {
        self.binSummaryLabel.stringValue =
            @"Rien à monter : ces médias n'ont pas cette partie.";
        return NO;
    }
    if (stagedLog.AppliedCount() > mark.history_depth + 1) {
        size_t grouped = 0;
        if (!stagedLog.GroupSince(stagedDocument, mark, "Médias déposés",
                                  grouped, error, message)) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Montage refusé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
    }

    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    // A track the drop created is targeted, as one added by hand
    // (-addTrack:).
    for (const Ulid& trackId : createdTrackIds)
        self.state->targetedTrackIds.insert(trackId);
    if (!createdTrackIds.empty())
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
    self.state->interaction->SelectClips(
        self.state->linkedSelection ? linkedClipIds : primaryClipIds);
    const RationalTime playhead = self.state->requestedPosition;
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
    return YES;
}

- (NSString*)transportStatus {
    if (self.state->playbackDirection > 0)
        return [NSString
            stringWithFormat:@"Lecture ▶ ×%d", self.state->playbackDirection];
    if (self.state->playbackDirection < 0)
        return [NSString
            stringWithFormat:@"Lecture ◀ ×%d", -self.state->playbackDirection];
    return @"Pause";
}

- (NSString*)playheadResolutionStatus {
    NSString* name = self.state->playheadResolution == PlayheadResolution::Frame
                         ? @"Image"
                         : @"Échantillon 48 kHz";
    const std::string key = ui::shortcuts::DisplaySpec(std::string(
        self.shortcutBindings[@"playhead.resolution"].UTF8String ?: ""));
    if (key.empty()) {
        return name;
    }
    return
        [NSString stringWithFormat:@"%@ (%@)", name,
                                   [NSString stringWithUTF8String:key.c_str()]];
}

- (void)setPlaybackDirection:(int)direction {
    // ON9 -- any playback command but « Lire autour » itself ends it.
    self.playAroundActive = NO;
    // Out of lot, 29 October 2026 -- one player at a time: the timeline's
    // stops the source monitor's.
    if (direction != 0 && self.state->sourcePlaybackDirection != 0)
        [self setSourcePlaybackDirection:0];
    const bool wasPlaying = self.state->playbackDirection != 0;
    self.state->playbackDirection = std::clamp(direction, -4, 4);
    self.state->playbackAnchor = self.state->requestedPosition;
    // PERF10. Playback's read-ahead is for playback: at rest the decoders
    // fill a few frames around the playhead, and what playback cached past
    // kRestingCacheBytes goes, least recently shown first -- the frames at
    // the playhead stay.
    const bool playing = self.state->playbackDirection != 0;
    for (auto& worker : self.state->workers) worker.second->SetPlaying(playing);
    [self syncPlaybackReader];
    if (wasPlaying && !playing && self.state->frameCache)
        self.state->frameCache->TrimTo(kRestingCacheBytes);
    // PRD37 -- ROADMAP.md. A play goes straight to PlayFrom, which sets the
    // cursor, the speed and the direction itself. Stopping first closed the
    // sound output, and opening it again at once blocked here 130 to 200 ms
    // (measured on 3 October 2026) with the playback clock already running:
    // three to five frames skipped at every start, and the sound that late
    // behind the picture for the whole play. A stop silences the output and
    // leaves it open (AudioPlayback.h).
    if (self.state->audioPlayback) {
        if (!playing) {
            self.state->audioPlayback->Stop();
        } else {
            std::string error;
            if (!self.state->audioPlayback->PlayFrom(
                    self.state->playbackAnchor, self.state->playbackDirection,
                    error))
                std::fprintf(stderr, "Audio playback failed: %s\n",
                             error.c_str());
        }
    }
    // FC4 -- stopped in trim mode, Record is the Trim Monitor again.
    if (!playing && wasPlaying && self.state->trimSession.Active())
        [self requestResolvedPosition:self.state->requestedPosition];
    // The clock starts once the sound has: an output that still had to open
    // (after its idle grace) holds the first image that long, sound and
    // picture together, instead of skipping the frames it took.
    self.state->playbackStarted = std::chrono::steady_clock::now();
    [self updateSelectionInfo];
}

- (void)updateSelectionInfo {
    [self refreshWorkspaceActivity];
    // F2.2 -- this is the one hook already fired after every selection
    // change and every edit that could touch the selected clip (see this
    // method's call sites), so it doubles as the Inspector's refresh
    // trigger.
    //
    // PrimarySelectedClipId(), not SelectedClipId(): the latter is empty
    // whenever more than one clip is selected, and clicking a clip that is
    // A/V-linked -- which every clip imported from a video with sound is --
    // selects its partner too. Keying the Inspector off it left the panel
    // blank for exactly the footage people actually edit.
    // A reload shows the document again: whatever a drag was previewing is
    // either committed by now or given up.
    if (self.state->gradePreview || self.state->framingPreview) {
        self.state->gradePreview.reset();
        self.state->framingPreview.reset();
        self.state->overlayDirty = true;
    }
    [self.inspectorView showPlayheadAt:self.state->requestedPosition];
    [self refreshTrimBar];
    [self.inspectorView
        reloadWithDocument:self.state->document
            selectedClipId:self.state->interaction->PrimarySelectedClipId()];
    // F2.3 -- keep the Captions tab's style list and its summary of the
    // selected cartons live even when that tab is not on screen: an edit
    // from the chat or an undo changes them too. Cheap -- a few styles and
    // one label.
    if (self.captionStyleTable) [self rebuildCaptionStylesList];
    const size_t selectionCount =
        self.state->interaction->SelectedClipIds().size();
    if (selectionCount > 1) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    %zu clips "
                             @"sélectionnés    Liens %@",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool, self.shortcutBindings),
                             selectionCount,
                             self.state->linkedSelection ? @"ON" : @"OFF"];
        return;
    }
    const DocumentClip* clip = self.state->document.FindClip(
        self.state->interaction->SelectedClipId());
    if (!clip) {
        if (const auto& gap = self.state->interaction->SelectedGap()) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@  "
                                 @"  Trou sélectionné    piste %@    début %@  "
                                 @"  durée %@    Delete pour raccorder",
                                 [self transportStatus],
                                 [self playheadResolutionStatus],
                                 ToolName(self.state->tool,
                                          self.shortcutBindings),
                                 self.state->interaction->SnappingEnabled()
                                     ? @"ON (N)"
                                     : @"OFF (N)",
                                 CMStringFromUtf8(gap->track_id),
                                 TimeString(gap->start),
                                 TimeString(gap->duration)];
            return;
        }
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"%@    Playhead %@    Outil %@    Aimant %@    "
                             @"Aucun clip sélectionné",
                             [self transportStatus],
                             [self playheadResolutionStatus],
                             ToolName(self.state->tool, self.shortcutBindings),
                             self.state->interaction->SnappingEnabled()
                                 ? @"ON (N)"
                                 : @"OFF (N)"];
        return;
    }
    const DocumentSource* source =
        self.state->document.FindSource(clip->source_id);
    const DocumentTrack* selectedTrack =
        self.state->document.FindTrackForClip(clip->id);
    const auto metadata = self.state->mediaMetadata.find(clip->source_id);
    const LibraryMedia* media = metadata == self.state->mediaMetadata.end()
                                    ? nullptr
                                    : &metadata->second;
    NSString* sourceText =
        source ? [NSString stringWithFormat:@"%@ (%@)",
                                            CMStringFromUtf8(source->path),
                                            CMStringFromUtf8(source->id)]
               : [NSString stringWithUTF8String:clip->source_id.c_str()];
    NSString* metadataText = @"métadonnées indisponibles";
    if (media && media->metadata_complete) {
        metadataText = [NSString
            stringWithFormat:@"%dx%d %@  %@/%@  range %@  matrix %@  rot %d°  "
                             @"%d/%d fps  audio %@",
                             media->width, media->height,
                             [NSString stringWithUTF8String:media->orientation
                                                                .c_str()],
                             CMStringFromUtf8(media->pixel_format),
                             CMStringFromUtf8(media->color_transfer),
                             CMStringFromUtf8(media->color_range),
                             CMStringFromUtf8(media->color_space),
                             media -> rotation_degrees, media -> rate.num,
                             media->rate.den,
                             media->has_audio ? @"oui" : @"non"];
    }
    NSString* roleText = @"clip";
    if (selectedTrack && selectedTrack->kind == "audio")
        roleText = @"audio séparé";
    else if (selectedTrack && selectedTrack->kind == "video")
        roleText =
            clip->sync_anchor_clip_id.empty() ? @"vidéo muette" : @"vidéo liée";
    NSString* syncText = @"";
    if (selectedTrack && selectedTrack->kind == "audio") {
        const auto drift = ClipSyncDrift(self.state->document, clip->id);
        if (drift && drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*drift, [self playheadFrameRate]);
            syncText = [NSString stringWithFormat:@"    Décalage sync %@",
                                                  CMStringFromUtf8(label)];
        }
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:
            @"%@    Playhead %@    Outil %@    Aimant %@    %@%@    source %@  "
            @"  %@    source_in %@    durée %@    timeline_in %@",
            [self transportStatus], [self playheadResolutionStatus],
            ToolName(self.state->tool, self.shortcutBindings),
            self.state->interaction->SnappingEnabled() ? @"ON (N)" : @"OFF (N)",
            roleText, syncText, sourceText, metadataText,
            TimeString(clip->source_in), TimeString(clip->duration),
            TimeString(clip->timeline_in)];
}

// F2.2 -- CMInspectorViewDelegate. Every grading slider commit arrives
// here as a ready-to-apply SetClipEffectsOperation; this is the only place
// that actually calls EditLog::Apply for it, matching every other edit in
// this file (e.g. -menuAddCrossDissolve: above) -- CMInspectorView itself
// never touches Document/DocumentClip directly (PHILOSOPHY.md principle
// 2/3). No refreshTimelineAfterEditFromPosition here: a grade change
// leaves every clip's position/duration untouched, it only changes what
// the next render composites (see main.mm's ResolveColorGrade call site),
// so marking overlayDirty is the only redraw trigger this edit needs.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipEffects:(SetClipEffectsOperation)operation {
    // Applied or refused, the monitor shows the document again.
    self.state->gradePreview.reset();
    self.state->overlayDirty = true;
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    if (!applied)
        std::fprintf(stderr, "Grading edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
    // PRD37 -- ROADMAP.md. And so does the panel, which this edit came from:
    // its reset button, wheels, curves and sliders show the stack the
    // document holds now (QA of 3 October 2026: the reset left off on a
    // graded clip, a master bar left in the middle under a typed 1.50). Not
    // -updateSelectionInfo: its full reload drops the commits other sliders
    // still have pending.
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    self.state->overlayDirty = true;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipEffects:(const std::vector<ClipEffect>&)effects
               forClip:(const Ulid&)clipId {
    (void)inspectorView;
    self.state->gradePreview = ui::inspector::GradePreview{clipId, effects};
    self.state->overlayDirty = true;
}

// The author, 29 October 2026 -- zoom, position, fit or fill from the Inspector:
// one SetClipFramingOperation per released drag, the operation
// set_clip_framing applies.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipFraming:(SetClipFramingOperation)operation {
    self.state->framingPreview.reset();
    self.state->overlayDirty = true;
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Cadrage refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipFraming:(const ClipFraming&)framing
               forClip:(const Ulid&)clipId {
    (void)inspectorView;
    self.state->framingPreview =
        ui::inspector::FramingPreview{clipId, framing, std::nullopt};
    self.state->overlayDirty = true;
}

// R2c -- docs/roadmap/REEL-2026-09.md. A key posed or removed by the
// Inspector's diamond, or a keyed row's slider let go: SetClipAnimation, or
// with the last key a batch that keeps its value in the field. One undo.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipKeys:(Operation)operation
                label:(NSString*)label {
    self.state->framingPreview.reset();
    self.state->overlayDirty = true;
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message];
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Clé refusée : %@", CMStringFromUtf8(message)];
        return;
    }
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Inspecteur : %@", label];
    [self requestResolvedPosition:self.state->requestedPosition];
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewClipAnimation:(const std::vector<ClipParameterAnimation>&)animation
                 forClip:(const Ulid&)clipId {
    (void)inspectorView;
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    if (!clip) return;
    self.state->framingPreview =
        ui::inspector::FramingPreview{clipId, clip->framing, animation};
    self.state->overlayDirty = true;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
          moveToFrame:(int64_t)localFrame
               ofClip:(const Ulid&)clipId {
    (void)inspectorView;
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    const MediaRate rate = self.state->document.sequence.frame_rate;
    if (!clip || rate.num <= 0 || rate.den <= 0) return;
    [self setPlaybackDirection:0];
    [self requestTimelinePosition:clip->timeline_in.add(RationalTime(
                                      localFrame * rate.den, rate.num))];
    self.state->overlayDirty = true;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipOpacity:(SetClipOpacityOperation)operation {
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    // PRD37 -- the panel shows what the document holds, applied or not.
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        std::fprintf(stderr, "Opacity edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. The Inspector's « Flou », applied
// as the opacity is: Record composites the blur on its next render.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitClipBlur:(SetClipBlurOperation)operation {
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        [self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                         error:error
                                       message:message];
    [inspectorView refreshAfterOwnEditWithDocument:self.state->document];
    if (!applied) {
        std::fprintf(stderr, "Blur edit rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Flou refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
}

// F2.3 -- ROADMAP.md. The style of a carton, set in the Inspector: one
// UpdateCaptionStyleOperation per gesture, built and checked by the panel's
// policy (RestyleCaptionStyleWhole). Every carton of the style follows, on
// Record and in the Captions tab's list; one undo takes it back.
- (void)inspectorView:(CMInspectorView*)inspectorView
    didCommitCaptionStyle:(UpdateCaptionStyleOperation)operation {
    (void)inspectorView;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                          error:error
                                        message:message]) {
        // The panel shows the style the document still holds.
        [self updateSelectionInfo];
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Style refusé : %@", CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
    self.infoLabel.stringValue = @"Style de légende modifié : tous ses cartons "
                                 @"suivent. ⌘Z l’annule.";
}

#pragma mark - REM5 motion canvas

// REMOTION-PARITY-2026-09 REM5 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// The visual editor of motion, on the Record monitor. Everything here
// relays: what a click selects, where a selection is drawn and which
// operation a gesture commits are MotionCanvas.h's answers (tested,
// portable); this turns events into its calls and its results into one
// operation through the same EditLog as every other edit. Not seen on
// screen yet (VISUAL_QA_CHECKLIST.md, section REM5).

- (std::string)motionMediaDirectory {
    return self.documentPath.length > 0
               ? std::filesystem::absolute(self.documentPath.UTF8String)
                     .parent_path()
                     .string()
               : std::string();
}

// The motion clip Record shows at the playhead -- the topmost one -- its
// frame evaluated, and where its pixels sit in the monitor.
- (BOOL)motionCanvasPrepare {
    MotionCanvasState& canvas = self.state->motionCanvas;
    canvas.has_frame = false;
    const DocumentClip* clip = nullptr;
    for (auto slot = self.state->requested.rbegin();
         slot != self.state->requested.rend(); ++slot) {
        if (!slot->active) continue;
        const DocumentClip* candidate =
            self.state->document.FindClip(slot->clipId);
        if (candidate && IsMotionClip(*candidate)) {
            clip = candidate;
            break;
        }
    }
    if (!clip) return NO;
    MotionClipInstant instant;
    std::string errorName;
    std::string message;
    MotionReach reach;
    MotionError error;
    MotionComposition resolved;
    mcp_json::Value props;
    if (!MotionClipAt(self.state->document, clip->id,
                      self.state->requestedPosition, instant, errorName,
                      message) ||
        !MotionReachFor(self.state->document, instant.composition->id, reach,
                        error) ||
        !ResolveMotionVariant(instant.parsed, std::string(), std::string(),
                              clip->motion_props, resolved, props, error))
        return NO;
    MotionCoreTextMeasurer measurer([self motionMediaDirectory]);
    measurer.SetMediaPaths(reach.media_paths);
    MotionFrame frame;
    if (!EvaluateMotionFrame(resolved, props, instant.frame, measurer,
                             reach.Context(), frame, error))
        return NO;
    // MA6 -- the layers, named as After names them.
    const std::string outlineKey = instant.composition->id + "\n" +
                                   props.Dump() + "\n" +
                                   instant.composition->definition.Dump();
    if (canvas.outline_key != outlineKey) {
        MotionError outlineError;
        if (!MotionOutlineRows(resolved, props, measurer, reach.Context(),
                               canvas.outline, outlineError)) {
            canvas.outline.clear();
        }
        canvas.outline_key = outlineKey;
    }
    const int32_t sequenceWidth = self.state->document.sequence.width;
    const int32_t sequenceHeight = self.state->document.sequence.height;
    ClipFramingGeometry framing;
    if (!ResolveClipFraming(clip->framing, resolved.width, resolved.height,
                            sequenceWidth, sequenceHeight, framing, message))
        return NO;
    // The frame's canvas in the monitor, as the captions are placed on it.
    const NSRect bounds = self.programMonitorView.bounds;
    const double layerScale = self.programMonitorView.layer.contentsScale;
    const double zoomPoints =
        self.state->programMonitorZoom > 0.0 && layerScale > 0.0
            ? self.state->programMonitorZoom / layerScale
            : 0.0;
    const ui::caption_overlay::Layout layout = ui::caption_overlay::Compute(
        sequenceWidth, sequenceHeight, bounds.size.width, bounds.size.height,
        zoomPoints);
    if (canvas.composition_id != instant.composition->id) {
        canvas.selected.clear();
        canvas.hovered.clear();
    }
    canvas.clip_id = clip->id;
    canvas.composition_id = instant.composition->id;
    canvas.composition_frame = instant.frame;
    canvas.frame = std::move(frame);
    canvas.mapping = MotionMonitorMappingFor(framing, layout.canvas_x,
                                             layout.canvas_y, layout.scale);
    canvas.has_frame = true;
    return YES;
}

- (BOOL)motionCanvasContainsEvent:(NSEvent*)event {
    if (!self.programMonitorView ||
        event.window != self.programMonitorView.window)
        return NO;
    const NSPoint point =
        [self.programMonitorView convertPoint:event.locationInWindow
                                     fromView:nil];
    return NSPointInRect(point, self.programMonitorView.bounds);
}

// The event in the composition's pixels.
- (void)motionCanvasPoint:(NSEvent*)event x:(double&)x y:(double&)y {
    const NSPoint point =
        [self.programMonitorView convertPoint:event.locationInWindow
                                     fromView:nil];
    self.state->motionCanvas.mapping.FromView(
        point.x, self.programMonitorView.bounds.size.height - point.y, x, y);
}

- (std::vector<std::pair<double, double>>)motionCanvasViewQuad:
    (const std::pair<double, double>*)quad {
    std::vector<std::pair<double, double>> points;
    for (int corner = 0; corner < 4; ++corner) {
        double x = 0.0;
        double y = 0.0;
        self.state->motionCanvas.mapping.ToView(quad[corner].first,
                                                quad[corner].second, x, y);
        points.emplace_back(x, y);
    }
    return points;
}

// What the overlay draws: the node under the mouse, the one selected and
// its handles -- moved by the gesture in progress, which is only a preview.
- (void)motionCanvasRedraw {
    const MotionCanvasState& canvas = self.state->motionCanvas;
    std::vector<std::pair<double, double>> hover;
    std::vector<std::pair<double, double>> selection;
    std::vector<std::pair<double, double>> handles;
    // Nothing over the picture in full screen. The selection is kept and
    // comes back with the window (toggleFullscreen).
    if (self.videoFullscreenActive) {
        [self.motionCanvasView setHover:hover
                              selection:selection
                                handles:handles];
        return;
    }
    std::pair<double, double> quad[4];
    if (canvas.has_frame && !canvas.hovered.empty() &&
        canvas.hovered != canvas.selected &&
        MotionNodeQuad(canvas.frame, canvas.hovered, quad))
        hover = [self motionCanvasViewQuad:quad];
    if (canvas.has_frame && !canvas.selected.empty() &&
        MotionNodeQuad(canvas.frame, canvas.selected, quad)) {
        if (canvas.gesture == 1) {
            for (auto& corner : quad) {
                corner.first += canvas.delta_x;
                corner.second += canvas.delta_y;
            }
        } else if (canvas.gesture == 2) {
            // The corner or the edge dragged moves; the rest stays.
            static const int kCorners[8][2] = {{0, 0}, {1, 1}, {2, 2}, {3, 3},
                                               {0, 1}, {1, 2}, {2, 3}, {3, 0}};
            for (const int corner : kCorners[canvas.handle]) {
                quad[corner].first += canvas.delta_x;
                quad[corner].second += canvas.delta_y;
            }
        }
        selection = [self motionCanvasViewQuad:quad];
        std::pair<double, double> viewQuad[4];
        for (int corner = 0; corner < 4; ++corner)
            viewQuad[corner] = selection[static_cast<size_t>(corner)];
        handles = MotionSelectionHandles(viewQuad);
    }
    [self.motionCanvasView setHover:hover selection:selection handles:handles];
}

// The Inspector shows the node selected, or the clip when there is none.
// MA6 -- the composition's layers above its fields; a layer chosen in the
// list that this frame does not show keeps the list, without fields.
- (void)motionCanvasShowInspector {
    MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    std::string error;
    if (!canvas.selected.empty() && stored != nullptr && canvas.has_frame) {
        const bool shown =
            MotionNodeFields(stored->definition, canvas.frame, canvas.selected,
                             canvas.composition_frame, canvas.fields, error);
        if (!shown) {
            canvas.fields.clear();
        }
        MotionNodeLocation location;
        std::string title = "Motion";
        if (FindMotionNode(stored->definition, canvas.selected, location)) {
            const mcp_json::Value* node =
                MotionTreeAt(stored->definition, location.path);
            const mcp_json::Value* type = node ? node->Find("type") : nullptr;
            const mcp_json::Value* name = node ? node->Find("id") : nullptr;
            title =
                stored->name + " · " +
                (type && type->IsString() ? type->AsString() : "") +
                (name && name->IsString() ? " " + name->AsString() : "") +
                (shown ? " · image " + std::to_string(canvas.composition_frame)
                       : " · pas montré à cette image");
        }
        [self.motionInspectorView
            showTitle:CMStringFromUtf8(title)
              outline:canvas.outline
                shown:MotionOutlineShown(canvas.outline, canvas.selected)
             selected:canvas.selected
               fields:canvas.fields];
        if (!self.motionInspectorShown) {
            [self.rightDockPanel setContentView:self.motionInspectorView
                                        forSlot:ui::PanelSlot::Inspector];
            self.motionInspectorShown = YES;
        }
        return;
    }
    canvas.fields.clear();
    if (self.motionInspectorShown) {
        [self.rightDockPanel setContentView:self.inspectorView
                                    forSlot:ui::PanelSlot::Inspector];
        self.motionInspectorShown = NO;
    }
}

- (void)motionInspector:(CMMotionInspectorView*)view selectNode:(NSString*)uid {
    (void)view;
    [self motionCanvasSelect:uid.UTF8String];
}

- (void)motionCanvasRefresh {
    if (![self motionCanvasPrepare]) {
        self.state->motionCanvas.hovered.clear();
    }
    [self motionCanvasRedraw];
    [self motionCanvasShowInspector];
}

- (void)motionCanvasSelect:(const std::string&)uid {
    self.state->motionCanvas.selected = uid;
    [self motionCanvasRedraw];
    [self motionCanvasShowInspector];
}

- (void)motionCanvasHover:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    if (!canvas.has_frame && ![self motionCanvasPrepare]) {
        if (!canvas.hovered.empty()) {
            canvas.hovered.clear();
            [self motionCanvasRedraw];
        }
        return;
    }
    double x = 0.0;
    double y = 0.0;
    [self motionCanvasPoint:event x:x y:y];
    MotionHit hit;
    const std::string hovered =
        MotionHitTest(canvas.frame, x, y, false, hit) ? hit.uid : std::string();
    if (hovered != canvas.hovered) {
        canvas.hovered = hovered;
        [self motionCanvasRedraw];
    }
}

- (void)motionCanvasMouseDown:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    canvas.gesture = 0;
    if (![self motionCanvasPrepare]) {
        [self motionCanvasSelect:std::string()];
        return;
    }
    double x = 0.0;
    double y = 0.0;
    [self motionCanvasPoint:event x:x y:y];
    canvas.start_x = x;
    canvas.start_y = y;
    canvas.delta_x = canvas.delta_y = 0.0;
    // A handle of the selection, within six points, resizes it.
    std::pair<double, double> quad[4];
    if (!canvas.selected.empty() &&
        MotionNodeQuad(canvas.frame, canvas.selected, quad)) {
        const auto handles = MotionSelectionHandles(quad);
        for (size_t index = 0; index < handles.size(); ++index) {
            double hx = 0.0;
            double hy = 0.0;
            double px = 0.0;
            double py = 0.0;
            canvas.mapping.ToView(handles[index].first, handles[index].second,
                                  hx, hy);
            canvas.mapping.ToView(x, y, px, py);
            if (std::hypot(hx - px, hy - py) <= 6.0) {
                canvas.gesture = 2;
                canvas.handle = static_cast<int>(index);
                return;
            }
        }
    }
    MotionHit hit;
    if (MotionHitTest(canvas.frame, x, y, false, hit)) {
        [self motionCanvasSelect:hit.uid];
        canvas.gesture = 1;
    } else {
        [self motionCanvasSelect:std::string()];
    }
}

- (void)motionCanvasMouseDragged:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    double x = 0.0;
    double y = 0.0;
    [self motionCanvasPoint:event x:x y:y];
    canvas.delta_x = x - canvas.start_x;
    canvas.delta_y = y - canvas.start_y;
    [self motionCanvasRedraw];
}

- (void)motionCanvasMouseUp:(NSEvent*)event {
    (void)event;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const int gesture = canvas.gesture;
    canvas.gesture = 0;
    // Less than half a pixel of the composition: a click, not a drag.
    if (std::hypot(canvas.delta_x, canvas.delta_y) < 0.5 ||
        canvas.selected.empty()) {
        [self motionCanvasRedraw];
        return;
    }
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!stored) return;
    std::vector<MotionPropertyEdit> edits;
    std::string error;
    const bool built =
        gesture == 1
            ? MotionMoveEdits(canvas.frame, stored->definition, canvas.selected,
                              canvas.composition_frame, canvas.delta_x,
                              canvas.delta_y, edits, error)
            : MotionResizeEdits(canvas.frame, stored->definition,
                                canvas.selected, canvas.composition_frame,
                                canvas.handle, canvas.delta_x, canvas.delta_y,
                                edits, error);
    mcp_json::Value definition;
    if (!built || !MotionDefinitionWithEdits(stored->definition, edits,
                                             definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        canvas.delta_x = canvas.delta_y = 0.0;
        [self motionCanvasRedraw];
        return;
    }
    canvas.delta_x = canvas.delta_y = 0.0;
    [self motionCanvasCommit:definition
                       label:gesture == 1 ? @"Motion : déplacé"
                                          : @"Motion : redimensionné"];
}

// One gesture, one operation: the whole new definition, which one undo
// takes back.
- (BOOL)motionCanvasCommit:(const mcp_json::Value&)definition
                     label:(NSString*)label {
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    if (!stored) return NO;
    DocumentComposition updated = *stored;
    updated.definition = definition;
    return [self motionCanvasApply:Operation {
        UpdateMotionCompositionOperation { updated }
    }
                             label:label];
}

- (BOOL)motionCanvasApply:(Operation)operation label:(NSString*)label {
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
    [self motionCanvasRefresh];
    self.infoLabel.stringValue = label;
    return YES;
}

- (BOOL)motionCanvasKeyDown:(NSEvent*)event {
    MotionCanvasState& canvas = self.state->motionCanvas;
    if (event.keyCode == 53) {  // Escape
        [self motionCanvasSelect:std::string()];
        return YES;
    }
    if (event.keyCode == 51 || event.keyCode == 117) {  // Delete
        [self motionCanvasRemoveSelected:nil];
        return YES;
    }
    double dx = 0.0;
    double dy = 0.0;
    const double step =
        (event.modifierFlags & NSEventModifierFlagShift) != 0 ? 10.0 : 1.0;
    switch (event.keyCode) {
        case 123:
            dx = -step;
            break;
        case 124:
            dx = step;
            break;
        case 125:
            dy = step;
            break;
        case 126:
            dy = -step;
            break;
        default:
            return NO;
    }
    if (![self motionCanvasPrepare]) return NO;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    std::vector<MotionPropertyEdit> edits;
    mcp_json::Value definition;
    std::string error;
    if (!stored ||
        !MotionMoveEdits(canvas.frame, stored->definition, canvas.selected,
                         canvas.composition_frame, dx, dy, edits, error) ||
        !MotionDefinitionWithEdits(stored->definition, edits, definition,
                                   error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return YES;
    }
    [self motionCanvasCommit:definition label:@"Motion : déplacé"];
    return YES;
}

- (void)motionCanvasRemoveSelected:(id)sender {
    (void)sender;
    MotionCanvasState& canvas = self.state->motionCanvas;
    if (canvas.selected.empty()) return;
    const std::string uid = canvas.selected;
    canvas.selected.clear();
    [self motionCanvasApply:Operation {
        RemoveMotionNodeOperation { canvas.composition_id, uid }
    }
                      label:@"Motion : nœud supprimé"];
}

#pragma mark REM5 menu

- (NSMenuItem*)motionMenuItem:(NSString*)title action:(SEL)action {
    NSMenuItem* item = [self menuItem:title action:action key:@""];
    item.target = self;
    return item;
}

// Where a new node goes: into the node selected when it holds children,
// else into the composition's root.
- (std::string)motionCanvasParentUid {
    const MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!stored) return {};
    MotionNodeLocation location;
    if (!canvas.selected.empty() &&
        FindMotionNode(stored->definition, canvas.selected, location)) {
        const mcp_json::Value* node =
            MotionTreeAt(stored->definition, location.path);
        const mcp_json::Value* type = node ? node->Find("type") : nullptr;
        const std::string name =
            type && type->IsString() ? type->AsString() : std::string();
        if (name == "box" || name == "fill" || name == "sequence")
            return canvas.selected;
    }
    const mcp_json::Value* root = stored->definition.Find("root");
    const mcp_json::Value* uid = root ? root->Find("uid") : nullptr;
    return uid && uid->IsString() ? uid->AsString() : std::string();
}

// The series a sequence sits in, and the node that follows it there.
- (BOOL)motionCanvasSeriesOf:(const std::string&)uid
                      series:(std::string&)series
                        next:(const mcp_json::Value*&)next {
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    MotionNodeLocation location;
    if (!stored || uid.empty() ||
        !FindMotionNode(stored->definition, uid, location) ||
        location.parent.empty() && !location.is_root)
        return NO;
    const mcp_json::Value* parent =
        MotionTreeAt(stored->definition, location.parent);
    const mcp_json::Value* type = parent ? parent->Find("type") : nullptr;
    if (!type || !type->IsString() || type->AsString() != "series") return NO;
    const mcp_json::Value* id = parent->Find("uid");
    series = id && id->IsString() ? id->AsString() : std::string();
    next = nullptr;
    if (location.path.empty() || !location.path.back().is_index) return YES;
    const mcp_json::Value* children = parent->Find("children");
    const size_t index = location.path.back().index;
    if (children && index + 1 < children->AsArray().size())
        next = &children->AsArray()[index + 1];
    return YES;
}

- (NSMenu*)motionCanvasMenuForEvent:(NSEvent*)event {
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Motion"];
    menu.autoenablesItems = NO;
    const BOOL shown = [self motionCanvasPrepare];
    if (shown) {
        double x = 0.0;
        double y = 0.0;
        [self motionCanvasPoint:event x:x y:y];
        MotionHit hit;
        if (MotionHitTest(self.state->motionCanvas.frame, x, y, false, hit))
            [self motionCanvasSelect:hit.uid];
        [menu addItem:[self motionMenuItem:@"Ajouter un texte"
                                    action:@selector(motionCanvasAddText:)]];
        [menu addItem:[self motionMenuItem:@"Ajouter une boîte"
                                    action:@selector(motionCanvasAddBox:)]];
        NSMenuItem* video =
            [self motionMenuItem:@"Ajouter la vidéo du rush sélectionné"
                          action:@selector(motionCanvasAddVideo:)];
        video.enabled = [self selectedMediaIds].count > 0;
        [menu addItem:video];
        [menu addItem:[self motionMenuItem:@"Ajouter une image…"
                                    action:@selector(motionCanvasAddImage:)]];
        std::string series;
        const mcp_json::Value* next = nullptr;
        const std::string& selected = self.state->motionCanvas.selected;
        if ([self motionCanvasSeriesOf:selected series:series next:next]) {
            [menu addItem:NSMenuItem.separatorItem];
            [menu
                addItem:[self motionMenuItem:@"Ajouter une scène à la suite"
                                      action:@selector(motionCanvasAddScene:)]];
            const mcp_json::Value* nextType =
                next ? next->Find("type") : nullptr;
            const std::string following =
                nextType && nextType->IsString() ? nextType->AsString() : "";
            if (following == "sequence") {
                NSMenuItem* transition = [[NSMenuItem alloc]
                    initWithTitle:@"Transition vers la scène suivante"
                           action:nil
                    keyEquivalent:@""];
                NSMenu* presentations =
                    [[NSMenu alloc] initWithTitle:@"Transition"];
                NSArray* names = @[ @"Fondu", @"Glissé", @"Volet", @"Iris" ];
                for (NSUInteger index = 0; index < names.count; ++index) {
                    NSMenuItem* item = [self
                        motionMenuItem:names[index]
                                action:@selector(motionCanvasAddTransition:)];
                    item.tag = static_cast<NSInteger>(index);
                    [presentations addItem:item];
                }
                transition.submenu = presentations;
                [menu addItem:transition];
            } else if (following == "transition") {
                [menu
                    addItem:[self
                                motionMenuItem:@"Régler la transition suivante"
                                        action:@selector
                                        (motionCanvasSelectTransition:)]];
            }
        }
        if (!selected.empty()) {
            [menu addItem:NSMenuItem.separatorItem];
            [menu addItem:[self motionMenuItem:@"Supprimer le nœud"
                                        action:@selector
                                        (motionCanvasRemoveSelected:)]];
        }
        [menu addItem:NSMenuItem.separatorItem];
    }
    [menu addItem:[self motionMenuItem:@"Nouvelle composition de motion ici"
                                action:@selector(motionCanvasNewComposition:)]];
    [menu addItem:[self motionMenuItem:@"Nouvelle composition en scènes ici"
                                action:@selector(motionCanvasNewScenes:)]];
    return menu;
}

- (void)motionCanvasInsert:(const mcp_json::Value&)node label:(NSString*)label {
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    const std::string parent = [self motionCanvasParentUid];
    mcp_json::Value definition;
    std::string error;
    if (!stored || parent.empty() ||
        !MotionDefinitionWithChild(stored->definition, parent, std::string(),
                                   node, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:label];
}

- (void)motionCanvasAddText:(id)sender {
    (void)sender;
    [self motionCanvasInsert:MotionNewNode("text")
                       label:@"Motion : texte ajouté"];
}

- (void)motionCanvasAddBox:(id)sender {
    (void)sender;
    [self motionCanvasInsert:MotionNewNode("box")
                       label:@"Motion : boîte ajoutée"];
}

- (void)motionCanvasAddVideo:(id)sender {
    (void)sender;
    NSString* media = [self selectedMediaIds].firstObject;
    if (!media) return;
    mcp_json::Value node = MotionNewNode("video");
    node.Set("media", mcp_json::Value::MakeString(media.UTF8String));
    [self motionCanvasInsert:node label:@"Motion : vidéo ajoutée"];
}

- (void)motionCanvasAddImage:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[ UTTypeImage ];
    panel.allowsMultipleSelection = NO;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;
    mcp_json::Value node = MotionNewNode("image");
    node.Set("src", mcp_json::Value::MakeString(panel.URL.path.UTF8String));
    [self motionCanvasInsert:node label:@"Motion : image ajoutée"];
}

- (void)motionCanvasAddScene:(id)sender {
    (void)sender;
    std::string series;
    const mcp_json::Value* next = nullptr;
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    mcp_json::Value definition;
    std::string error;
    if (!stored ||
        ![self motionCanvasSeriesOf:self.state->motionCanvas.selected
                             series:series
                               next:next] ||
        !MotionDefinitionWithScene(stored->definition, series,
                                   MotionNewNode("sequence"), 50, definition,
                                   error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:@"Motion : scène ajoutée"];
}

- (void)motionCanvasAddTransition:(NSMenuItem*)sender {
    static const char* kPresentations[] = {"fade", "slide", "wipe", "iris"};
    const NSInteger index = std::clamp<NSInteger>(sender.tag, 0, 3);
    std::string series;
    const mcp_json::Value* next = nullptr;
    const DocumentComposition* stored = self.state->document.FindComposition(
        self.state->motionCanvas.composition_id);
    mcp_json::Value definition;
    std::string error;
    if (!stored ||
        ![self motionCanvasSeriesOf:self.state->motionCanvas.selected
                             series:series
                               next:next] ||
        !MotionDefinitionWithChild(
            stored->definition, series, self.state->motionCanvas.selected,
            MotionNewTransition(kPresentations[index], "from-right", 10,
                                "ease-in-out"),
            definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:@"Motion : transition ajoutée"];
}

- (void)motionCanvasSelectTransition:(id)sender {
    (void)sender;
    std::string series;
    const mcp_json::Value* next = nullptr;
    if (![self motionCanvasSeriesOf:self.state->motionCanvas.selected
                             series:series
                               next:next] ||
        !next)
        return;
    const mcp_json::Value* uid = next->Find("uid");
    if (uid && uid->IsString()) [self motionCanvasSelect:uid->AsString()];
}

// A new composition at the timeline's size, cadence and five seconds,
// posed at the playhead on the lowest video track free there: one step.
- (void)motionCanvasCreate:(BOOL)scenes {
    const DocumentSequence& sequence = self.state->document.sequence;
    const MediaRate rate = sequence.frame_rate;
    const int64_t frames =
        std::max<int64_t>(1, (5 * rate.num + rate.den - 1) / rate.den);
    const RationalTime duration{frames * rate.den, rate.num};
    const RationalTime at = self.state->requestedPosition;
    const DocumentTrack* free = nullptr;
    for (const DocumentTrack* track :
         TimelineTracksInDisplayOrder(self.state->document)) {
        if (track->kind != "video" || track->locked) continue;
        const bool clear =
            std::none_of(track->clips.begin(), track->clips.end(),
                         [&](const DocumentClip& clip) {
                             return clip.timeline_in < at.add(duration) &&
                                    at < clip.timeline_in.add(clip.duration);
                         });
        if (clear) free = track;
    }
    if (!free) {
        self.infoLabel.stringValue =
            @"Motion : aucune piste vidéo libre au playhead, ajoutez-en une";
        return;
    }
    const Ulid composition = GenerateUlid();
    AddMotionCompositionOperation add;
    add.composition = {
        composition, scenes ? "Scènes" : "Composition",
        MotionNewComposition(sequence.width, sequence.height, rate.num,
                             rate.den, frames, scenes)};
    InsertMotionClipOperation insert;
    insert.track_id = free->id;
    insert.composition_id = composition;
    insert.source_in = {0, rate.num};
    insert.duration = duration;
    insert.timeline_in = at;
    BatchOperation batch;
    batch.label = "Nouvelle composition de motion";
    batch.operations = {Operation{add}, Operation{insert}};
    [self motionCanvasApply:Operation{batch}
                      label:@"Motion : composition créée au playhead"];
}

- (void)motionCanvasNewComposition:(id)sender {
    (void)sender;
    [self motionCanvasCreate:NO];
}

- (void)motionCanvasNewScenes:(id)sender {
    (void)sender;
    [self motionCanvasCreate:YES];
}

#pragma mark REM5 Inspector

- (const MotionField*)motionCanvasField:(NSString*)property {
    const std::string name = property.UTF8String;
    for (const MotionField& field : self.state->motionCanvas.fields)
        if (field.property == name) return &field;
    return nullptr;
}

- (void)motionInspector:(CMMotionInspectorView*)view
            commitField:(NSString*)property
                   text:(NSString*)text {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const MotionField* field = [self motionCanvasField:property];
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!field || !stored) return;
    // A field of the node itself, a text's cascade, or a style: the
    // portable policy decides (MotionCanvas.h).
    mcp_json::Value definition;
    std::string error;
    if (!MotionDefinitionWithTypedField(
            stored->definition, canvas.selected, *field, text.UTF8String,
            canvas.composition_frame, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        [self motionCanvasShowInspector];
        return;
    }
    [self motionCanvasCommit:definition
                       label:[NSString stringWithFormat:@"Motion : %@",
                                                        CMStringFromUtf8(
                                                            field->label)]];
}

- (void)motionInspector:(CMMotionInspectorView*)view
              toggleKey:(NSString*)property {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const MotionField* field = [self motionCanvasField:property];
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!field || !stored) return;
    const std::string name = property.UTF8String;
    mcp_json::Value definition;
    std::string error;
    bool built = false;
    if (field->key_here) {
        MotionNodeLocation location;
        const mcp_json::Value* node =
            FindMotionNode(stored->definition, canvas.selected, location)
                ? MotionTreeAt(stored->definition, location.path)
                : nullptr;
        const mcp_json::Value* animate = node ? node->Find("animate") : nullptr;
        const mcp_json::Value* animation =
            animate ? animate->Find(name) : nullptr;
        std::optional<mcp_json::Value> fewer;
        built =
            animation &&
            MotionAnimationWithoutKey(*animation, canvas.composition_frame,
                                      fewer, error) &&
            MotionDefinitionWithAnimation(stored->definition, canvas.selected,
                                          name, fewer, definition, error);
    } else {
        const mcp_json::Value value =
            field->kind == MotionFieldKind::Number
                ? MotionNumberValue(std::strtod(field->text.c_str(), nullptr))
                : mcp_json::Value::MakeString(field->text);
        built = MotionDefinitionWithEdits(
            stored->definition,
            {{canvas.selected, name, value, true, canvas.composition_frame}},
            definition, error);
    }
    if (!built) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition
                       label:field->key_here ? @"Motion : clé retirée"
                                             : @"Motion : clé posée"];
}

- (void)motionInspector:(CMMotionInspectorView*)view
                 easing:(NSString*)easing
            forProperty:(NSString*)property {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    if (!stored) return;
    const std::string name = property.UTF8String;
    MotionNodeLocation location;
    const mcp_json::Value* node =
        FindMotionNode(stored->definition, canvas.selected, location)
            ? MotionTreeAt(stored->definition, location.path)
            : nullptr;
    const mcp_json::Value* animate = node ? node->Find("animate") : nullptr;
    const mcp_json::Value* animation = animate ? animate->Find(name) : nullptr;
    mcp_json::Value eased;
    mcp_json::Value definition;
    std::string error;
    if (!animation ||
        !MotionAnimationWithEasing(*animation, easing.UTF8String, eased,
                                   error) ||
        !MotionDefinitionWithAnimation(stored->definition, canvas.selected,
                                       name, eased, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition label:@"Motion : courbe"];
}

- (void)motionInspector:(CMMotionInspectorView*)view
                 spring:(NSString*)property
                     on:(BOOL)on
                   from:(double)from
                     to:(double)to
                  delay:(NSInteger)delay
                damping:(double)damping {
    (void)view;
    MotionCanvasState& canvas = self.state->motionCanvas;
    const DocumentComposition* stored =
        self.state->document.FindComposition(canvas.composition_id);
    mcp_json::Value definition;
    std::string error;
    if (!stored || !MotionDefinitionWithSpring(
                       stored->definition, canvas.selected, property.UTF8String,
                       on, from, to, std::max<NSInteger>(0, delay),
                       damping > 0.0 ? damping : 10.0, definition, error)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Motion : %@", CMStringFromUtf8(error)];
        return;
    }
    [self motionCanvasCommit:definition
                       label:on ? @"Motion : ressort"
                                : @"Motion : ressort retiré"];
}

- (void)motionInspectorDidClose:(CMMotionInspectorView*)view {
    (void)view;
    [self motionCanvasSelect:std::string()];
}

- (void)setLinkedSelectionEnabled:(BOOL)enabled {
    self.state->linkedSelection = enabled;
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    if (self.state->linkedSelection) {
        self.state->interaction->SelectClips(
            ExpandLinkedClipSelection(self.state->document, selected));
    } else if (selected.size() > 1) {
        self.state->interaction->SelectClip(selected.front());
    }
    [self.window makeFirstResponder:self.metalView];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)timelineMouseDown:(NSEvent*)event {
    // Full screen is for watching. The whole screen is Record there, so the
    // mouse was always over some node of a motion clip and its outline was
    // drawn across the picture; and a click, which no monitor claimed, was
    // read in the timeline's coordinates and acted on clips nobody could
    // see. The mouse does nothing until the window is back.
    if (self.videoFullscreenActive) return;
    if (self.window.firstResponder == self.sourceMonitorView) {
        self.state->sourceMonitorActive = true;
        [self scrubSourceMonitorEvent:event];
        return;
    }
    if (self.window.firstResponder == self.programMonitorView) {
        self.state->sourceMonitorActive = false;
        // ON9 -- in trim mode Record is the Trim Monitor: a press trims.
        if ([self trimMonitorPressAt:[self.programMonitorView
                                         convertPoint:event.locationInWindow
                                             fromView:nil]])
            return;
        // REM5 -- a click on Record selects in the motion clip it shows.
        [self motionCanvasMouseDown:event];
        return;
    }
    self.state->sourceMonitorActive = false;
    self.state->duplicateDragging = false;
    self.state->duplicateDragClipboard.clear();
    self.state->zoomBarDrag.reset();
    const NSPoint point = [self timelinePointForEvent:event];
    const double timelineHeight = [self timelineHeight];
    if (point.y < 0.0 || point.y > timelineHeight) return;
    [self dismissAgentMarks];  // PRD25
    const TimelinePointerTarget target = [self timelineTargetAt:point
                                                          event:event];
    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. What the press took: a click
    // on an edge that moves nothing selects its point at the release. Any
    // other press in the tracks leaves trim mode, as a click elsewhere drops
    // Resolve's edit selection; Shift on an edge adds to it instead.
    self.state->pressTarget = target;
    self.state->pressShift =
        (event.modifierFlags & NSEventModifierFlagShift) != 0;
    if (self.state->trimSession.Active() && point.y >= kTimelineRulerHeight &&
        (target.kind == TimelinePointerTargetKind::ClipBody ||
         target.kind == TimelinePointerTargetKind::None ||
         (target.kind == TimelinePointerTargetKind::ClipEdge &&
          !self.state->pressShift)))
        [self leaveTrimMode];
    // The integrated strip shares its pure geometry with Metal rendering, so
    // the visible handles and the draggable hit areas cannot drift apart.
    if (target.kind == TimelinePointerTargetKind::ZoomBar) {
        const TimelineZoomBarGeometry geometry =
            CalculateTimelineZoomBarGeometry(self.state->viewport,
                                             self.state->duration,
                                             self.metalView.bounds.size.width);
        const TimelineZoomBarPart part = target.zoom_bar_part;
        if (part != TimelineZoomBarPart::None)
            self.state->zoomBarDrag = TimelineZoomBarDrag{
                part, point.x, self.state->viewport, geometry};
        if (NavTraceEnabled())
            std::fprintf(stderr,
                         "nav bar-down x=%.1f part=%s thumb=%.1f..%.1f "
                         "rail=%.1f..%.1f ruler=%.3fs pps=%.3f start=%.4fs\n",
                         point.x, NavTracePart(part), geometry.thumb_x,
                         geometry.thumb_x + geometry.thumb_width,
                         geometry.rail_x, geometry.rail_x + geometry.rail_width,
                         geometry.ruler_seconds,
                         self.state->viewport.pixels_per_second,
                         NavTraceStart(self.state->viewport));
        return;
    }
    const double tracksBottom =
        kTimelineRulerHeight + self.state->document.sequence.tracks.size() *
                                   self.state->viewport.track_height;
    if (point.y >= kTimelineRulerHeight && point.y < tracksBottom &&
        point.x >= 40.0 && point.x < 92.0) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t index =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (index < tracks.size()) {
            const DocumentTrack& track = *tracks[index];
            const int control = point.x < 56.0 ? 0 : point.x < 74.0 ? 1 : 2;
            EditError error = EditError::None;
            std::string message;
            const RationalTime playhead = self.state->requestedPosition;
            bool applies = true;
            Operation operation = SetTrackLockOperation{};
            if ((track.kind == "video" || track.kind == "caption") &&
                control == 0) {
                operation = SetTrackOutputOperation{track.id, !track.visible,
                                                    track.muted, track.solo};
            } else if (track.kind == "audio" && control == 0) {
                operation = SetTrackOutputOperation{track.id, track.visible,
                                                    track.muted, !track.solo};
            } else if (track.kind == "audio" && control == 1) {
                operation = SetTrackOutputOperation{track.id, track.visible,
                                                    !track.muted, track.solo};
            } else if (((track.kind == "video" || track.kind == "caption") &&
                        control == 1) ||
                       (track.kind == "audio" && control == 2)) {
                operation = SetTrackLockOperation{track.id, !track.locked};
            } else {
                applies = false;
            }
            if (applies &&
                [self applyAndPersistTimelineOperation:std::move(operation)
                                                 error:error
                                               message:message]) {
                [self refreshTimelineAfterEditFromPosition:playhead];
                self.state->overlayDirty = true;
            }
        }
        return;
    }
    if (point.y < kTimelineToolbarHeight && point.x >= 0.0) {
        constexpr double toolStart = 4.0;
        constexpr double toolWidth = 24.0;
        if (point.x >= toolStart && point.x < toolStart + toolWidth * 5.0) {
            const int index =
                static_cast<int>((point.x - toolStart) / toolWidth);
            [self setTimelineTool:static_cast<TimelineTool>(index)];
        } else if (point.x >= 128.0 && point.x < 166.0) {
            [self menuToggleSnapping:nil];
        } else if (point.x >= 170.0 && point.x < 216.0) {
            [self menuToggleLinkedSelection:nil];
        } else if (point.x >= 222.0 && point.x < 294.0) {
            const int index = static_cast<int>((point.x - 222.0) / 24.0);
            if (index == 0)
                [self menuPlayReverse:nil];
            else if (index == 1)
                [self menuStop:nil];
            else
                [self menuPlayForward:nil];
        }
        return;
    }
    // D2 -- docs/roadmap/DIALOGUE-2026-10.md. A click on a marker's flag
    // sends the playhead to its exact time, whatever the tool; a double
    // click opens its note. Hit by the geometry the flag is drawn from.
    if (target.kind == TimelinePointerTargetKind::Marker) {
        const std::optional<Ulid> markerId = target.marker_id;
        const DocumentMarker* marker =
            self.state->document.FindMarker(*markerId);
        if (!marker) return;
        // D6 -- with Option held, the marker is taken hold of to be
        // stretched into a span, as in the editor's other program; the
        // playhead stays where it is.
        if (target.marker_stretch) {
            [self setPlaybackDirection:0];
            self.state->markerStretch = AppState::MarkerStretch{
                *markerId,
                TimelineMarkerSpanAnchor(*marker, self.state->viewport,
                                         point.x),
                std::nullopt};
            self.infoLabel.stringValue =
                @"MARQUEUR · glisser pour marquer un passage";
            return;
        }
        [self setPlaybackDirection:0];
        [self requestTimelinePosition:marker->time];
        [self showMarkerInfo:*marker];
        self.state->overlayDirty = true;
        if (event.clickCount >= 2) {
            // After this event: the note is asked in a modal window, which
            // must not open inside the mouse-down it would swallow the
            // mouse-up of.
            const Ulid edited = *markerId;
            __weak AppDelegate* weakSelf = self;
            dispatch_async(dispatch_get_main_queue(), ^{
              [weakSelf editMarkerNote:edited];
            });
        }
        return;
    }
    const TimelineTool tool = [self effectiveTool];
    self.state->lassoCandidate = false;
    self.state->lassoDragging = false;
    if (tool == TimelineTool::Hand) {
        if (self.state->spaceHand) self.state->spaceUsedForPan = true;
        self.state->navigationDragging = true;
        self.state->navigationLastX = point.x;
        [self applyToolCursor];
        return;
    }
    if (tool == TimelineTool::Zoom) {
        const bool zoomOut =
            (event.modifierFlags & NSEventModifierFlagOption) != 0;
        ZoomTimelineAtPointer(self.state->viewport, point.x,
                              zoomOut ? 0.5 : 2.0, self.state->duration,
                              self.metalView.bounds.size.width, std::nullopt);
        self.state->overlayDirty = true;
        return;
    }
    if (tool == TimelineTool::Cut) {
        [self setPlaybackDirection:0];
        if (target.kind != TimelinePointerTargetKind::ClipBody) return;
        const Ulid hitClipId = target.clip_id;
        const DocumentClip* clip = self.state->document.FindClip(hitClipId);
        if (!clip) return;
        const bool linkedCut =
            self.state->linkedSelection &&
            (event.modifierFlags & NSEventModifierFlagOption) == 0;
        Operation operation = TimelineCutOperationForClip(
            self.state->document, *clip,
            self.state->viewport.XToTime(point.x, clip->duration.rate),
            linkedCut);
        EditError error = EditError::None;
        std::string message;
        const RationalTime playhead = self.state->requestedPosition;
        if ([self applyAndPersistTimelineOperation:std::move(operation)
                                             error:error
                                           message:message]) {
            if (linkedCut)
                self.state->interaction->SelectClips(ExpandLinkedClipSelection(
                    self.state->document, {hitClipId}));
            else
                self.state->interaction->SelectClip(hitClipId);
            [self refreshTimelineAfterEditFromPosition:playhead];
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
        } else if (error != EditError::InvalidTimelineIn) {
            std::fprintf(stderr, "Cut rejected (%s): %s\n",
                         EditErrorName(error), message.c_str());
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Coupe refusée (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
        }
        return;
    }
    const bool selectionHit =
        target.kind == TimelinePointerTargetKind::ClipBody ||
        target.kind == TimelinePointerTargetKind::ClipEdge;
    // D7 -- docs/roadmap/DIALOGUE-2026-10.md. A double click on a carton
    // puts the keyboard in its text, in the Inspector. Its edges count as
    // the carton: one word long, it is little more than its two edges.
    // Nothing is dragged by this click -- the first one selected the carton.
    if (tool == TimelineTool::Select && event.clickCount >= 2 && selectionHit &&
        ui::media_panel::CartonTextAccess(self.state->document,
                                          target.clip_id) !=
            ui::media_panel::CartonTextOutcome::NotACarton) {
        [self setPlaybackDirection:0];
        const Ulid edited = target.clip_id;
        self.state->interaction->SelectClip(edited);
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
        [self editCartonText:edited];
        return;
    }
    // ON9 -- a double click on a cut takes it as a roll and shows the Trim
    // Monitor, as Resolve opens its Trim Editor there (manual, p. 1067);
    // the first click took the side its zone gives.
    if ((tool == TimelineTool::Select || tool == TimelineTool::Trim) &&
        event.clickCount >= 2 &&
        target.kind == TimelinePointerTargetKind::ClipEdge &&
        !TimelineTrimModeStretches(target.trim_mode) &&
        !self.state->trimSession.Dynamic().active) {
        [self setPlaybackDirection:0];
        const TrimEdge edge = target.edge == TimelineHitEdge::Head
                                  ? TrimEdge::Head
                                  : TrimEdge::Tail;
        trim::TrimRefusal refusal = self.state->trimSession.SelectEdge(
            self.state->document, target.clip_id, edge, trim::TrimSide::Roll,
            false);
        if (refusal == trim::TrimRefusal::NoJunction)
            refusal = self.state->trimSession.SelectEdge(
                self.state->document, target.clip_id, edge,
                trim::TrimSideOfEdge(edge, false), false);
        self.state->trimPreview.reset();
        self.state->overlayDirty = true;
        if (refusal != trim::TrimRefusal::None) {
            [self showTrimRefusal:refusal];
            return;
        }
        [self syncTrimStyle];
        [self requestResolvedPosition:self.state->requestedPosition];
        [self showTrimStatus];
        return;
    }
    if (tool == TimelineTool::Select &&
        target.kind == TimelinePointerTargetKind::None &&
        point.x >= self.state->viewport.header_width &&
        !target.track_id.empty()) {
        self.state->lassoCandidate = true;
        self.state->lassoStartX = self.state->lassoCurrentX = point.x;
        self.state->lassoStartY = self.state->lassoCurrentY = point.y;
        self.state->lassoPlayhead = self.state->requestedPosition;
    }
    [self setPlaybackDirection:0];
    const bool optionGesture =
        (event.modifierFlags & NSEventModifierFlagOption) != 0;
    const bool commandGesture =
        (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    const bool duplicateGesture = target.duplicate;
    self.state->linkedSelectionGesture =
        self.state->linkedSelection && (!optionGesture || duplicateGesture);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelectionGesture);
    self.state->interaction->SetPlayheadSnapTarget(
        commandGesture
            ? std::optional<RationalTime>(self.state->requestedPosition)
            : std::nullopt);
    self.state->interaction->SetPlayhead(self.state->requestedPosition);
    // FC1 -- docs/roadmap/FILMCRAFT-2026-10.md. The timeline's In and Out
    // hold the magnet too; the window keeps them, never the document.
    self.state->interaction->SetInOutSnapTargets(self.state->timelineIn,
                                                 self.state->timelineOut);
    self.state->interaction->SetTrimMode(target.trim_mode);
    // Shift on a clip's body extends the selection, as it does in the media
    // panel; on an edge it stays the ripple trim (TimelineView.h).
    self.state->interaction->SetSelectionToggle(target.selection_toggle);
    // DEC40 -- Command on a body extends it, as in Resolve; the magnet
    // still takes the playhead for the drag that follows.
    self.state->interaction->SetSelectionExtend(target.selection_extend);
    self.state->interaction->PointerDown(point.x, point.y,
                                         self.metalView.bounds.size.width,
                                         [self playheadInputRate]);
    // PointerDown owns linked-selection expansion because it also knows which
    // linked members are protected by a locked track. Re-expanding here would
    // undo that safety decision before the first drag event.
    self.state->editDragging = self.state->interaction->HasActiveDrag();
    self.state->duplicateDragging =
        duplicateGesture && self.state->editDragging;
    if (self.state->duplicateDragging)
        self.state->duplicateDragClipboard = CopyTimelineClips(
            self.state->document, self.state->interaction->SelectedClipIds());
    self.state->scrubDragging =
        self.state->interaction->RequestedPlayhead().has_value();
    if (self.state->interaction->RequestedPlayhead()) {
        RationalTime requested = *self.state->interaction->RequestedPlayhead();
        if (commandGesture) {
            const auto snapped = SnapTimelineTimeToEdit(
                self.state->document, self.state->viewport, requested);
            if (snapped) requested = *snapped;
        }
        [self requestTimelinePosition:requested];
        self.state->interaction->ClearRequestedPlayhead();
    }
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)timelineMouseDragged:(NSEvent*)event {
    if (self.videoFullscreenActive) return;
    if (self.trimMonitorDragging) {
        [self trimMonitorDragTo:[self.programMonitorView
                                    convertPoint:event.locationInWindow
                                        fromView:nil]];
        return;
    }
    // REM5 -- a gesture begun on Record follows the mouse there.
    if (self.state->motionCanvas.gesture != 0) {
        [self motionCanvasMouseDragged:event];
        return;
    }
    if (self.state->sourceMonitorActive &&
        self.window.firstResponder == self.sourceMonitorView) {
        [self scrubSourceMonitorEvent:event];
        return;
    }
    const NSPoint point = [self timelinePointForEvent:event];
    if (self.state->markerStretch) {
        // D6 -- the span follows the pointer, on whole frames; the policy
        // (which side, a point again on the anchor) is TimelineView.h's.
        const DocumentMarker* marker = self.state->document.FindMarker(
            self.state->markerStretch->marker_id);
        if (!marker) {
            self.state->markerStretch.reset();
            return;
        }
        const double x = std::clamp(point.x, self.state->viewport.header_width,
                                    self.metalView.bounds.size.width);
        RationalTime pointer =
            self.state->viewport.XToTime(x, [self playheadFrameRate].num);
        if (pointer < RationalTime{0, 1}) pointer = {0, pointer.rate};
        pointer = QuantizePlayheadPosition(pointer, PlayheadResolution::Frame,
                                           [self playheadFrameRate]);
        self.state->markerStretch->reached = TimelineMarkerSpanUpdate(
            *marker, self.state->markerStretch->anchor, pointer);
        const UpdateMarkerOperation& reached =
            *self.state->markerStretch->reached;
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"MARQUEUR  %@ → %@",
                             TimelineTimecode(reached.time,
                                              [self playheadFrameRate]),
                             TimelineTimecode(
                                 reached.time.add(reached.duration),
                                 [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->zoomBarDrag) {
        UpdateTimelineZoomBarDrag(
            self.state->viewport, *self.state->zoomBarDrag, point.x,
            self.state->duration, self.metalView.bounds.size.width,
            kTimelineViewRate);
        if (NavTraceEnabled()) {
            const TimelineZoomBarGeometry held =
                CalculateTimelineZoomBarGeometry(
                    self.state->viewport, self.state->duration,
                    self.metalView.bounds.size.width,
                    self.state->zoomBarDrag->initial_geometry.ruler_seconds);
            std::fprintf(stderr,
                         "nav bar-drag x=%.1f part=%s thumb=%.1f..%.1f "
                         "pps=%.3f start=%.4fs\n",
                         point.x, NavTracePart(self.state->zoomBarDrag->part),
                         held.thumb_x, held.thumb_x + held.thumb_width,
                         self.state->viewport.pixels_per_second,
                         NavTraceStart(self.state->viewport));
        }
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->navigationDragging) {
        const double delta = self.state->navigationLastX - point.x;
        self.state->viewport.ScrollByPixels(delta, kTimelineViewRate);
        self.state->navigationLastX = point.x;
        self.state->overlayDirty = true;
        return;
    }
    if (self.state->lassoCandidate) {
        self.state->lassoCurrentX = point.x;
        self.state->lassoCurrentY = point.y;
        const double deltaX = point.x - self.state->lassoStartX;
        const double deltaY = point.y - self.state->lassoStartY;
        if (!self.state->lassoDragging && std::hypot(deltaX, deltaY) >= 4.0) {
            self.state->lassoDragging = true;
            self.state->scrubDragging = false;
            // The press jumped the playhead to it before the gesture could
            // tell a lasso from a scrub; selecting does not move it.
            [self requestTimelinePosition:self.state->lassoPlayhead];
        }
        if (self.state->lassoDragging) {
            std::vector<Ulid> selected = LassoHitTestTimeline(
                self.state->document, self.state->viewport,
                self.state->lassoStartX, self.state->lassoStartY,
                self.state->lassoCurrentX, self.state->lassoCurrentY,
                self.metalView.bounds.size.width);
            if (self.state->linkedSelectionGesture)
                selected =
                    ExpandLinkedClipSelection(self.state->document, selected);
            self.state->interaction->SelectClips(selected);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->scrubDragging) {
        RationalTime requested =
            self.state->viewport.XToTime(point.x, [self playheadInputRate]);
        if ((event.modifierFlags & NSEventModifierFlagCommand) != 0) {
            const auto snapped = SnapTimelineTimeToEdit(
                self.state->document, self.state->viewport, requested);
            if (snapped) requested = *snapped;
        }
        [self requestTimelinePosition:requested];
        return;
    }
    self.state->interaction->SetPlayheadSnapTarget(
        (event.modifierFlags & NSEventModifierFlagCommand) != 0
            ? std::optional<RationalTime>(self.state->requestedPosition)
            : std::nullopt);
    self.state->interaction->SetPlayhead(self.state->requestedPosition);
    // FC1 -- read again with each move, as the playhead target is.
    self.state->interaction->SetInOutSnapTargets(self.state->timelineIn,
                                                 self.state->timelineOut);
    self.state->interaction->PointerDrag(point.x, point.y,
                                         self.metalView.bounds.size.width);
    // AppKit may report Option only after the mouse-down event. Promote an
    // active body move to a duplicate as soon as the modifier appears, which
    // matches the interaction users expect from Premiere/Resolve.
    if (!self.state->duplicateDragging && self.state->editDragging &&
        [self effectiveTool] == TimelineTool::Select &&
        (event.modifierFlags & NSEventModifierFlagOption) != 0 &&
        self.state->interaction->MovePreview()) {
        self.state->duplicateDragging = true;
        self.state->duplicateDragClipboard = CopyTimelineClips(
            self.state->document, self.state->interaction->SelectedClipIds());
    }
    self.state->overlayDirty = true;
}

- (void)timelineMouseMoved:(NSEvent*)event {
    if (self.videoFullscreenActive) return;
    // REM5 -- over Record, the node under the mouse is outlined.
    if ([self motionCanvasContainsEvent:event]) {
        [self motionCanvasHover:event];
        return;
    }
    [self applyToolCursor];
    self.state->cutPreviewX.reset();
    self.state->cutPreviewY.reset();
    const NSPoint point = [self timelinePointForEvent:event];
    const int previousHoveredTool = self.state->hoveredTimelineTool;
    const Ulid previousHoveredTrack = self.state->hoveredTrackId;
    const int previousHoveredControl = self.state->hoveredTrackControl;
    self.state->hoveredTimelineTool = -1;
    self.state->hoveredTrackId.clear();
    self.state->hoveredTrackControl = -1;
    // FC2 -- what a press here would take hold of.
    const TimelinePointerTarget target = [self timelineTargetAt:point
                                                          event:event];
    // D2 -- the flag under the pointer, by the geometry it is drawn from.
    const Ulid previousHoveredMarker = self.state->hoveredMarkerId;
    self.state->hoveredMarkerId =
        target.kind == TimelinePointerTargetKind::Marker ? target.marker_id
                                                         : Ulid{};

    if (point.y >= 0.0 && point.y < kTimelineToolbarHeight && point.x >= 4.0 &&
        point.x < 124.0)
        self.state->hoveredTimelineTool =
            static_cast<int>((point.x - 4.0) / 24.0);
    if (point.y >= kTimelineRulerHeight) {
        const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
        const size_t trackIndex =
            static_cast<size_t>((point.y - kTimelineRulerHeight) /
                                self.state->viewport.track_height);
        if (trackIndex < tracks.size() && point.x >= 40.0 && point.x < 92.0) {
            self.state->hoveredTrackId = tracks[trackIndex]->id;
            self.state->hoveredTrackControl = point.x < 56.0   ? 0
                                              : point.x < 74.0 ? 1
                                                               : 2;
        }
    }
    const bool chromeHoverChanged =
        previousHoveredTool != self.state->hoveredTimelineTool ||
        previousHoveredTrack != self.state->hoveredTrackId ||
        previousHoveredControl != self.state->hoveredTrackControl ||
        previousHoveredMarker != self.state->hoveredMarkerId;
    if ([self effectiveTool] == TimelineTool::Select) {
        if (target.kind == TimelinePointerTargetKind::ClipEdge) {
            [[self edgeCursorFor:target] set];
            if (TimelineTrimModeStretches(target.trim_mode))
                self.infoLabel.stringValue =
                    @"VITESSE · glisser pour étirer le plan";
            else if (target.trim_mode == TimelineTrimMode::Roll)
                self.infoLabel.stringValue =
                    @"ROLL · glisser la coupe, les deux côtés à la fois";
            else if (target.trim_mode == TimelineTrimMode::Ripple)
                self.infoLabel.stringValue = @"RIPPLE TRIM · Shift-glisser";
            else
                self.infoLabel.stringValue = @"TRIM · glisser le raccord";
        } else if (target.duplicate) {
            self.infoLabel.stringValue = @"DUPLIQUER · Option-glisser le clip";
        }
        if (chromeHoverChanged) self.state->overlayDirty = true;
        return;
    }
    if ([self effectiveTool] == TimelineTool::Trim) {
        // FC4 -- the Trim tool: an edge ripples (Command rolls), the body
        // slips.
        // DEC40 -- its stretch ripples.
        if (target.kind == TimelinePointerTargetKind::ClipEdge)
            self.infoLabel.stringValue =
                TimelineTrimModeStretches(target.trim_mode)
                    ? @"VITESSE · glisser pour étirer le plan, la suite suit"
                : target.trim_mode == TimelineTrimMode::Roll
                    ? @"ROLL · glisser la coupe, les deux côtés à la fois"
                    : @"RIPPLE TRIM · glisser le raccord";
        else if (target.kind == TimelinePointerTargetKind::ClipBody)
            self.infoLabel.stringValue =
                target.trim_mode == TimelineTrimMode::Slide
                    ? @"SLIDE · glisser le plan entre ses voisins"
                    : @"SLIP · glisser le contenu source";
        // The author, 31 October 2026: "des fois il est en mode trim normal, des
        // fois il bouge ce qu'il y a dans le plan". The tool's one cursor,
        // the double arrow, said nothing of which: six points from an edge
        // it ripples, past them it slips. Each gesture now has its own --
        // the double arrow on an edge, an open hand on the body that slips
        // the content, a pointing hand on the name bar that slides the clip
        // -- before the press, not only on the status line.
        if (target.kind == TimelinePointerTargetKind::ClipBody)
            [(target.trim_mode == TimelineTrimMode::Slide
                  ? NSCursor.pointingHandCursor
                  : NSCursor.openHandCursor) set];
        else if (target.kind == TimelinePointerTargetKind::ClipEdge)
            [[self edgeCursorFor:target] set];
        else
            [NSCursor.arrowCursor set];
        self.state->overlayDirty = true;
        return;
    }
    if ([self effectiveTool] != TimelineTool::Cut) {
        self.state->overlayDirty = true;
        return;
    }
    if (point.y < kTimelineRulerHeight || point.y > [self timelineHeight]) {
        self.state->overlayDirty = true;
        return;
    }
    const DocumentClip* clip =
        target.kind == TimelinePointerTargetKind::ClipBody
            ? self.state->document.FindClip(target.clip_id)
            : nullptr;
    if (clip) {
        const RationalTime cut =
            self.state->viewport.XToTime(point.x, clip->duration.rate);
        if (cut > clip->timeline_in &&
            cut < clip->timeline_in.add(clip->duration)) {
            self.state->cutPreviewX = self.state->viewport.TimeToX(cut);
            const auto tracks =
                TimelineTracksInDisplayOrder(self.state->document);
            const auto track = std::find_if(
                tracks.begin(), tracks.end(), [&](const DocumentTrack* value) {
                    return value->id == target.track_id;
                });
            if (track != tracks.end())
                self.state->cutPreviewY = kTimelineRulerHeight +
                                          std::distance(tracks.begin(), track) *
                                              self.state->viewport.track_height;
        }
    }
    self.state->overlayDirty = true;
}

- (BOOL)persistEdits:(std::string&)message {
    const BOOL persisted = [self persistStagedDocumentNow:self.state->document
                                                  editLog:self.state->editLog
                                                  message:message];
    if (persisted) self.state->lastHistoryDomain = HistoryDomain::Timeline;
    return persisted;
}

// Out of lot -- the author, 29 October 2026: "l'app est toujours lente dans les
// opérations". A window edit is saved as a gesture is: checked in memory
// here, written on the commit queue (-persistEditsInBackground:). Written
// here, the autosave envelope (encoded, then read back) and the package
// transaction held the main thread ~280 ms per edit on a 32-timeline
// project, once the validation itself had been cut from ~800 ms. An agent's
// edit is still written before it is answered (-persistStagedDocumentNow:).
- (BOOL)persistStagedDocument:(const Document&)document
                      editLog:(const EditLog&)editLog
                      message:(std::string&)message {
    Project candidate = self.state->project;
    if (!candidate.CommitDocument(self.state->activeTimelineId, document,
                                  message))
        return NO;
    auto job = std::make_shared<ProjectCommitJob>();
    job->path = self.documentPath.UTF8String ?: "";
    job->timeline_logs = [self timelineLogsForProject:candidate
                                              editLog:editLog];
    job->project_log = self.state->projectEditLog;
    job->project = candidate;
    self.state->project = std::move(candidate);
    self.state->timelineEditLogs[self.state->activeTimelineId] = editLog;
    {
        std::lock_guard<std::mutex> guard(self.state->pendingCommitMutex);
        self.state->pendingCommit = std::move(job);
    }
    AppState* state = self.state;
    __weak AppDelegate* weakSelf = self;
    dispatch_async([self commitQueue], ^{
      std::shared_ptr<ProjectCommitJob> latest;
      {
          std::lock_guard<std::mutex> guard(state->pendingCommitMutex);
          latest = std::move(state->pendingCommit);
      }
      // A later block of the same burst already wrote the newest state.
      if (!latest) return;
      std::string error;
      const bool committed = RunProjectCommit(*latest, error);
      dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf projectCommitFinished:committed
                                message:CMStringFromUtf8(error)];
      });
    });
    return YES;
}

// SAVING_ROADMAP.md, S3. Written before it returns: what an agent is
// answered, an explicit save, a project switch.
- (BOOL)persistStagedDocumentNow:(const Document&)document
                         editLog:(const EditLog&)editLog
                         message:(std::string&)message {
    Project candidate = self.state->project;
    if (!candidate.CommitDocument(self.state->activeTimelineId, document,
                                  message))
        return NO;
    if (![self commitProjectCandidate:candidate
                              editLog:editLog
                              message:message])
        return NO;
    self.state->project = std::move(candidate);
    self.state->timelineEditLogs[self.state->activeTimelineId] = editLog;
    return YES;
}

- (BOOL)applyAndPersistTimelineOperation:(Operation)operation
                                   error:(EditError&)error
                                 message:(std::string&)message {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (!stagedLog.Apply(stagedDocument, std::move(operation), error, message))
        return NO;
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message])
        return NO;
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    return YES;
}

- (void)reportPendingProjectArchive {
    if (project_archive::DefaultDatabasePath().empty() || self.window == nil) {
        return;
    }
    const char* projectPath = self.documentPath.UTF8String;
    const auto report =
        StoredProjectArchiveStatus(projectPath != nullptr ? projectPath : "");
    if (report.pending == 0 && report.error_name.empty()) {
        self.projectArchiveWarningShown = NO;
        return;
    }
    if (self.projectArchiveWarningShown || self.window.attachedSheet != nil) {
        return;
    }
    self.projectArchiveWarningShown = YES;
    NSAlert* warning = [NSAlert new];
    warning.alertStyle = NSAlertStyleWarning;
    warning.messageText =
        NSLocalizedString(@"Projet enregistré, archive SQL en attente", nil);
    warning.informativeText = NSLocalizedString(
        @"Les captures restent conservées dans le projet. CUTMACHINE "
        @"réessaiera de les archiver à la prochaine sauvegarde ou ouverture. "
        @"Le montage et son historique sont enregistrés.",
        nil);
    [warning addButtonWithTitle:NSLocalizedString(@"Compris", nil)];
    [warning beginSheetModalForWindow:self.window completionHandler:nil];
}

- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                       message:(std::string&)message {
    return [self commitProjectCandidate:project
                                editLog:editLog
                             projectLog:self.state->projectEditLog
                                message:message];
}

// What every window commit does, on whichever thread runs it: the autosave
// envelope first (the recovery net if the package write fails), the package
// transaction, then the envelope discarded. StoredProjectIoMutex serialises
// it against any other storage access.
static bool RunProjectCommit(const ProjectCommitJob& job,
                             std::string& message) {
    if (!ProjectRecovery::WriteAutosave(
            job.path, job.project, job.timeline_logs, job.project_log, message))
        return false;
    if (!CommitStoredProjectAndLogs(job.path, job.project, job.timeline_logs,
                                    job.project_log, message))
        return false;
    std::string discardError;
    if (!ProjectRecovery::DiscardAutosave(job.path, discardError))
        std::fprintf(stderr, "Unable to discard committed autosave: %s\n",
                     discardError.c_str());
    return true;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. One log per timeline of the
// candidate, as the recovery envelope requires since 20 August: a timeline
// the candidate adds starts with an empty log (as a new package does,
// ProjectStorage.cc), and one it removes leaves its log behind. Copying the
// logs the window held refused every timeline creation.
- (std::map<std::string, EditLog>)timelineLogsForProject:(const Project&)project
                                                 editLog:
                                                     (const EditLog&)editLog {
    std::map<std::string, EditLog> timelineLogs;
    for (const DocumentSequence& timeline : project.timelines) {
        if (timeline.id == self.state->activeTimelineId) {
            timelineLogs.emplace(timeline.id, editLog);
            continue;
        }
        const auto held = self.state->timelineEditLogs.find(timeline.id);
        timelineLogs.emplace(timeline.id,
                             held != self.state->timelineEditLogs.end()
                                 ? held->second
                                 : EditLog{});
    }
    return timelineLogs;
}

- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                    projectLog:(const ProjectEditLog&)projectLog
                       message:(std::string&)message {
    // A gesture's save may still be on the queue: it lands first, so this
    // newer state is the one left on disk.
    [self drainProjectCommits];
    ProjectCommitJob job{self.documentPath.UTF8String ?: "", project,
                         [self timelineLogsForProject:project editLog:editLog],
                         projectLog};
    if (!RunProjectCommit(job, message)) return NO;
    self.backgroundCommitError = nil;
    [self reportPendingProjectArchiveThrottled];
    return YES;
}

// Out of lot -- the author, 29 October 2026. The window's commits, in order, off
// the main thread for a gesture's (persistEditsInBackground:).
- (dispatch_queue_t)commitQueue {
    if (!self.projectCommitQueue)
        self.projectCommitQueue = dispatch_queue_create(
            "com.cutmachine.project-commit", DISPATCH_QUEUE_SERIAL);
    return self.projectCommitQueue;
}

// Waits until every save already enqueued is on disk (or has failed).
- (void)drainProjectCommits {
    if (self.projectCommitQueue)
        dispatch_sync(self.projectCommitQueue, ^{
                      });
}

// Out of lot -- the author, 29 October 2026: "there is a delay when moving clips,
// it needs an optimistic update". A gesture's edit is applied, kept and
// drawn at once; its save (autosave, package, archive: ~240 ms on this
// window's main thread for a 23-timeline project) runs on the commit queue.
// Only the candidate's own check, in memory, can still refuse the edit here.
// A save that fails later cannot take the gesture back: the window says so,
// keeps the work in memory, and the next save -- which holds every edit --
// tries again.
- (BOOL)persistEditsInBackground:(std::string&)message {
    if (![self persistStagedDocument:self.state->document
                             editLog:self.state->editLog
                             message:message])
        return NO;
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    return YES;
}

- (void)projectCommitFinished:(BOOL)committed message:(NSString*)message {
    if (!committed) {
        std::fprintf(stderr, "Unable to persist edit: %s\n",
                     message.UTF8String ?: "");
        self.backgroundCommitError = message;
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Modification non enregistrée, nouvel essai "
                             @"à la prochaine modification : %@",
                             message];
        return;
    }
    if (self.backgroundCommitError) {
        self.backgroundCommitError = nil;
        self.infoLabel.stringValue = @"Projet enregistré";
    }
    [self reportPendingProjectArchiveThrottled];
}

- (void)reportPendingProjectArchiveThrottled {
    const NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
    if (self.lastProjectArchiveCheck > 0 &&
        now - self.lastProjectArchiveCheck < 60.0)
        return;
    self.lastProjectArchiveCheck = now;
    [self reportPendingProjectArchive];
}

- (void)rebuildVideoTrackIds {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.kind == "video" && track.visible) tracks.push_back(&track);
    std::sort(tracks.begin(), tracks.end(),
              [](const DocumentTrack* left, const DocumentTrack* right) {
                  return left->index < right->index;
              });
    self.state->videoTrackIds.clear();
    for (const DocumentTrack* track : tracks)
        self.state->videoTrackIds.push_back(track->id);
    self.state->requested.assign(tracks.size(), {});
    self.state->rendered.assign(tracks.size(), {});
}

- (void)addTrack:(BOOL)audio {
    int32_t index = 0;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        index = std::max(index, track.index + 1);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    const Ulid trackId = GenerateUlid();
    Operation operation =
        AddTrackOperation{trackId, audio ? "audio" : "video", index};
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        self.state->targetedTrackIds.insert(trackId);
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else {
        std::fprintf(stderr, "Track creation rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
    }
}

- (void)refreshTimelineAfterEditFromPosition:(RationalTime)position {
    self.state->interaction->ClearGapSelection();
    // FC4 -- points an edit took away leave trim mode.
    if (self.state->trimSession.Active() &&
        !self.state->trimSession.StillHolds(self.state->document))
        [self leaveTrimMode];
    [self rebuildVideoTrackIds];
    if (self.state->audioPlayback)
        self.state->audioPlayback->RebuildTimeline(self.state->document);
    self.state->duration = self.state->timeline->Duration();
    if (position < RationalTime{0, 1}) position = {0, 1};
    if (position > self.state->duration) position = self.state->duration;
    RationalTime quantized = QuantizePlayheadPosition(
        position, self.state->playheadResolution, [self playheadFrameRate]);
    if (quantized > self.state->duration) quantized = self.state->duration;
    [self refreshTimelineChrome];
    // PERF15 -- an edit while playing: the reader reads the new montage.
    if (self.state->playbackReader.Running()) [self syncPlaybackReader];
    [self requestResolvedPosition:quantized];
}

- (void)timelineMouseUp:(NSEvent*)event {
    if (self.videoFullscreenActive) return;
    if (self.trimMonitorDragging) {
        [self trimMonitorRelease];
        return;
    }
    // REM5 -- and ends there, as one operation.
    if (self.state->motionCanvas.gesture != 0) {
        [self motionCanvasMouseUp:event];
        return;
    }
    if (self.state->sourceMonitorActive &&
        self.window.firstResponder == self.sourceMonitorView)
        return;
    (void)event;
    if (self.state->markerStretch) {
        // D6 -- released: the span the pointer reached is one edit. A press
        // with no drag changes nothing.
        const auto stretch = *self.state->markerStretch;
        self.state->markerStretch.reset();
        self.state->overlayDirty = true;
        const DocumentMarker* marker =
            self.state->document.FindMarker(stretch.marker_id);
        if (!marker || !stretch.reached ||
            (stretch.reached->time == marker->time &&
             stretch.reached->duration == marker->duration))
            return;
        const UpdateMarkerOperation reached = *stretch.reached;
        if ([self applyMarkerOperation:reached]) {
            if (const DocumentMarker* stretched =
                    self.state->document.FindMarker(reached.marker_id))
                [self showMarkerInfo:*stretched];
        }
        return;
    }
    if (self.state->zoomBarDrag) {
        self.state->zoomBarDrag.reset();
        if (NavTraceEnabled()) {
            const TimelineZoomBarGeometry released =
                CalculateTimelineZoomBarGeometry(
                    self.state->viewport, self.state->duration,
                    self.metalView.bounds.size.width);
            std::fprintf(
                stderr,
                "nav bar-up thumb=%.1f..%.1f ruler=%.3fs pps=%.3f "
                "start=%.4fs\n",
                released.thumb_x, released.thumb_x + released.thumb_width,
                released.ruler_seconds, self.state->viewport.pixels_per_second,
                NavTraceStart(self.state->viewport));
        }
        self.state->overlayDirty = true;
        return;
    }
    self.state->interaction->SetPlayheadSnapTarget(std::nullopt);
    self.state->interaction->SetInOutSnapTargets(std::nullopt, std::nullopt);
    if (self.state->lassoCandidate) {
        const bool completedLasso = self.state->lassoDragging;
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        if (completedLasso) {
            self.state->scrubDragging = false;
            self.state->interaction->SetLinkedSelectionEnabled(
                self.state->linkedSelection);
            [self updateSelectionInfo];
            self.state->overlayDirty = true;
            return;
        }
    }
    if (self.state->navigationDragging || self.state->scrubDragging) {
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        [self applyToolCursor];
        return;
    }
    if (!self.state->editDragging) {
        self.state->duplicateDragging = false;
        self.state->duplicateDragClipboard.clear();
        self.state->interaction->SetLinkedSelectionEnabled(
            self.state->linkedSelection);
        return;
    }
    self.state->editDragging = false;
    const RationalTime playhead = self.state->requestedPosition;
    EditError error = EditError::None;
    std::string message;
    trim::TrimRefusal edgeRefusal = trim::TrimRefusal::None;
    const Document documentBeforeEdit = self.state->document;
    const EditLog logBeforeEdit = self.state->editLog;
    if (self.state->duplicateDragging) {
        const auto preview = self.state->interaction->MovePreview();
        self.state->interaction->CancelDrag();
        self.state->duplicateDragging = false;
        const auto operation =
            preview ? PasteTimelineClipboardAtMoves(
                          self.state->duplicateDragClipboard, *preview)
                    : std::nullopt;
        self.state->duplicateDragClipboard.clear();
        if (operation)
            [self applyTimelinePaste:*operation label:@"Clips dupliqués"];
        else
            self.infoLabel.stringValue = @"Duplication annulée";
    } else if (self.state->interaction->PointerUp(error, message)) {
        if (![self persistEditsInBackground:message]) {
            self.state->document = documentBeforeEdit;
            self.state->editLog = logBeforeEdit;
            std::fprintf(stderr, "Unable to persist edit: %s\n",
                         message.c_str());
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Modification non enregistrée : %@",
                                           CMStringFromUtf8(message)];
        }
        [self refreshTimelineAfterEditFromPosition:playhead];
    } else if (error != EditError::None) {
        std::fprintf(stderr, "Edit rejected (%s): %s\n", EditErrorName(error),
                     message.c_str());
    } else {
        edgeRefusal = [self selectPressedEdgePoint];
    }
    self.state->pressTarget.reset();
    [self updateSelectionInfo];
    if (edgeRefusal != trim::TrimRefusal::None)
        [self showTrimRefusal:edgeRefusal];
    else if (self.state->trimSession.Active())
        [self showTrimStatus];
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    self.state->overlayDirty = true;
}

- (void)timelineScroll:(NSEvent*)event {
    if (self.window.firstResponder == self.sourceMonitorView) return;
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    // Visual QA of 1 October 2026 -- what a scroll means and how far it
    // zooms are decided in TimelineView.h; AppKit only says whether the
    // deltas are precise and which modifiers are held. Option zooms on any
    // device: a trackpad's scroll was a pan and nothing else.
    const bool precise = event.hasPreciseScrollingDeltas;
    const TimelineScrollIntent intent = ResolveTimelineScrollIntent(
        event.scrollingDeltaX, event.scrollingDeltaY, precise,
        (event.modifierFlags & NSEventModifierFlagShift) != 0,
        (event.modifierFlags & NSEventModifierFlagOption) != 0,
        event.isDirectionInvertedFromDevice);
    if (std::abs(intent.delta) <= 0.0001) return;
    try {
        const TimelineViewport before = self.state->viewport;
        if (intent.action == TimelineScrollAction::Zoom) {
            // Around the playhead, wherever the pointer is: the zoom of
            // the editor's other program (TimelineView.h).
            self.state->zoomAnchor = ZoomTimelineAtPlayhead(
                self.state->viewport, self.state->requestedPosition,
                TimelineScrollZoomFactor(intent.delta, precise),
                self.state->duration, self.metalView.bounds.size.width,
                self.state->zoomAnchor);
        } else {
            self.state->viewport.ScrollByPixels(intent.delta,
                                                kTimelineViewRate);
        }
        if (NavTraceEnabled())
            std::fprintf(
                stderr,
                "nav scroll dx=%.3f dy=%.3f precise=%d inverted=%d shift=%d "
                "option=%d phase=%lu momentum=%lu x=%.1f -> %s %.3f | "
                "pps %.3f->%.3f start %.4fs->%.4fs | playhead %.3fs at "
                "x %.1f->%.1f\n",
                event.scrollingDeltaX, event.scrollingDeltaY, precise ? 1 : 0,
                event.isDirectionInvertedFromDevice ? 1 : 0,
                (event.modifierFlags & NSEventModifierFlagShift) != 0 ? 1 : 0,
                (event.modifierFlags & NSEventModifierFlagOption) != 0 ? 1 : 0,
                static_cast<unsigned long>(event.phase),
                static_cast<unsigned long>(event.momentumPhase), point.x,
                intent.action == TimelineScrollAction::Zoom ? "zoom" : "pan",
                intent.delta, before.pixels_per_second,
                self.state->viewport.pixels_per_second, NavTraceStart(before),
                NavTraceStart(self.state->viewport),
                static_cast<double>(self.state->requestedPosition.value) /
                    self.state->requestedPosition.rate,
                before.TimeToX(self.state->requestedPosition),
                self.state->viewport.TimeToX(self.state->requestedPosition));
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport update rejected: %s\n",
                     exception.what());
    }
}

- (void)timelineMagnify:(NSEvent*)event {
    if (self.window.firstResponder == self.sourceMonitorView) return;
    const NSPoint point = [self timelinePointForEvent:event];
    if (point.y < 0.0 || point.y > [self timelineHeight]) return;
    try {
        const TimelineViewport before = self.state->viewport;
        self.state->zoomAnchor = ZoomTimelineAtPlayhead(
            self.state->viewport, self.state->requestedPosition,
            std::exp(std::clamp(static_cast<double>(event.magnification), -1.0,
                                1.0)),
            self.state->duration, self.metalView.bounds.size.width,
            self.state->zoomAnchor);
        if (NavTraceEnabled())
            std::fprintf(stderr,
                         "nav pinch magnification=%.4f x=%.1f | pps "
                         "%.3f->%.3f start %.4fs->%.4fs\n",
                         static_cast<double>(event.magnification), point.x,
                         before.pixels_per_second,
                         self.state->viewport.pixels_per_second,
                         NavTraceStart(before),
                         NavTraceStart(self.state->viewport));
        self.state->overlayDirty = true;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Viewport magnification rejected: %s\n",
                     exception.what());
    }
}

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode, wired. The
// policy is TrimSession.h's: which point, which side, what each gesture
// plans and refuses, what the counters say. This reads the window's state
// into it and applies what it plans through the timeline's edit log, one
// operation per gesture, so that one undo takes a gesture back.
- (std::vector<Ulid>)targetedTrackList {
    return std::vector<Ulid>(self.state->targetedTrackIds.begin(),
                             self.state->targetedTrackIds.end());
}

// Selection (A) resizes, Trim (T) ripples (Resolve 21.1 manual, p.
// 1061-1062); linked partners follow while the linked selection is on.
- (void)syncTrimStyle {
    self.state->trimSession.SetStyle([self effectiveTool] == TimelineTool::Trim
                                         ? trim::TrimStyle::Ripple
                                         : trim::TrimStyle::Trim);
    self.state->trimSession.SetFollowLinks(self.state->linkedSelection);
}

- (const Document&)trimShownDocument {
    return self.state->trimPreview ? *self.state->trimPreview
                                   : self.state->document;
}

- (void)showTrimStatus {
    const trim::TrimMonitorView view = trim::DescribeTrimMonitor(
        [self trimShownDocument], self.state->trimSession);
    if (!view.active) return;
    const trim::TrimMonitorText text = trim::DescribeTrimMonitorText(view);
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@ · A %@ · B %@%@", CMStringFromUtf8(text.status),
                         CMStringFromUtf8(text.outgoing_shift),
                         CMStringFromUtf8(text.incoming_shift),
                         self.state->dynamicTrimMode ? @" · W" : @""];
    [self refreshTrimBar];
}

- (void)showTrimRefusal:(trim::TrimRefusal)refusal {
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Trim refusé : %@",
                                   CMStringFromUtf8(
                                       trim::TrimRefusalDescription(refusal))];
}

- (void)leaveTrimMode {
    if (!self.state->trimSession.Active() && !self.state->trimPreview) return;
    self.state->trimSession.Clear();
    self.state->trimPreview.reset();
    self.state->overlayDirty = true;
    // Record is the program again.
    [self requestResolvedPosition:self.state->requestedPosition];
    [self refreshTrimBar];
}

#pragma mark - ON9 trim gestures

// ON9 -- docs/roadmap/ONGLETS-2026-10.md. The cursor of an edge, Resolve's:
// "]" on the outgoing clip's tail, "[" on the incoming clip's head, "]["
// where the cut rolls. A stretch keeps AppKit's double arrow.
- (NSCursor*)edgeCursorFor:(const TimelinePointerTarget&)target {
    if (TimelineTrimModeStretches(target.trim_mode))
        return NSCursor.resizeLeftRightCursor;
    if (target.trim_mode == TimelineTrimMode::Roll)
        return CMTrimCursor(CMTrimCursorRoll);
    return CMTrimCursor(target.edge == TimelineHitEdge::Tail ? CMTrimCursorOut
                                                             : CMTrimCursorIn);
}

#pragma mark - ON8 trim bar

// ON8 -- docs/roadmap/ONGLETS-2026-10.md. Between the transport letters
// (J K L end at 294 points) and the timecode (102 points from the right
// edge), in the tool row along the top of the timeline.
- (void)layOutTrimBar {
    if (!self.trimBarView || !self.metalView) return;
    const NSRect bounds = self.metalView.bounds;
    constexpr CGFloat kLeft = 304.0;
    constexpr CGFloat kRight = 108.0;
    self.trimBarView.frame =
        NSMakeRect(kLeft, bounds.size.height - kTimelineToolbarHeight,
                   std::max<CGFloat>(0.0, bounds.size.width - kLeft - kRight),
                   kTimelineToolbarHeight);
}

// What the bar shows now: the trim mode's state, with the keys bound to its
// commands as the menus write them.
- (void)refreshTrimBar {
    if (!self.trimBarView || !self.state) return;
    NSDictionary<NSString*, NSString*>* bindings = self.shortcutBindings;
    const trim::TrimBarModel model = trim::DescribeTrimBar(
        self.state->trimSession, self.state->document,
        [self effectiveTool] == TimelineTool::Trim, self.state->dynamicTrimMode,
        [bindings](const std::string& command) {
            NSString* spec = bindings[@(command.c_str())];
            return spec ? ui::shortcuts::DisplaySpec(spec.UTF8String ?: "")
                        : std::string();
        });
    [self.trimBarView showModel:model];
}

// ON9 -- Resolve's Play Around Current Selection (manual 21.1, p. 1070):
// from two seconds before to two seconds after the selected edit point, the
// selected clips, or the playhead, then back to where it started -- the
// cut, judged again after each nudge. Resolve's pre- and post-roll are
// preferences; these two seconds are not yet.
- (void)playAroundSelection {
    if (!self.state || self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    const MediaRate rate = self.state->document.sequence.frame_rate;
    const RationalTime roll{2, 1};
    RationalTime from = self.state->requestedPosition;
    RationalTime to = from;
    NSString* what = @"la tête de lecture";
    const trim::TrimMonitorView view = trim::DescribeTrimMonitor(
        self.state->document, self.state->trimSession);
    if (view.active) {
        from = to = view.edit_time;
        what = @"le raccord";
    } else if (!self.state->interaction->SelectedClipIds().empty()) {
        bool first = true;
        for (const Ulid& id : self.state->interaction->SelectedClipIds()) {
            const DocumentClip* clip = self.state->document.FindClip(id);
            if (!clip) continue;
            const RationalTime end = clip->timeline_in.add(clip->duration);
            from = first || clip->timeline_in < from ? clip->timeline_in : from;
            to = first || end > to ? end : to;
            first = false;
        }
        what = @"la sélection";
    }
    RationalTime start = from.sub(roll);
    if (start < RationalTime{0, 1}) start = {0, rate.num > 0 ? rate.num : 1};
    RationalTime end = to.add(roll);
    if (end > self.state->duration) end = self.state->duration;
    self.playAroundReturn =
        view.active ? view.edit_time : self.state->requestedPosition;
    self.playAroundStart = start;
    self.playAroundEnd = end;
    [self requestTimelinePosition:start];
    [self setPlaybackDirection:1];
    self.playAroundActive = YES;
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"Lecture autour de %@%@", what,
                                   self.loopPlayback ? @", en boucle" : @""];
}

// Past the window's end: again from its start in a loop, otherwise stopped
// and the playhead back where it was asked from.
- (void)checkPlayAround {
    if (!self.playAroundActive || !self.state ||
        self.state->playbackDirection <= 0 ||
        self.state->requestedPosition < self.playAroundEnd)
        return;
    if (self.loopPlayback) {
        const RationalTime start = self.playAroundStart;
        const RationalTime end = self.playAroundEnd;
        const RationalTime back = self.playAroundReturn;
        [self requestTimelinePosition:start];
        [self setPlaybackDirection:1];
        self.playAroundStart = start;
        self.playAroundEnd = end;
        self.playAroundReturn = back;
        self.playAroundActive = YES;
        return;
    }
    const RationalTime back = self.playAroundReturn;
    [self setPlaybackDirection:0];
    [self requestTimelinePosition:back];
}

// ON9 -- the Trim Monitor under the hand, as Resolve's Trim Editor
// (manual 21.1, p. 1067) and Avid's two-up: a press on the left image takes
// A, on the right image B, on the divide between them a roll; the drag
// moves it, six points a frame, previewed on the monitor and the timeline,
// and the release writes it in one edit. Past what the clips can give, it
// holds at the last frame that applies.
static constexpr CGFloat kTrimMonitorDivide = 24.0;
static constexpr CGFloat kTrimMonitorPointsPerFrame = 6.0;

- (BOOL)trimMonitorPressAt:(NSPoint)point {
    if (!self.state || !self.state->trimSession.Active() ||
        self.state->trimMonitorPanes.empty() ||
        self.state->trimSession.Dynamic().active ||
        self.state->playbackDirection != 0)
        return NO;
    const CGFloat middle = NSMidX(self.programMonitorView.bounds);
    const trim::TrimSide side =
        point.x < middle - kTrimMonitorDivide   ? trim::TrimSide::A
        : point.x > middle + kTrimMonitorDivide ? trim::TrimSide::B
                                                : trim::TrimSide::Roll;
    if (self.state->trimSession.SetSide(self.state->document, side) !=
        trim::TrimRefusal::None) {
        [self showTrimRefusal:trim::TrimRefusal::NoJunction];
        return YES;
    }
    // The style the release will trim with, for the preview too.
    [self syncTrimStyle];
    self.trimMonitorDragging = YES;
    self.trimMonitorDragStartX = point.x;
    self.trimMonitorDragFrames = 0;
    self.state->trimPreview.reset();
    [self showTrimStatus];
    self.state->overlayDirty = true;
    return YES;
}

// The farthest of `wanted` frames, toward zero, the session's plan applies:
// a document showing it, or none at zero.
- (int64_t)trimMonitorPreviewFrames:(int64_t)wanted
                           document:(Document&)preview {
    for (int64_t frames = wanted; frames != 0; frames += wanted > 0 ? -1 : 1) {
        const trim::TrimEdit edit =
            self.state->trimSession.PlanNudge(self.state->document, frames);
        if (!edit.ok() || !edit.operation) continue;
        Document candidate = self.state->document;
        Operation operation = *edit.operation;
        Operation inverse = RemoveClipOperation{};
        EditError error = EditError::None;
        std::string message;
        if (ApplyOperation(candidate, operation, inverse, error, message)) {
            preview = std::move(candidate);
            return frames;
        }
    }
    return 0;
}

- (void)trimMonitorDragTo:(NSPoint)point {
    if (!self.trimMonitorDragging || !self.state) return;
    const int64_t wanted = static_cast<int64_t>(std::lround(
        (point.x - self.trimMonitorDragStartX) / kTrimMonitorPointsPerFrame));
    Document preview;
    const int64_t frames = [self trimMonitorPreviewFrames:wanted
                                                 document:preview];
    if (frames == self.trimMonitorDragFrames) return;
    self.trimMonitorDragFrames = frames;
    if (frames == 0)
        self.state->trimPreview.reset();
    else
        self.state->trimPreview = std::move(preview);
    [self requestResolvedPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"TRIM MONITOR · %@ image%@ · relâcher pour écrire",
                         CMStringFromUtf8(trim::SignedFrames(frames)),
                         frames == 1 || frames == -1 ? @"" : @"s"];
    self.state->overlayDirty = true;
}

- (void)trimMonitorRelease {
    if (!self.trimMonitorDragging || !self.state) return;
    self.trimMonitorDragging = NO;
    const int64_t frames = self.trimMonitorDragFrames;
    self.trimMonitorDragFrames = 0;
    self.state->trimPreview.reset();
    if (frames != 0) {
        [self syncTrimStyle];
        [self applyTrimEdit:self.state->trimSession.PlanNudge(
                                self.state->document, frames)];
    } else {
        [self requestResolvedPosition:self.state->requestedPosition];
        [self showTrimStatus];
    }
    self.state->overlayDirty = true;
}

- (void)menuPlayAround:(id)sender {
    (void)sender;
    [self playAroundSelection];
}

- (void)menuToggleLoop:(id)sender {
    (void)sender;
    self.loopPlayback = !self.loopPlayback;
    self.infoLabel.stringValue = self.loopPlayback
                                     ? @"Lecture en boucle : activée"
                                     : @"Lecture en boucle : désactivée";
}

- (void)trimBar:(CMTrimBarView*)bar didPress:(NSString*)action {
    (void)bar;
    const std::string name = action.UTF8String ?: "";
    if (name == "select")
        [self trimSelectNearest];
    else if (name == "side.a")
        [self setTrimSide:trim::TrimSide::A];
    else if (name == "side.roll")
        [self setTrimSide:trim::TrimSide::Roll];
    else if (name == "side.b")
        [self setTrimSide:trim::TrimSide::B];
    else if (name == "nudge.-5")
        [self trimNudge:-trim::kLargeTrimFrames];
    else if (name == "nudge.-1")
        [self trimNudge:-1];
    else if (name == "nudge.+1")
        [self trimNudge:1];
    else if (name == "nudge.+5")
        [self trimNudge:trim::kLargeTrimFrames];
    else if (name == "extend")
        [self trimExtendToPlayhead];
    else if (name == "dynamic")
        [self toggleDynamicTrim];
    else if (name == "leave")
        [self leaveTrimMode];
    else if (name == "play.around")
        [self playAroundSelection];
    [self refreshTrimBar];
    // The keys go on reaching the timeline.
    [self.window makeFirstResponder:self.metalView];
}

// The bar's A, Roll and B: the side chosen outright, as U cycles to it.
- (void)setTrimSide:(trim::TrimSide)side {
    const trim::TrimRefusal refusal =
        self.state->trimSession.SetSide(self.state->document, side);
    self.state->overlayDirty = true;
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    [self showTrimStatus];
}

// Applies what the session planned; a refusal is said, nothing written.
- (BOOL)applyTrimEdit:(const trim::TrimEdit&)edit {
    if (!edit.ok()) {
        if (!edit.detail.empty())
            std::fprintf(stderr, "Trim refused (%s): %s\n",
                         trim::TrimRefusalName(edit.refusal),
                         edit.detail.c_str());
        [self showTrimRefusal:edit.refusal];
        return NO;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:*edit.operation
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Trim rejected (%s): %s\n", EditErrorName(error),
                     message.c_str());
        [self showTrimRefusal:trim::TrimRefusal::Rejected];
        return NO;
    }
    self.state->trimSession.RecordApplied(edit);
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    [self showTrimStatus];
    self.state->overlayDirty = true;
    return YES;
}

// V -- the edit point nearest the playhead on the targeted tracks, as a
// roll, and the playhead goes to it (manual, p. 1069).
- (void)trimSelectNearest {
    if (self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    [self setPlaybackDirection:0];
    RationalTime editTime;
    const trim::TrimRefusal refusal = self.state->trimSession.SelectNearestEdit(
        self.state->document, self.state->requestedPosition,
        [self targetedTrackList], trim::TrimSide::Roll, editTime);
    self.state->trimPreview.reset();
    self.state->overlayDirty = true;
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return;
    }
    [self syncTrimStyle];
    [self requestTimelinePosition:editTime];
    [self showTrimStatus];
}

// U -- outgoing, centred, incoming (manual, p. 1070).
- (void)trimCycleSide {
    const trim::TrimRefusal refusal =
        self.state->trimSession.CycleSide(self.state->document);
    self.state->overlayDirty = true;
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return;
    }
    [self requestResolvedPosition:self.state->requestedPosition];
    [self showTrimStatus];
}

// Comma, period, and with Shift five frames (manual, p. 1069-1070).
- (void)trimNudge:(int64_t)frames {
    if (self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    [self syncTrimStyle];
    [self applyTrimEdit:self.state->trimSession.PlanNudge(self.state->document,
                                                          frames)];
}

// E -- the selected points to the playhead (manual, p. 1088-1089).
- (void)trimExtendToPlayhead {
    if (self.state->trimSession.Dynamic().active) {
        NSBeep();
        return;
    }
    [self syncTrimStyle];
    [self
        applyTrimEdit:self.state->trimSession.PlanExtendToPlayhead(
                          self.state->document, self.state->requestedPosition)];
}

// The clock a dynamic trim is counted on, as an exact time: microseconds
// of the monotonic clock. TrimSession reads no clock of its own.
- (RationalTime)trimClockNow {
    const int64_t microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return RationalTime{microseconds, 1000000};
}

// W -- Resolve's Dynamic Trim mode (manual, p. 1085-1086). On, with no
// point selected, the edit nearest the playhead is; off, a trim still
// running is dropped and nothing is written.
- (void)toggleDynamicTrim {
    if (self.state->dynamicTrimMode) {
        self.state->dynamicTrimMode = false;
        if (self.state->trimSession.CancelDynamic()) {
            self.state->trimPreview.reset();
            [self requestResolvedPosition:self.state->requestedPosition];
        }
        self.state->overlayDirty = true;
        if (self.state->trimSession.Active())
            [self showTrimStatus];
        else
            self.infoLabel.stringValue = @"Trim dynamique désactivé";
        return;
    }
    [self setPlaybackDirection:0];
    self.state->dynamicTrimMode = true;
    if (!self.state->trimSession.Active()) [self trimSelectNearest];
    if (self.state->trimSession.Active())
        [self showTrimStatus];
    else
        self.infoLabel.stringValue =
            @"Trim dynamique : aucun point de montage, J K L lisent";
    self.state->overlayDirty = true;
}

// J (-1), L (+1) and K (0) while the dynamic mode holds points: they trim
// instead of playing, and K commits what was reached as one edit. NO when
// the keys are the transport's.
- (BOOL)dynamicTrimTransport:(int)direction {
    if (!self.state->dynamicTrimMode || !self.state->trimSession.Active())
        return NO;
    const RationalTime now = [self trimClockNow];
    if (direction == 0) {
        if (!self.state->trimSession.Dynamic().active) return YES;
        const trim::TrimEdit edit =
            self.state->trimSession.StopDynamic(self.state->document, now);
        self.state->trimPreview.reset();
        self.state->overlayDirty = true;
        if (edit.refusal == trim::TrimRefusal::NoMovement) {
            [self requestResolvedPosition:self.state->requestedPosition];
            [self showTrimStatus];
            return YES;
        }
        [self applyTrimEdit:edit];
        return YES;
    }
    if (!self.state->trimSession.Dynamic().active) [self syncTrimStyle];
    if (self.state->playbackDirection != 0) [self setPlaybackDirection:0];
    const trim::TrimRefusal refusal =
        self.state->trimSession.Shuttle(self.state->document, direction, now);
    if (refusal != trim::TrimRefusal::None) {
        [self showTrimRefusal:refusal];
        return YES;
    }
    [self refreshDynamicTrim];
    return YES;
}

// While J or L runs: the offset follows the clock, and the timeline and
// Record draw the document as it would be.
- (void)refreshDynamicTrim {
    if (!self.state->trimSession.Dynamic().active) return;
    self.state->trimSession.Tick(self.state->document, [self trimClockNow]);
    Document preview;
    if (self.state->trimSession.PreviewDocument(self.state->document, preview))
        self.state->trimPreview = std::move(preview);
    else
        self.state->trimPreview.reset();
    [self requestResolvedPosition:self.state->requestedPosition];
    [self showTrimStatus];
    self.state->overlayDirty = true;
}

// A click on a clip's edge that moved nothing: its point, on the side the
// edge gives (Command rolls), added to the others with Shift.
- (trim::TrimRefusal)selectPressedEdgePoint {
    if (!self.state->pressTarget ||
        self.state->pressTarget->kind != TimelinePointerTargetKind::ClipEdge ||
        TimelineTrimModeStretches(self.state->pressTarget->trim_mode))
        return trim::TrimRefusal::None;
    const TimelinePointerTarget target = *self.state->pressTarget;
    const TrimEdge edge =
        target.edge == TimelineHitEdge::Head ? TrimEdge::Head : TrimEdge::Tail;
    if (self.state->trimSession.Dynamic().active)
        return trim::TrimRefusal::Rejected;
    const trim::TrimRefusal refusal = self.state->trimSession.SelectEdge(
        self.state->document, target.clip_id, edge,
        trim::TrimSideOfEdge(edge, target.trim_mode == TimelineTrimMode::Roll),
        self.state->pressShift);
    self.state->trimPreview.reset();
    self.state->overlayDirty = true;
    if (refusal == trim::TrimRefusal::None) {
        [self syncTrimStyle];
        [self requestResolvedPosition:self.state->requestedPosition];
    }
    return refusal;
}

- (BOOL)timelineKeyDown:(NSEvent*)event {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    // REM5 -- on Record, with a motion node selected: the arrows nudge it,
    // Delete removes it, Escape lets it go.
    if (self.window.firstResponder == self.programMonitorView &&
        !self.state->motionCanvas.selected.empty() &&
        [self motionCanvasKeyDown:event]) {
        return YES;
    }
    if (self.videoFullscreenActive && event.keyCode == 53) {
        [self toggleFullscreen:nil];
        return YES;
    }
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        // Out of lot, 29 October 2026 -- the source monitor plays: Space,
        // J, K and L act on it while it is the active viewer, as Resolve's
        // transport keys act on the viewer that has the focus.
        if (source && [self event:event matchesShortcut:@"play.toggle"]) {
            if (!event.isARepeat)
                [self setSourcePlaybackDirection:
                          self.state->sourcePlaybackDirection != 0 ? 0 : 1];
            return YES;
        }
        if (source && [self event:event matchesShortcut:@"play.forward"]) {
            [self setSourcePlaybackDirection:NextShuttleSpeed(
                                                 self.state
                                                     ->sourcePlaybackDirection,
                                                 1)];
            return YES;
        }
        if (source && [self event:event matchesShortcut:@"play.reverse"]) {
            [self setSourcePlaybackDirection:NextShuttleSpeed(
                                                 self.state
                                                     ->sourcePlaybackDirection,
                                                 -1)];
            return YES;
        }
        if (source && [self event:event matchesShortcut:@"play.stop"]) {
            [self setSourcePlaybackDirection:0];
            return YES;
        }
        // A step, a jump to either end: the player stops where asked.
        if (source && (event.keyCode == 123 || event.keyCode == 124 ||
                       event.keyCode == 115 || event.keyCode == 119))
            [self setSourcePlaybackDirection:0];
        if (source && (event.keyCode == 123 || event.keyCode == 124)) {
            // Shift moves by one timecode second, as in Resolve.
            const int64_t amount = (modifiers & NSEventModifierFlagShift) != 0
                                       ? NominalTimecodeFps(source->rate)
                                       : 1;
            const int64_t direction = event.keyCode == 123 ? -1 : 1;
            [self requestSourcePosition:self.state->sourceMonitorPosition.add(
                                            {direction * amount *
                                                 source->rate.den,
                                             source->rate.num})];
            return YES;
        }
        if (source && event.keyCode == 115) {
            [self requestSourcePosition:{0, source->duration.rate}];
            return YES;
        }
        if (source && event.keyCode == 119) {
            [self requestSourcePosition:source->duration];
            return YES;
        }
    }
    [self dismissAgentMarks];  // PRD25
    NSString* characters = event.charactersIgnoringModifiers.lowercaseString;
    if ([self event:event matchesShortcut:@"view.fullscreen"]) {
        [self toggleFullscreen:nil];
        return YES;
    }
    // G6 -- the gesture a colourist makes before trusting a grade.
    if ([self event:event matchesShortcut:@"color.bypass"]) {
        [self toggleGradeBypass:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"color.reference.grab"]) {
        [self grabReferenceStill:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"color.reference.split"]) {
        [self toggleReferenceSplit:nil];
        return YES;
    }
    // ON9 -- the slash plays around the selection, Command-slash loops.
    if ([self event:event matchesShortcut:@"play.loop"]) {
        if (!event.isARepeat) [self menuToggleLoop:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.around"]) {
        if (!event.isARepeat) [self playAroundSelection];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.toggle"]) {
        if ([characters isEqualToString:@" "]) {
            if (!event.isARepeat) self.state->spaceUsedForPan = false;
            self.state->spaceHand = true;
            [self applyToolCursor];
        } else if (!event.isARepeat) {
            [self menuPlayPause:nil];
        }
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.linked"]) {
        [self menuToggleLinkedSelection:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.copy"]) {
        [self menuCopyTimelineClips:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.paste"]) {
        [self menuPasteTimelineClips:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"transition.add"]) {
        [self menuAddCrossDissolve:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.ripple"]) {
        [self deleteCurrentTimelineSelectionRipple:YES];
        return YES;
    }
    if ([self event:event matchesShortcut:@"edit.delete"]) {
        [self deleteCurrentTimelineSelectionRipple:NO];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.in"]) {
        [self menuMarkTimelineIn:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.out"]) {
        [self menuMarkTimelineOut:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"mark.clear"]) {
        [self menuClearTimelineRange:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"source.insert"]) {
        [self menuInsertSource:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"source.overwrite"]) {
        [self menuOverwriteSource:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.select"]) {
        [self setTimelineTool:TimelineTool::Select];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.hand"]) {
        [self setTimelineTool:TimelineTool::Hand];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.zoom"]) {
        [self setTimelineTool:TimelineTool::Zoom];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.cut"] ||
        [self event:event matchesShortcut:@"tool.cut.alternate"]) {
        [self setTimelineTool:TimelineTool::Cut];
        return YES;
    }
    if ([self event:event matchesShortcut:@"tool.trim"]) {
        [self setTimelineTool:TimelineTool::Trim];
        return YES;
    }
    // FC4 -- the trim mode's keys, Resolve's (KeyboardShortcuts.h).
    if ([self event:event matchesShortcut:@"trim.select"]) {
        if (!event.isARepeat) [self trimSelectNearest];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.side"]) {
        if (!event.isARepeat) [self trimCycleSide];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.back"]) {
        [self trimNudge:-1];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.forward"]) {
        [self trimNudge:1];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.back.many"]) {
        [self trimNudge:-trim::kLargeTrimFrames];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.nudge.forward.many"]) {
        [self trimNudge:trim::kLargeTrimFrames];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.extend"]) {
        if (!event.isARepeat) [self trimExtendToPlayhead];
        return YES;
    }
    if ([self event:event matchesShortcut:@"trim.dynamic"]) {
        if (!event.isARepeat) [self toggleDynamicTrim];
        return YES;
    }
    // FC3 -- Command-R, before the Command keys below claim it.
    if ([self event:event matchesShortcut:@"clip.retime"]) {
        if (!event.isARepeat) [self toggleRetimeControls];
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.fit"]) {
        [self menuFitTimeline:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"timeline.snapping"]) {
        [self menuToggleSnapping:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"select.after"]) {
        [self menuSelectClipsAfter:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"clip.enable"]) {
        if (!event.isARepeat) [self menuToggleClipsEnabled:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.reverse"]) {
        if ([self dynamicTrimTransport:-1]) return YES;
        [self setPlaybackDirection:NextShuttleSpeed(
                                       self.state->playbackDirection, -1)];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.stop"]) {
        [self menuStop:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"play.forward"]) {
        if ([self dynamicTrimTransport:1]) return YES;
        [self setPlaybackDirection:NextShuttleSpeed(
                                       self.state->playbackDirection, 1)];
        return YES;
    }
    const bool zoomKey = [characters isEqualToString:@"+"] ||
                         [characters isEqualToString:@"="] ||
                         [characters isEqualToString:@"-"];
    if ((modifiers & NSEventModifierFlagCommand) != 0) {
        if ([characters isEqualToString:@"t"] &&
            (modifiers & NSEventModifierFlagShift) != 0) {
            [self addTrack:(modifiers & NSEventModifierFlagOption) != 0];
            return YES;
        }
        // Resolve zooms the timeline with Cmd+= and Cmd+- (its + and - alone
        // start a relative timecode, which this window has not).
        if (zoomKey && (modifiers & (NSEventModifierFlagOption |
                                     NSEventModifierFlagControl)) == 0) {
            [self zoomTimelineAtPlayheadOut:[characters isEqualToString:@"-"]];
            return YES;
        }
        if (![characters isEqualToString:@"z"]) return NO;
        if ((modifiers & NSEventModifierFlagShift) != 0)
            [self menuRedo:nil];
        else
            [self menuUndo:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"marker.add"]) {
        if (!event.isARepeat) [self menuAddMarker:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"marker.note"]) {
        if (!event.isARepeat) [self menuAnnotateMarker:nil];
        return YES;
    }
    if ([self event:event matchesShortcut:@"playhead.resolution"]) {
        self.state->playheadResolution =
            self.state->playheadResolution == PlayheadResolution::Frame
                ? PlayheadResolution::Sample
                : PlayheadResolution::Frame;
        if (self.state->playbackDirection == 0)
            [self requestTimelinePosition:self.state->requestedPosition];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
        return YES;
    }
    if (zoomKey) {
        [self zoomTimelineAtPlayheadOut:[characters isEqualToString:@"-"]];
        return YES;
    }
    if (event.keyCode == 115) {  // Home
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        return YES;
    }
    if (event.keyCode == 119) {  // End
        [self requestTimelinePosition:self.state->duration];
        return YES;
    }
    if (event.keyCode == 126 || event.keyCode == 125) {  // Up / down
        const bool forward = event.keyCode == 125;
        // D2 -- with Shift, from marker to marker instead of cut to cut.
        if ((modifiers & NSEventModifierFlagShift) != 0) {
            [self goToAdjacentMarkerForward:forward];
            return YES;
        }
        [self setPlaybackDirection:0];
        const auto edit = AdjacentTimelineEdit(
            self.state->document, self.state->requestedPosition, forward);
        if (edit) [self requestTimelinePosition:*edit];
        return YES;
    }
    if (event.keyCode == 123 || event.keyCode == 124) {  // Left / right
        const int64_t direction = event.keyCode == 123 ? -1 : 1;
        if ((modifiers & NSEventModifierFlagShift) != 0) {
            [self stepPlayheadSeconds:direction];
        } else {
            [self stepPlayheadFrames:direction];
        }
        return YES;
    }
    // D7 -- docs/roadmap/DIALOGUE-2026-10.md. Return puts the keyboard in
    // the text of the one carton selected; on any other selection the key
    // stays unhandled.
    if ((event.keyCode == 36 || event.keyCode == 76) &&
        (modifiers & kShortcutModifierMask) == 0) {
        if (const auto carton = ui::media_panel::SelectedCartonForText(
                self.state->document,
                self.state->interaction->SelectedClipIds())) {
            const Ulid edited = *carton;
            if (!event.isARepeat) [self editCartonText:edited];
            return YES;
        }
    }
    if (event.keyCode == 53) {  // Escape
        // FC4 -- a dynamic trim running is dropped, nothing written; else
        // the trim mode is left.
        if (self.state->trimSession.CancelDynamic()) {
            self.state->trimPreview.reset();
            [self requestResolvedPosition:self.state->requestedPosition];
            [self showTrimStatus];
            self.state->overlayDirty = true;
            return YES;
        }
        if (self.state->trimSession.Active()) {
            [self leaveTrimMode];
            self.infoLabel.stringValue = @"Mode trim quitté";
        }
        // FC3 -- and the Retime controls close (Resolve 21.1 manual, p.
        // 1222).
        self.state->retimeClipIds.clear();
        self.state->interaction->CancelDrag();
        self.state->markerStretch.reset();
        self.state->navigationDragging = false;
        self.state->scrubDragging = false;
        self.state->editDragging = false;
        self.state->duplicateDragging = false;
        self.state->duplicateDragClipboard.clear();
        self.state->lassoCandidate = false;
        self.state->lassoDragging = false;
        self.state->overlayDirty = true;
        return YES;
    }
    return NO;
}

- (void)timelineKeyUp:(NSEvent*)event {
    if ([event.charactersIgnoringModifiers isEqualToString:@" "] &&
        [self event:event matchesShortcut:@"play.toggle"]) {
        self.state->spaceHand = false;
        [self applyToolCursor];
        if (!self.state->spaceUsedForPan) {
            [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1
                                                                          : 0];
        }
    }
}

// GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The selected
// shot's whole stack, held by the window until it is pasted.
- (void)copyClipGrade:(id)sender {
    (void)sender;
    if (!self.state || !self.state->interaction) return;
    const std::vector<Ulid> selected =
        self.state->interaction->SelectedClipIds();
    const DocumentClip* clip =
        selected.size() == 1 ? self.state->document.FindClip(selected.front())
                             : nullptr;
    if (clip == nullptr ||
        !ClipGradeRefusal(self.state->document, *clip).empty()) {
        NSBeep();
        return;
    }
    self.state->gradeClipboard = clip->effects;
    self.infoLabel.stringValue =
        clip->effects.empty()
            ? @"Étalonnage copié : aucun étage (le coller remet à zéro)"
            : [NSString
                  stringWithFormat:@"Étalonnage copié : %lu étage%@",
                                   (unsigned long)clip->effects.size(),
                                   clip->effects.size() == 1 ? @"" : @"s"];
}

// The copied stack on every selected shot that carries a grade, in one
// undo step; the others are left and counted.
- (void)pasteClipGrade:(id)sender {
    (void)sender;
    if (!self.state || !self.state->interaction ||
        !self.state->gradeClipboard) {
        NSBeep();
        return;
    }
    ClipGradePaste paste;
    std::string message;
    if (!BuildClipGradePasteOfStack(
            self.state->document, *self.state->gradeClipboard,
            self.state->interaction->SelectedClipIds(), paste, message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Étalonnage non collé : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
        return;
    }
    if (paste.operation) {
        EditError error = EditError::None;
        if (![self applyAndPersistTimelineOperation:*paste.operation
                                              error:error
                                            message:message]) {
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Étalonnage refusé : %@",
                                           CMStringFromUtf8(message)];
            NSBeep();
            return;
        }
        self.state->overlayDirty = true;
        [self updateSelectionInfo];
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:
            @"Étalonnage collé sur %lu plan%@%@",
            (unsigned long)paste.pasted.size(),
            paste.pasted.size() == 1 ? @"" : @"s",
            paste.skipped.empty()
                ? @""
                : [NSString
                      stringWithFormat:@" (%lu sans étalonnage possible)",
                                       (unsigned long)paste.skipped.size()]];
}

- (void)menuCopyTimelineClips:(id)sender {
    (void)sender;
    std::vector<Ulid> clipIds = self.state->interaction->SelectedClipIds();
    if (self.state->linkedSelection)
        clipIds = ExpandLinkedClipSelection(self.state->document, clipIds);
    self.state->timelineClipboard =
        CopyTimelineClips(self.state->document, clipIds);
    if (self.state->timelineClipboard.empty()) {
        self.infoLabel.stringValue = @"Aucun clip à copier";
        return;
    }
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%lu clip%@ copié%@",
                         (unsigned long)self.state->timelineClipboard.size(),
                         self.state->timelineClipboard.size() == 1 ? @"" : @"s",
                         self.state->timelineClipboard.size() == 1 ? @""
                                                                   : @"s"];
}

- (BOOL)applyTimelinePaste:(PasteClipsOperation)operation
                     label:(NSString*)label {
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (!stagedLog.Apply(stagedDocument, Operation{std::move(operation)}, error,
                         message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"%@ refusé (%@) : %@", label,
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    const auto& stored =
        std::get<PasteClipsOperation>(stagedLog.AppliedEntries().back().op);
    std::vector<Ulid> pastedIds;
    pastedIds.reserve(stored.clips.size());
    for (const PastedClip& clip : stored.clips)
        pastedIds.push_back(clip.clip_id);
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        std::fprintf(stderr, "Unable to persist paste: %s\n", message.c_str());
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"%@ impossible : %@", label,
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    self.state->interaction->SelectClips(pastedIds);
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%@ · %lu clip%@", label,
                                   (unsigned long)pastedIds.size(),
                                   pastedIds.size() == 1 ? @"" : @"s"];
    self.state->overlayDirty = true;
    return YES;
}

- (void)menuPasteTimelineClips:(id)sender {
    (void)sender;
    const auto operation = PasteTimelineClipboardAt(
        self.state->document, self.state->timelineClipboard,
        self.state->requestedPosition);
    if (!operation) {
        self.infoLabel.stringValue =
            @"Collage impossible : aucune piste compatible";
        return;
    }
    PasteClipsOperation overwrite = *operation;
    overwrite.overwrite = true;
    [self applyTimelinePaste:std::move(overwrite)
                       label:@"Clips collés en overwrite"];
}

- (void)menuUndo:(id)sender {
    (void)sender;
    [self dismissAgentMarks];  // PRD25
    // FC4 -- the counters of the trim mode say what was trimmed since its
    // points were selected; an undo would make them lie. Left, as Escape.
    [self leaveTrimMode];
    EditError error = EditError::None;
    std::string message;
    const bool projectHistory =
        (self.state->lastHistoryDomain == HistoryDomain::Project &&
         self.state->projectEditLog.AppliedCount() > 0) ||
        (self.state->editLog.AppliedCount() == 0 &&
         self.state->projectEditLog.AppliedCount() > 0);
    if (projectHistory) {
        const bool renameOnly =
            std::holds_alternative<RenameProjectItemOperation>(
                self.state->projectEditLog.AppliedEntries().back().op);
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Undo(candidate, error, message)) {
            if (error != EditError::EmptyUndo)
                std::fprintf(stderr, "Project undo failed (%s): %s\n",
                             EditErrorName(error), message.c_str());
            return;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            std::fprintf(stderr, "Unable to persist project undo: %s\n",
                         message.c_str());
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        if (renameOnly)
            [self refreshAfterProjectRename];
        else
            [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (stagedLog.Undo(stagedDocument, error, message)) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message]) {
            std::fprintf(stderr, "Unable to persist undo: %s\n",
                         message.c_str());
            return;
        }
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else if (error != EditError::EmptyUndo) {
        std::fprintf(stderr, "Undo failed (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
}

- (void)menuRedo:(id)sender {
    (void)sender;
    [self dismissAgentMarks];  // PRD25
    [self leaveTrimMode];      // FC4, as for an undo.
    EditError error = EditError::None;
    std::string message;
    const bool projectHistory =
        (self.state->lastHistoryDomain == HistoryDomain::Project &&
         self.state->projectEditLog.UndoneCount() > 0) ||
        (self.state->editLog.UndoneCount() == 0 &&
         self.state->projectEditLog.UndoneCount() > 0);
    if (projectHistory) {
        const bool renameOnly =
            std::holds_alternative<RenameProjectItemOperation>(
                self.state->projectEditLog.UndoneEntries().back().op);
        Project candidate = self.state->project;
        ProjectEditLog projectLog = self.state->projectEditLog;
        if (!projectLog.Redo(candidate, error, message)) {
            if (error != EditError::EmptyRedo)
                std::fprintf(stderr, "Project redo failed (%s): %s\n",
                             EditErrorName(error), message.c_str());
            return;
        }
        if (![self commitProjectCandidate:candidate
                                  editLog:self.state->editLog
                               projectLog:projectLog
                                  message:message]) {
            std::fprintf(stderr, "Unable to persist project redo: %s\n",
                         message.c_str());
            return;
        }
        self.state->project = std::move(candidate);
        self.state->projectEditLog = std::move(projectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        if (renameOnly)
            [self refreshAfterProjectRename];
        else
            [self refreshAfterProjectMutation];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (stagedLog.Redo(stagedDocument, error, message)) {
        if (![self persistStagedDocument:stagedDocument
                                 editLog:stagedLog
                                 message:message]) {
            std::fprintf(stderr, "Unable to persist redo: %s\n",
                         message.c_str());
            return;
        }
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self refreshBinControlsSelecting:nil];
        [self updateSelectionInfo];
        self.state->overlayDirty = true;
    } else if (error != EditError::EmptyRedo) {
        std::fprintf(stderr, "Redo failed (%s): %s\n", EditErrorName(error),
                     message.c_str());
    }
}

- (BOOL)hasValidTimelineRange {
    return self.state->timelineIn && self.state->timelineOut &&
           *self.state->timelineOut > *self.state->timelineIn;
}

- (BOOL)deleteTimelineRangeRipple:(BOOL)ripple {
    if (![self hasValidTimelineRange]) return NO;
    std::optional<DeleteGapOperation> rangeOperation =
        TimelineRangeDeleteOperation(
            self.state->document, *self.state->timelineIn,
            *self.state->timelineOut, ripple,
            std::vector<Ulid>(self.state->targetedTrackIds.begin(),
                              self.state->targetedTrackIds.end()));
    if (!rangeOperation) return NO;
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = *self.state->timelineIn;
    if (![self applyAndPersistTimelineOperation:Operation{
                                                    std::move(*rangeOperation)}
                                          error:error
                                        message:message]) {
        std::fprintf(stderr, "Range delete rejected (%s): %s\n",
                     EditErrorName(error), message.c_str());
        return NO;
    }
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.infoLabel.stringValue = ripple ? @"Zone In/Out extraite avec ripple"
                                        : @"Zone In/Out effacée (gap conservé)";
    self.state->overlayDirty = true;
    return YES;
}

- (BOOL)deleteCurrentTimelineSelectionRipple:(BOOL)ripple {
    if ([self hasValidTimelineRange])
        return [self deleteTimelineRangeRipple:ripple];
    const auto gap = self.state->interaction->SelectedGap();
    const std::vector<Ulid> ids = self.state->interaction->SelectedClipIds();
    if (!gap && ids.empty()) return NO;
    Operation operation =
        gap               ? Operation{GapDeleteOperationForSelection(
                  self.state->document, *gap, self.state->linkedSelection)}
        : ids.size() == 1 ? Operation{ClearClipOperation{ids.front(), {}}}
                          : Operation{ClearClipsOperation{ids, {}}};
    if (!gap && ripple) {
        const auto rippleOperation =
            BuildTimelineRippleDeleteSelection(self.state->document, ids);
        if (!rippleOperation) return NO;
        operation = *rippleOperation;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message])
        return NO;
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    return YES;
}

- (BOOL)deleteCurrentTimelineSelection {
    return [self deleteCurrentTimelineSelectionRipple:NO];
}

- (void)menuDeleteSelection:(id)sender {
    (void)sender;
    if (self.window.firstResponder == self.binOutline)
        [self deleteBinPressed:nil];
    else
        [self deleteCurrentTimelineSelection];
}

- (void)menuRippleDeleteSelection:(id)sender {
    (void)sender;
    [self deleteCurrentTimelineSelectionRipple:YES];
}

- (void)menuMarkTimelineIn:(id)sender {
    (void)sender;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        self.state->sourceIn = self.state->sourceMonitorPosition;
        [self updateSourceZoneLabel];
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"SOURCE IN  %@",
                                       TimelineTimecode(
                                           *self.state->sourceIn,
                                           source ? source->rate
                                                  : [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineIn = self.state->requestedPosition;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"IN  %@", TimelineTimecode(*self.state->timelineIn,
                                                     [self playheadFrameRate])];
    self.state->overlayDirty = true;
}

- (void)menuMarkTimelineOut:(id)sender {
    (void)sender;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        const DocumentSource* source =
            self.state->document.FindSource(self.state->sourceMonitorId);
        self.state->sourceOut = self.state->sourceMonitorPosition;
        if (source) {
            self.state->sourceOut = self.state->sourceOut->add(
                {source->rate.den, source->rate.num});
            if (*self.state->sourceOut > source->duration)
                self.state->sourceOut = source->duration;
        }
        [self updateSourceZoneLabel];
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"SOURCE OUT %@",
                                       TimelineTimecode(
                                           *self.state->sourceOut,
                                           source ? source->rate
                                                  : [self playheadFrameRate])];
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineOut = self.state->requestedPosition;
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"OUT %@", TimelineTimecode(*self.state->timelineOut,
                                                     [self playheadFrameRate])];
    self.state->overlayDirty = true;
}

- (void)menuClearTimelineRange:(id)sender {
    (void)sender;
    if (self.state->sourceMonitorActive && self.state->sourceMonitor) {
        self.state->sourceIn.reset();
        self.state->sourceOut.reset();
        [self updateSourceZoneLabel];
        self.infoLabel.stringValue = @"Points Source In/Out effacés";
        self.state->overlayDirty = true;
        return;
    }
    self.state->timelineIn.reset();
    self.state->timelineOut.reset();
    self.infoLabel.stringValue = @"Points In/Out effacés";
    self.state->overlayDirty = true;
}

// D2 -- docs/roadmap/DIALOGUE-2026-10.md. Markers in the window: the editor
// leaves a note where the playhead is, an agent reads it (`describe`,
// `only: ["markers"]`), and what an agent marks shows up here. Every gesture
// below is one of the three marker operations the engine already had.
//
// A marker changes no clip, so applying one redraws the overlay and nothing
// else: -refreshTimelineAfterEditFromPosition: rebuilds the audio timeline,
// and a marker dropped during playback would cut the sound it annotates.
- (BOOL)applyMarkerOperation:(Operation)operation {
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:std::move(operation)
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Marqueur refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->overlayDirty = true;
    [self refreshWorkspaceActivity];
    return YES;
}

- (BOOL)markersBelongToTimeline {
    if (!self.state->sourceMonitorActive || !self.state->sourceMonitor)
        return YES;
    self.infoLabel.stringValue =
        @"Les marqueurs se posent sur la timeline, pas dans SOURCE";
    return NO;
}

- (void)showMarkerInfo:(const DocumentMarker&)marker {
    // D6 -- a span says where it ends.
    NSString* when = TimelineTimecode(marker.time, [self playheadFrameRate]);
    if (marker.duration.value > 0)
        when = [NSString
            stringWithFormat:@"%@ → %@", when,
                             TimelineTimecode(marker.time.add(marker.duration),
                                              [self playheadFrameRate])];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"MARQUEUR  %@    %@", when,
                                   CMStringFromUtf8(marker.name)];
}

// The note of one marker, typed. `note` is the text to start from;
// `answer` gets the button -- first to keep it, second to give up, third
// (only when `removable`) to delete the marker -- and the text typed.
// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel, a
// sheet: the answer comes when it closes, no longer before this returns.
- (void)askMarkerNote:(NSString*)note
                 time:(RationalTime)time
             duration:(RationalTime)duration
            removable:(BOOL)removable
               answer:
                   (void (^)(NSModalResponse response, NSString* typed))answer {
    [self setPlaybackDirection:0];
    // D6 -- a span is named by both its ends.
    NSString* title =
        duration.value > 0
            ? [NSString
                  stringWithFormat:@"Marqueur de %@ à %@",
                                   TimelineTimecode(time,
                                                    [self playheadFrameRate]),
                                   TimelineTimecode(time.add(duration),
                                                    [self playheadFrameRate])]
            : [NSString stringWithFormat:@"Marqueur à %@",
                                         TimelineTimecode(
                                             time, [self playheadFrameRate])];
    NSTextField* field =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 24)];
    field.identifier = @"marker.note";
    field.stringValue = note ?: @"";
    field.placeholderString = @"Ce qu’il y a à voir ou à reprendre ici";
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:title
              message:@"La note est lue par l’agent qui monte ce projet."
                 form:field];
    [panel addButtonWithTitle:@"Enregistrer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    if (removable)
        [panel addButtonWithTitle:@"Supprimer le marqueur"
                             role:CMFormButtonRoleAlternate];
    panel.initialFirstResponder = field;
    void (^answered)(NSModalResponse, NSString*) = [answer copy];
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  answered(response, field.stringValue);
                }];
}

- (void)editMarkerNote:(const Ulid&)markerId {
    const DocumentMarker* found = self.state->document.FindMarker(markerId);
    if (!found) return;
    // Copied: applying an operation replaces the document `found` points in.
    const DocumentMarker marker = *found;
    const bool unnamed = marker.name == kTimelineMarkerDefaultName;
    NSString* note = unnamed ? @"" : CMStringFromUtf8(marker.name);
    [self askMarkerNote:note
                   time:marker.time
               duration:marker.duration
              removable:YES
                 answer:^(NSModalResponse response, NSString* typed) {
                   if (response == NSAlertThirdButtonReturn) {
                       if ([self applyMarkerOperation:RemoveMarkerOperation{
                                                          marker.id}])
                           self.infoLabel.stringValue = @"Marqueur supprimé";
                       return;
                   }
                   if (response != NSAlertFirstButtonReturn) return;
                   const std::string name =
                       TimelineMarkerNameFromNote(typed.UTF8String ?: "");
                   if (name == marker.name) return;
                   if ([self applyMarkerOperation:UpdateMarkerOperation{
                                                      marker.id, name,
                                                      marker.time, marker.color,
                                                      marker.category,
                                                      marker.duration}]) {
                       DocumentMarker renamed = marker;
                       renamed.name = name;
                       [self showMarkerInfo:renamed];
                   }
                 }];
}

// M: a marker where the playhead is, at once and without a question, so
// that it can be dropped while watching. On a marker, M opens its note.
- (void)menuAddMarker:(id)sender {
    (void)sender;
    if (![self markersBelongToTimeline]) return;
    const DocumentMarker* existing = TimelineMarkerAtPlayhead(
        self.state->document, self.state->requestedPosition,
        [self playheadFrameRate]);
    if (existing) {
        const Ulid existingId = existing->id;
        [self editMarkerNote:existingId];
        return;
    }
    DocumentMarker marker;
    marker.name = kTimelineMarkerDefaultName;
    marker.color = kTimelineMarkerEditorColor;
    marker.time = self.state->requestedPosition;
    if ([self applyMarkerOperation:AddMarkerOperation{marker, -1}])
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"MARQUEUR  %@    M à nouveau pour l’annoter",
                             TimelineTimecode(marker.time,
                                              [self playheadFrameRate])];
}

// Shift+M: the note first. Given up, nothing was added -- one marker and
// its note are one undo step, not two.
- (void)menuAnnotateMarker:(id)sender {
    (void)sender;
    if (![self markersBelongToTimeline]) return;
    const DocumentMarker* existing = TimelineMarkerAtPlayhead(
        self.state->document, self.state->requestedPosition,
        [self playheadFrameRate]);
    if (existing) {
        const Ulid existingId = existing->id;
        [self editMarkerNote:existingId];
        return;
    }
    DocumentMarker marker;
    marker.color = kTimelineMarkerEditorColor;
    marker.time = self.state->requestedPosition;
    // D6 -- with an In/Out range set on the timeline, the note is about
    // that passage: the marker spans it. The keyboard's way to what
    // Option-drag does with the mouse.
    if ([self hasValidTimelineRange]) {
        marker.time = *self.state->timelineIn;
        marker.duration = self.state->timelineOut->sub(*self.state->timelineIn);
    }
    [self
        askMarkerNote:@""
                 time:marker.time
             duration:marker.duration
            removable:NO
               answer:^(NSModalResponse response, NSString* typed) {
                 if (response != NSAlertFirstButtonReturn) return;
                 DocumentMarker added = marker;
                 added.name =
                     TimelineMarkerNameFromNote(typed.UTF8String ?: "");
                 if ([self applyMarkerOperation:AddMarkerOperation{added, -1}])
                     [self showMarkerInfo:added];
               }];
}

- (void)menuRemoveMarker:(id)sender {
    (void)sender;
    if (![self markersBelongToTimeline]) return;
    const DocumentMarker* existing = TimelineMarkerAtPlayhead(
        self.state->document, self.state->requestedPosition,
        [self playheadFrameRate]);
    if (!existing) {
        self.infoLabel.stringValue = @"Aucun marqueur sous la tête de lecture";
        return;
    }
    if ([self applyMarkerOperation:RemoveMarkerOperation{existing->id}])
        self.infoLabel.stringValue = @"Marqueur supprimé";
}

- (void)goToAdjacentMarkerForward:(BOOL)forward {
    if (![self markersBelongToTimeline]) return;
    const auto time = AdjacentTimelineMarker(self.state->document,
                                             self.state->requestedPosition,
                                             [self playheadFrameRate], forward);
    if (!time) {
        self.infoLabel.stringValue =
            forward ? @"Pas de marqueur après" : @"Pas de marqueur avant";
        return;
    }
    [self setPlaybackDirection:0];
    [self requestTimelinePosition:*time];
    if (const DocumentMarker* marker = TimelineMarkerAtPlayhead(
            self.state->document, self.state->requestedPosition,
            [self playheadFrameRate]))
        [self showMarkerInfo:*marker];
    self.state->overlayDirty = true;
}

- (void)menuNextMarker:(id)sender {
    (void)sender;
    [self goToAdjacentMarkerForward:YES];
}

- (void)menuPreviousMarker:(id)sender {
    (void)sender;
    [self goToAdjacentMarkerForward:NO];
}

// D7 -- docs/roadmap/DIALOGUE-2026-10.md. The words of a carton, corrected
// in the window: generated by an agent or read from an SRT, they could only
// be changed through set_clip_caption. They are typed in the Inspector, as
// in the editor the timelines go to (InspectorView.mm). What may be sent,
// and the operation that keeps the carton's style, are decided in
// MediaPanelModel.h (EditCartonText); this only applies and reports.
- (void)showCartonInfo:(const Ulid&)clipId {
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    if (!clip) return;
    // One line for a label that has one.
    NSString* text = [CMStringFromUtf8(clip->caption_text)
        stringByReplacingOccurrencesOfString:@"\n"
                                  withString:@" / "];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"CARTON  %@    %@",
                                   TimelineTimecode(clip->timeline_in,
                                                    [self playheadFrameRate]),
                                   text];
}

// One edit, or none and the reason in the status line. YES when the carton
// now holds the text typed.
- (BOOL)setCartonText:(const std::string&)typed forClip:(const Ulid&)clipId {
    using ui::media_panel::CartonTextOutcome;
    ui::media_panel::CartonTextEdit edit =
        ui::media_panel::EditCartonText(self.state->document, clipId, typed);
    switch (edit.outcome) {
        case CartonTextOutcome::NotACarton:
            self.infoLabel.stringValue =
                @"Le texte se modifie sur un carton de la piste de légendes";
            return NO;
        case CartonTextOutcome::Locked:
            self.infoLabel.stringValue =
                @"Piste de légendes verrouillée : le texte ne se modifie pas";
            return NO;
        case CartonTextOutcome::Empty:
            self.infoLabel.stringValue =
                @"Un carton ne reste pas vide : le supprimer pour le retirer";
            return NO;
        case CartonTextOutcome::Unchanged:
            return NO;
        case CartonTextOutcome::Edit:
            break;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{
                                                    std::move(edit.operation)}
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Texte refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    // As a restyle does: the monitor shows the carton under the playhead
    // again, with its new words.
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
    [self showCartonInfo:clipId];
    return YES;
}

// D7 -- the keys of a text being typed. The Edit menu gives Cmd+Z, Cmd+C
// and Cmd+V to the montage and has neither Cut nor Select All, so in a
// field of this window copying a word copied the selected clip, pasting it
// pasted a clip, and undoing took back the last edit of the film. While a
// text view holds the keyboard these five are its own. Cmd+Z with nothing
// typed to undo does nothing rather than reach the montage: nobody means
// the film by it from inside a field.
// ON3 -- a key typed in the Agent's window: its field keeps the same five
// keys, which the menus would otherwise take for the montage.
- (BOOL)agentWindowTakesKey:(NSEvent*)event {
    NSWindow* keyed = event.window ?: NSApp.keyWindow;
    if (keyed == nil || keyed != self.agentWindow) return NO;
    NSResponder* typing = keyed.firstResponder;
    return [typing isKindOfClass:NSTextView.class] &&
           [self textEditKeyDown:event inTextView:(NSTextView*)typing];
}

- (BOOL)textEditKeyDown:(NSEvent*)event inTextView:(NSTextView*)text {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & kShortcutModifierMask;
    const BOOL shift = (modifiers & NSEventModifierFlagShift) != 0;
    if ((modifiers & ~NSEventModifierFlagShift) != NSEventModifierFlagCommand)
        return NO;
    // Cmd+Return in the carton's field: Return breaks the line there, so
    // this is the key that says "done". Leaving the field commits it.
    if (!shift && (event.keyCode == 36 || event.keyCode == 76) &&
        text.delegate == (id)self.inspectorView.captionTextField) {
        [self.window makeFirstResponder:self.metalView];
        return YES;
    }
    NSString* key = event.charactersIgnoringModifiers.lowercaseString;
    if ([key isEqualToString:@"z"]) {
        NSUndoManager* typed = text.undoManager;
        if (shift && typed.canRedo)
            [typed redo];
        else if (!shift && typed.canUndo)
            [typed undo];
        return YES;
    }
    if (shift) return NO;
    if ([key isEqualToString:@"c"])
        [text copy:nil];
    else if ([key isEqualToString:@"v"])
        [text paste:nil];
    else if ([key isEqualToString:@"x"])
        [text cut:nil];
    else if ([key isEqualToString:@"a"])
        [text selectAll:nil];
    else
        return NO;
    return YES;
}

// The keyboard goes to the carton's text in the Inspector, its dock turned
// to that panel. The carton is the selection by now: the Inspector shows
// the selected clip, and nothing else.
- (void)editCartonText:(const Ulid&)clipId {
    using ui::media_panel::CartonTextOutcome;
    // Refused here rather than by a field that will not take the keys:
    // the status line says why.
    if (ui::media_panel::CartonTextAccess(self.state->document, clipId) !=
        CartonTextOutcome::Edit) {
        [self setCartonText:"" forClip:clipId];
        return;
    }
    [self setPlaybackDirection:0];
    [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    if ([self.inspectorView focusCaptionTextForClip:clipId]) {
        self.infoLabel.stringValue =
            @"CARTON · Entrée passe à la ligne · ⌘ Entrée enregistre · Tab "
            @"carton suivant · Échap annule";
        return;
    }
    // The dock shows a motion layer, or the selection is not this carton.
    self.infoLabel.stringValue =
        @"Le texte d’un carton se modifie dans l’Inspector, carton sélectionné";
}

// The Inspector's three calls. A text typed there is one edit; the walk
// from carton to carton selects and sends the playhead, so that Record
// shows the carton being read; Escape hands the keys back to the timeline.
- (BOOL)inspectorView:(CMInspectorView*)inspectorView
    didCommitCartonText:(const std::string&)typed
                forClip:(const Ulid&)clipId {
    (void)inspectorView;
    return [self setCartonText:typed forClip:clipId];
}

- (BOOL)inspectorView:(CMInspectorView*)inspectorView
    selectCartonAdjacentTo:(const Ulid&)clipId
                   forward:(BOOL)forward {
    (void)inspectorView;
    const auto neighbour =
        ui::media_panel::AdjacentCarton(self.state->document, clipId, forward);
    if (!neighbour) {
        self.infoLabel.stringValue = forward ? @"Dernier carton de la piste"
                                             : @"Premier carton de la piste";
        return NO;
    }
    const Ulid carton = *neighbour;
    const DocumentClip* clip = self.state->document.FindClip(carton);
    if (!clip) return NO;
    [self setPlaybackDirection:0];
    self.state->interaction->SelectClip(carton);
    [self requestTimelinePosition:clip->timeline_in];
    [self updateSelectionInfo];
    [self showCartonInfo:carton];
    self.state->overlayDirty = true;
    return YES;
}

- (void)inspectorView:(CMInspectorView*)inspectorView
    previewCartonText:(const std::string&)typed
              forClip:(const Ulid&)clipId {
    (void)inspectorView;
    if (clipId.empty())
        self.state->cartonPreview.reset();
    else
        self.state->cartonPreview =
            ui::media_panel::CartonTextPreview{clipId, typed};
    self.state->overlayDirty = true;
}

- (void)inspectorViewDidLeaveCartonText:(CMInspectorView*)inspectorView {
    (void)inspectorView;
    [self.window makeFirstResponder:self.metalView];
}

- (void)menuEditCartonText:(id)sender {
    (void)sender;
    if (!self.state || !self.state->interaction) return;
    const auto carton = ui::media_panel::SelectedCartonForText(
        self.state->document, self.state->interaction->SelectedClipIds());
    if (!carton) {
        self.infoLabel.stringValue =
            @"Sélectionner un carton pour modifier son texte";
        return;
    }
    const Ulid edited = *carton;
    [self editCartonText:edited];
}

- (BOOL)performSourceEditInsert:(BOOL)insert {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!self.state->sourceMonitor || !source) {
        self.infoLabel.stringValue = @"Chargez d’abord un rush dans SOURCE";
        return NO;
    }
    const DocumentTrack* target = nullptr;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind != "video" || track.locked ||
            !self.state->targetedTrackIds.count(track.id))
            continue;
        if (!target || track.index < target->index) target = &track;
    }
    if (!target) {
        self.infoLabel.stringValue =
            @"Ciblez au moins une piste vidéo déverrouillée";
        return NO;
    }
    const LibraryMedia* media = nullptr;
    const auto probed = self.state->mediaMetadata.find(source->id);
    if (probed != self.state->mediaMetadata.end())
        media = &probed->second;
    else
        media = self.state->document.FindLibraryMedia(source->id);
    const bool sourceHasAudio =
        media && media->metadata_complete && media->has_audio;
    const DocumentTrack* audioTarget = nullptr;
    if (sourceHasAudio) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            if (track.kind != "audio" || track.locked ||
                !self.state->targetedTrackIds.count(track.id))
                continue;
            if (!audioTarget || track.index < audioTarget->index)
                audioTarget = &track;
        }
        if (!audioTarget) {
            for (const DocumentTrack& track :
                 self.state->document.sequence.tracks) {
                if (track.kind != "audio" || track.locked) continue;
                if (!audioTarget || track.index < audioTarget->index)
                    audioTarget = &track;
            }
        }
    }

    SourceRecordEditPoints points{self.state->sourceIn, self.state->sourceOut,
                                  self.state->timelineIn,
                                  self.state->timelineOut};
    if (!points.timeline_in && !points.timeline_out) {
        points.timeline_in = self.state->requestedPosition;
    }
    SourceRecordEditRange range;
    std::string rangeError;
    if (!ResolveSourceRecordEditRange(source->duration, points, range,
                                      rangeError)) {
        std::fprintf(stderr, "Source/Record range rejected: %s\n",
                     rangeError.c_str());
        self.infoLabel.stringValue = NSLocalizedString(
            @"Zone Source/Record invalide ou durées incompatibles",
            @"Invalid Source/Record edit points");
        return NO;
    }
    const RationalTime sourceIn = range.source_in;
    const RationalTime sourceOut = range.source_out;
    const RationalTime timelineIn = range.timeline_in;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    Ulid audioTargetId = audioTarget ? audioTarget->id : Ulid{};
    if (sourceHasAudio && audioTargetId.empty()) {
        int32_t index = 0;
        for (const DocumentTrack& track : stagedDocument.sequence.tracks)
            index = std::max(index, track.index + 1);
        audioTargetId = GenerateUlid();
        if (!stagedLog.Apply(
                stagedDocument,
                Operation{AddTrackOperation{audioTargetId, "audio", index}},
                error, message)) {
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Création audio refusée (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(error)),
                                 CMStringFromUtf8(message)];
            return NO;
        }
    }
    std::optional<DeleteGapOperation> operation = TimelineSourceEditOperation(
        stagedDocument, self.state->sourceMonitorId, sourceIn, sourceOut,
        timelineIn, target->id, insert,
        audioTargetId.empty() ? std::vector<Ulid>{}
                              : std::vector<Ulid>{audioTargetId});
    if (!operation) {
        self.infoLabel.stringValue =
            NSLocalizedString(@"Zone Source/Record invalide pour ce montage",
                              @"Invalid Source/Record edit target");
        return NO;
    }
    if (!stagedLog.Apply(stagedDocument, Operation{std::move(*operation)},
                         error, message)) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Montage refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return NO;
    }
    const RationalTime next = timelineIn.add(sourceOut.sub(sourceIn));
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return NO;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    if (!audioTargetId.empty()) {
        self.state->targetedTrackIds.insert(audioTargetId);
        self.state->timelineTargetTracks[self.state->activeTimelineId] =
            self.state->targetedTrackIds;
    }
    [self refreshTimelineAfterEditFromPosition:next];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Rush %@ depuis SOURCE%@",
                         insert ? @"inséré" : @"écrasé",
                         !audioTargetId.empty() ? @" · audio séparé et lié"
                                                : @""];
    self.state->overlayDirty = true;
    return YES;
}

- (void)menuInsertSource:(id)sender {
    (void)sender;
    [self performSourceEditInsert:YES];
}

- (void)menuOverwriteSource:(id)sender {
    (void)sender;
    [self performSourceEditInsert:NO];
}

- (void)menuCutSelectedAtPlayhead:(id)sender {
    (void)sender;
    const Ulid clipId = self.state->interaction->SelectedClipId();
    if (clipId.empty()) return;
    self.state->contextClipId = clipId;
    self.state->contextTime = self.state->requestedPosition;
    [self contextCutClip:nil];
}

- (void)menuAddCrossDissolve:(id)sender {
    (void)sender;
    const DocumentTrack* chosenTrack = nullptr;
    const DocumentClip* left = nullptr;
    const DocumentClip* right = nullptr;
    const Ulid selected = self.state->interaction->SelectedClipId();
    const auto choosePair = [&](const DocumentTrack& track, size_t index) {
        // EF2 -- a sound track takes a cross fade.
        if ((track.kind != "video" && track.kind != "audio") ||
            index + 1 >= track.clips.size())
            return false;
        const DocumentClip& candidateLeft = track.clips[index];
        const DocumentClip& candidateRight = track.clips[index + 1];
        if (candidateLeft.timeline_in.add(candidateLeft.duration) !=
            candidateRight.timeline_in)
            return false;
        chosenTrack = &track;
        left = &candidateLeft;
        right = &candidateRight;
        return true;
    };
    if (!selected.empty()) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            for (size_t index = 0; index < track.clips.size(); ++index) {
                if (track.clips[index].id != selected) continue;
                if (!choosePair(track, index) && index > 0)
                    choosePair(track, index - 1);
                break;
            }
            if (left) break;
        }
    }
    if (!left) {
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            for (size_t index = 0; index + 1 < track.clips.size(); ++index) {
                if (track.clips[index + 1].timeline_in ==
                        self.state->requestedPosition &&
                    choosePair(track, index))
                    break;
            }
            if (left) break;
        }
    }
    if (!left || !right || !chosenTrack) {
        self.infoLabel.stringValue = @"Placez le playhead sur un cut ou "
                                     @"sélectionnez un clip adjacent";
        return;
    }
    for (const DocumentTransition& existing :
         self.state->document.sequence.transitions) {
        if (existing.left_clip_id == left->id &&
            existing.right_clip_id == right->id) {
            self.infoLabel.stringValue = @"Ce cut possède déjà une transition";
            return;
        }
    }
    // REMOTION-PARITY-2026-09 REM5 -- what the left clip reads: its media,
    // or for a motion clip its composition (ClipSource, M4). A motion clip
    // used to have no source here and the menu did nothing (the M4 usage
    // gap of VISUAL_QA_CHECKLIST.md).
    const std::optional<DocumentSource> leftSource =
        ClipSource(self.state->document, *left);
    if (!leftSource) return;
    const MediaRate rate = self.state->document.sequence.frame_rate;
    const RationalTime tail =
        leftSource->duration.sub(left->source_in.add(left->duration));
    const int64_t leftHandle = tail.to_frames(rate.num, rate.den);
    const int64_t rightHandle = right->source_in.to_frames(rate.num, rate.den);
    const int64_t halfFrames = std::min<int64_t>({6, leftHandle, rightHandle});
    if (halfFrames < 1) {
        self.infoLabel.stringValue =
            @"Fondu impossible : il manque des poignées média de chaque côté";
        return;
    }
    DocumentTransition transition;
    transition.track_id = chosenTrack->id;
    transition.left_clip_id = left->id;
    transition.right_clip_id = right->id;
    transition.duration = {halfFrames * 2 * rate.den, rate.num};
    // EF2 -- on a sound track, the sound's transition.
    const bool crossFade = chosenTrack->kind == "audio";
    if (crossFade) transition.type = kTransitionCrossFade;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            AddTransitionOperation { transition }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Transition refusée (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%@ · %lld images",
                         crossFade ? @"Fondu croisé" : @"Fondu enchaîné",
                         static_cast<long long>(halfFrames * 2)];
    self.state->overlayDirty = true;
}

- (void)menuRemoveCrossDissolve:(id)sender {
    (void)sender;
    const Ulid selected = self.state->interaction->SelectedClipId();
    const DocumentTransition* chosen = nullptr;
    for (const DocumentTransition& transition :
         self.state->document.sequence.transitions) {
        const DocumentClip* right =
            self.state->document.FindClip(transition.right_clip_id);
        const bool selectedCut =
            !selected.empty() && (transition.left_clip_id == selected ||
                                  transition.right_clip_id == selected);
        const bool playheadCut =
            right && right->timeline_in == self.state->requestedPosition;
        if (selectedCut || playheadCut) {
            chosen = &transition;
            break;
        }
    }
    if (!chosen) {
        self.infoLabel.stringValue =
            @"Aucun fondu au raccord ou au clip sélectionné";
        return;
    }
    const Ulid transitionId = chosen->id;
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveTransitionOperation { transitionId }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = @"Fondu enchaîné supprimé";
    self.state->overlayDirty = true;
}

// F1.5 -- ROADMAP.md. The switch the selected clip stands for: make its angle
// the active one, or clear its group's choice. Multicam.h decides which group
// and angle a clip belongs to, as it does for the CLI and MCP; this only
// reads the selection and applies the one reversible operation.
- (BOOL)multicamSwitchForSelection:(BOOL)clear
                         operation:(SetMulticamActiveAngleOperation&)operation {
    if (self.state == nullptr || !self.state->interaction) {
        return NO;
    }
    const Ulid selected = self.state->interaction->SelectedClipId();
    EditError error = EditError::None;
    std::string message;
    SetMulticamActiveAngleOperation angle;
    if (selected.empty() ||
        !BuildSetMulticamActiveAngle(self.state->document, {}, selected, angle,
                                     error, message)) {
        return NO;
    }
    const MulticamGroup* group =
        self.state->document.FindMulticamGroup(angle.group_id);
    if (group == nullptr) {
        return NO;
    }
    if (clear) {
        if (group->active_angle_id.empty()) {
            return NO;
        }
        operation = {group->id, {}};
        return YES;
    }
    if (group->active_angle_id == angle.active_angle_id) {
        return NO;
    }
    operation = angle;
    return YES;
}

- (void)applyMulticamSwitch:(BOOL)clear {
    SetMulticamActiveAngleOperation operation;
    if (![self multicamSwitchForSelection:clear operation:operation]) {
        self.infoLabel.stringValue =
            clear ? @"Le clip sélectionné n’est l’angle d’aucun groupe "
                    @"multicam ayant un angle actif"
                  : @"Sélectionnez un angle inactif d’un groupe multicam";
        return;
    }
    EditError error = EditError::None;
    std::string message;
    if (![self applyAndPersistTimelineOperation:Operation{operation}
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Changement d’angle refusé (%@) : %@",
                                       CMStringFromUtf8(EditErrorName(error)),
                                       CMStringFromUtf8(message)];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.infoLabel.stringValue = clear ? @"Multicam : tous les angles visibles"
                                       : @"Multicam : angle du clip activé";
    self.state->overlayDirty = true;
}

- (void)menuActivateMulticamAngle:(id)sender {
    (void)sender;
    [self applyMulticamSwitch:NO];
}

- (void)menuClearMulticamAngle:(id)sender {
    (void)sender;
    [self applyMulticamSwitch:YES];
}

// SY2 -- docs/roadmap/SYNCHRO-2026-10.md. Resolve's Auto Align Clips, Based
// on Waveform (DaVinci Resolve 21.1 manual, chapter 45, pages 955-956): a
// submenu, as Resolve has one, whose single entry is the waveform; the
// timecode is not read (decision of 8 October 2026).
- (NSMenuItem*)autoAlignMenuItem {
    NSMenuItem* root =
        [[NSMenuItem alloc] initWithTitle:@"Aligner automatiquement les plans"
                                   action:nil
                            keyEquivalent:@""];
    NSMenu* submenu =
        [[NSMenu alloc] initWithTitle:@"Aligner automatiquement les plans"];
    NSMenuItem* waveform = [self menuItem:@"Selon la forme d’onde"
                                   action:@selector(menuAutoAlignByWaveform:)
                                      key:@""];
    waveform.toolTip =
        @"Cale le son de l’enregistreur sélectionné sous le plan image "
        @"sélectionné, le lie au plan et coupe le son caméra ; une "
        @"annulation retire tout";
    [submenu addItem:waveform];
    root.submenu = submenu;
    return root;
}

- (void)menuAutoAlignByWaveform:(id)sender {
    (void)sender;
    [self alignSelectedSoundByWaveform];
}

// The two sounds are measured off the main thread, as
// --measure-audio-offset measures them, on the editor's task manager: the
// task bar shows it and cancels it. The edit is built again on the
// document as it is when the measure answers, and applied as one undo.
// YES when the measure was started.
- (BOOL)alignSelectedSoundByWaveform {
    if (!self.state || !self.state->interaction) return NO;
    const auto refuse = [&](const std::string& reason) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Synchro impossible : %@.",
                                       CMStringFromUtf8(reason)];
        return NO;
    };
    if (self.state->soundAlignmentRunning)
        return refuse("une synchro est déjà en cours");
    std::string reason;
    const std::optional<ui::separate_sound::SoundPair> pair =
        ui::separate_sound::PickSoundPair(
            self.state->document, self.state->interaction->SelectedClipIds(),
            reason);
    if (!pair) return refuse(reason);
    const SeparateSoundPairFault fault = CheckSeparateSoundPair(
        self.state->document, pair->picture_clip_id, pair->sound_clip_id);
    if (fault != SeparateSoundPairFault::None)
        return refuse(ui::separate_sound::PairFaultText(fault));
    const std::string projectPath = self.documentPath.UTF8String ?: "";
    if (projectPath.empty())
        return refuse(
            ui::separate_sound::MeasureRefusalText("InvalidDocument", ""));
    Project project = self.state->project;
    std::string message;
    if (!project.CommitDocument(self.state->activeTimelineId,
                                self.state->document, message))
        return refuse("le projet n’a pas pu être lu (" + message + ")");
    const Ulid pictureSource =
        self.state->document.FindClip(pair->picture_clip_id)->source_id;
    const Ulid soundSource =
        self.state->document.FindClip(pair->sound_clip_id)->source_id;
    MediaTaskManager* tasks = self.state->mediaTasks.get();
    const MediaTaskRunner runner = [tasks](MediaTaskKind kind,
                                           std::string label,
                                           MediaTaskManager::Work work) {
        return RunMediaTaskAndWait(*tasks, kind, std::move(label),
                                   std::move(work));
    };
    self.state->soundAlignmentRunning = true;
    self.infoLabel.stringValue = @"Synchro : mesure du décalage entre le son "
                                 @"du plan et celui de l’enregistreur…";
    const ui::separate_sound::SoundPair chosen = *pair;
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::string output;
      MeasureAudioOffsetCommand(projectPath, pictureSource, soundSource, {},
                                output, runner, &project);
      // Back by the run loop, as the setup notice comes back: a run loop
      // nested in a block of the main queue still takes it.
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        [weakSelf finishSoundAlignment:chosen measured:output];
      });
      CFRunLoopWakeUp(mainLoop);
    });
    [self refreshMediaTaskStatus];
    return YES;
}

- (void)finishSoundAlignment:(ui::separate_sound::SoundPair)pair
                    measured:(const std::string&)output {
    if (!self.state) return;
    self.state->soundAlignmentRunning = false;
    const auto refuse = [&](const std::string& reason) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Synchro refusée : %@. Rien n’a été posé.",
                             CMStringFromUtf8(reason)];
    };
    RationalTime offset;
    std::string code;
    std::string detail;
    if (!ui::separate_sound::ReadMeasuredOffset(output, offset, code, detail)) {
        refuse(ui::separate_sound::MeasureRefusalText(code, detail));
        return;
    }
    BatchOperation batch;
    SeparateSoundPlacement placement;
    EditError error = EditError::None;
    std::string message;
    if (!BuildSeparateSoundAlign(
            self.state->document,
            {pair.picture_clip_id, pair.sound_clip_id, offset, true}, batch,
            placement, error, message)) {
        refuse(ui::separate_sound::AlignRefusalText(error, message));
        return;
    }
    const std::string shift = ui::separate_sound::OffsetText(
        offset, self.state->document.sequence.frame_rate);
    if (batch.operations.empty()) {
        self.infoLabel.stringValue = [NSString
            stringWithFormat:@"Le son est déjà calé sur le plan (décalage "
                             @"%@) et lié à lui : rien à faire.",
                             CMStringFromUtf8(shift)];
        return;
    }
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{batch}
                                          error:error
                                        message:message]) {
        refuse(ui::separate_sound::AlignRefusalText(error, message));
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    NSString* muted = @"";
    if (!placement.muted_track_id.empty())
        muted = [NSString stringWithFormat:@", piste %@ du son caméra coupée",
                                           CMStringFromUtf8(TrackDisplayName(
                                               self.state->document,
                                               placement.muted_track_id))];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"Son de l’enregistreur calé sous le plan (décalage "
                         @"mesuré %@), lié%@. ⌘Z annule tout.",
                         CMStringFromUtf8(shift), muted];
    self.state->overlayDirty = true;
}

- (void)menuSelectTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Select];
}
- (void)menuHandTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Hand];
}
- (void)menuZoomTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Zoom];
}
- (void)menuCutTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Cut];
}
- (void)menuTrimTool:(id)sender {
    (void)sender;
    [self setTimelineTool:TimelineTool::Trim];
}
- (void)menuTrimSelectNearest:(id)sender {
    (void)sender;
    [self trimSelectNearest];
}
- (void)menuTrimCycleSide:(id)sender {
    (void)sender;
    [self trimCycleSide];
}
- (void)menuTrimNudgeBack:(id)sender {
    (void)sender;
    [self trimNudge:-1];
}
- (void)menuTrimNudgeForward:(id)sender {
    (void)sender;
    [self trimNudge:1];
}
- (void)menuTrimNudgeBackMany:(id)sender {
    (void)sender;
    [self trimNudge:-trim::kLargeTrimFrames];
}
- (void)menuTrimNudgeForwardMany:(id)sender {
    (void)sender;
    [self trimNudge:trim::kLargeTrimFrames];
}
- (void)menuTrimExtend:(id)sender {
    (void)sender;
    [self trimExtendToPlayhead];
}
- (void)menuTrimDynamic:(id)sender {
    (void)sender;
    [self toggleDynamicTrim];
}
- (void)menuRetimeControls:(id)sender {
    (void)sender;
    [self toggleRetimeControls];
}
// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. Command-R shows the selected
// clips' Retime controls, or hides them when all of them show theirs
// (Resolve 21.1 manual, p. 1218-1222): a band along the top of each clip
// with its speed, whose ends stretch it (RateStretchOperation).
- (void)toggleRetimeControls {
    std::vector<Ulid> selected = self.state->interaction->SelectedClipIds();
    if (selected.empty()) {
        self.infoLabel.stringValue =
            @"Contrôles de vitesse : sélectionnez un plan";
        return;
    }
    std::vector<Ulid>& shown = self.state->retimeClipIds;
    const bool all =
        std::all_of(selected.begin(), selected.end(), [&](const Ulid& id) {
            return std::find(shown.begin(), shown.end(), id) != shown.end();
        });
    if (all) {
        shown.erase(std::remove_if(shown.begin(), shown.end(),
                                   [&](const Ulid& id) {
                                       return std::find(selected.begin(),
                                                        selected.end(),
                                                        id) != selected.end();
                                   }),
                    shown.end());
        self.infoLabel.stringValue = @"Contrôles de vitesse masqués";
    } else {
        for (const Ulid& id : selected)
            if (std::find(shown.begin(), shown.end(), id) == shown.end())
                shown.push_back(id);
        self.infoLabel.stringValue =
            @"VITESSE · glisser un bout de la bande pour étirer le plan";
    }
    self.state->overlayDirty = true;
}
- (void)menuLeaveTrimMode:(id)sender {
    (void)sender;
    [self leaveTrimMode];
    self.infoLabel.stringValue = @"Mode trim quitté";
}
- (void)menuToggleSnapping:(id)sender {
    (void)sender;
    self.state->interaction->SetSnappingEnabled(
        !self.state->interaction->SnappingEnabled());
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}
- (void)menuToggleLinkedSelection:(id)sender {
    (void)sender;
    [self setLinkedSelectionEnabled:!self.state->linkedSelection];
}
// Out of lot -- the author, 29 October 2026 (Alt+Y): Resolve's Select All Clips
// Forward, from the playhead, on every unlocked track.
- (void)menuSelectClipsAfter:(id)sender {
    (void)sender;
    if (!self.state) return;
    self.state->interaction->SelectClips(TimelineClipsAfter(
        self.state->document, self.state->requestedPosition));
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}
// Out of lot -- the author, 29 October 2026: "D on a selected shot disables it:
// greyed, no longer seen or heard". Resolve's toggle on the whole selection
// (ToggleClipsEnabled, ClipEnabled.h): any clip enabled, all are disabled;
// none, all are enabled. One operation, one undo step.
- (void)menuToggleClipsEnabled:(id)sender {
    (void)sender;
    if (!self.state || !self.state->interaction) return;
    const SetClipEnabledOperation operation = ToggleClipsEnabled(
        self.state->document, self.state->interaction->SelectedClipIds());
    if (operation.clips.empty()) {
        NSBeep();
        return;
    }
    const bool enabling = operation.clips.front().enabled;
    const size_t count = operation.clips.size();
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation{operation}
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Plans non modifiés : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
        return;
    }
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.state->overlayDirty = true;
    [self updateSelectionInfo];
    self.infoLabel.stringValue = [NSString
        stringWithFormat:@"%lu plan%@ %@. ⌘Z l’annule.", (unsigned long)count,
                         count == 1 ? @"" : @"s",
                         enabling
                             ? (count == 1 ? @"activé" : @"activés")
                             : (count == 1 ? @"désactivé" : @"désactivés")];
}
- (void)menuFitTimeline:(id)sender {
    (void)sender;
    [self fitTimelineToViewportWidth];
}
- (void)menuAddVideoTrack:(id)sender {
    (void)sender;
    [self addTrack:NO];
}
- (void)menuAddAudioTrack:(id)sender {
    (void)sender;
    [self addTrack:YES];
}

- (void)removeContextTrackPressed:(id)sender {
    (void)sender;
    const DocumentTrack* track =
        self.state->document.FindTrack(self.state->contextTrackId);
    if (!track) return;
    if (!track->clips.empty()) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.alertStyle = NSAlertStyleWarning;
        alert.messageText = @"Supprimer cette piste et tous ses clips ?";
        alert.informativeText = [NSString
            stringWithFormat:@"La piste contient %lu clip%@. Cette action "
                             @"reste annulable avec ⌘Z.",
                             (unsigned long)track->clips.size(),
                             track->clips.size() == 1 ? @"" : @"s"];
        [alert addButtonWithTitle:@"Supprimer la piste"];
        [alert addButtonWithTitle:@"Annuler"];
        if ([alert runModal] != NSAlertFirstButtonReturn) return;
    }
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if (![self applyAndPersistTimelineOperation:Operation {
            RemoveTrackOperation { track->id }
        }
                                          error:error
                                        message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Suppression refusée : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->interaction->SelectClip("");
    [self refreshTimelineAfterEditFromPosition:playhead];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)removeEmptyTracksPressed:(id)sender {
    (void)sender;
    std::vector<Ulid> emptyTrackIds;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        if (track.clips.empty()) emptyTrackIds.push_back(track.id);
    if (emptyTrackIds.empty()) return;

    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    EditError error = EditError::None;
    std::string message;
    for (const Ulid& trackId : emptyTrackIds) {
        if (!stagedLog.Apply(stagedDocument,
                             Operation{RemoveTrackOperation{trackId}}, error,
                             message)) {
            self.infoLabel.stringValue =
                [NSString stringWithFormat:@"Nettoyage refusé : %@",
                                           CMStringFromUtf8(message)];
            return;
        }
    }
    const RationalTime playhead = self.state->requestedPosition;
    if (![self persistStagedDocument:stagedDocument
                             editLog:stagedLog
                             message:message]) {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Enregistrement impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    self.state->document = std::move(stagedDocument);
    self.state->editLog = std::move(stagedLog);
    self.state->lastHistoryDomain = HistoryDomain::Timeline;
    [self refreshTimelineAfterEditFromPosition:playhead];
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%lu piste%@ vide%@ supprimée%@",
                                   (unsigned long)emptyTrackIds.size(),
                                   emptyTrackIds.size() == 1 ? @"" : @"s",
                                   emptyTrackIds.size() == 1 ? @"" : @"s",
                                   emptyTrackIds.size() == 1 ? @"" : @"s"];
    self.state->overlayDirty = true;
}
- (void)menuPlayPause:(id)sender {
    (void)sender;
    [self setPlaybackDirection:self.state->playbackDirection == 0 ? 1 : 0];
}
- (void)menuPlayReverse:(id)sender {
    (void)sender;
    if ([self dynamicTrimTransport:-1]) return;
    [self setPlaybackDirection:-1];
}
- (void)menuStop:(id)sender {
    (void)sender;
    // FC4 -- K commits a dynamic trim, as one edit.
    if ([self dynamicTrimTransport:0]) return;
    [self setPlaybackDirection:0];
}
- (void)menuPlayForward:(id)sender {
    (void)sender;
    if ([self dynamicTrimTransport:1]) return;
    [self setPlaybackDirection:1];
}

- (NSMenu*)timelineMenuForEvent:(NSEvent*)event {
    if (self.videoFullscreenActive) return nil;
    // REM5 -- Record's menu builds the motion clip it shows.
    if ([self motionCanvasContainsEvent:event]) {
        return [self motionCanvasMenuForEvent:event];
    }
    const NSPoint viewPoint =
        [self.metalView convertPoint:event.locationInWindow fromView:nil];
    const double timelineY =
        self.metalView.bounds.size.height - viewPoint.y - [self videoHeight];
    self.state->contextClipId.clear();
    self.state->contextTrackId.clear();
    self.state->contextGap.reset();
    if (timelineY < 0.0 || timelineY > [self timelineHeight]) return nil;
    const RationalTime time =
        self.state->viewport.XToTime(viewPoint.x, self.state->duration.rate);
    self.state->contextTime = time;
    const auto hit =
        HitTestTimeline(self.state->document, self.state->viewport, viewPoint.x,
                        timelineY, self.metalView.bounds.size.width);
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Timeline"];
    if (hit) {
        self.state->contextClipId = hit->clip_id;
        self.state->contextTrackId = hit->track_id;
        // SY2 -- as in Resolve and in every list of macOS, a right click on
        // a clip already selected keeps the selection: Auto Align takes the
        // picture and the sound selected together.
        const std::vector<Ulid>& selected =
            self.state->interaction->SelectedClipIds();
        if (std::find(selected.begin(), selected.end(), hit->clip_id) ==
            selected.end()) {
            std::vector<Ulid> selection{hit->clip_id};
            if (self.state->linkedSelection)
                selection =
                    ExpandLinkedClipSelection(self.state->document, selection);
            self.state->interaction->SelectClips(selection);
        }
        [self updateSelectionInfo];
        // D7 -- docs/roadmap/DIALOGUE-2026-10.md. On a carton, its text
        // first: it has no source to open.
        if (ui::media_panel::CartonTextAccess(self.state->document,
                                              hit->clip_id) !=
            ui::media_panel::CartonTextOutcome::NotACarton) {
            [menu addItem:[self menuItem:@"Modifier le texte"
                                  action:@selector(menuEditCartonText:)
                                     key:@""]];
            [menu addItem:NSMenuItem.separatorItem];
        }
        [menu addItem:[self menuItem:@"Ouvrir dans le moniteur source"
                              action:@selector(contextOpenClipSource:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Retrouver dans la médiathèque"
                              action:@selector(contextFindClipInMediaPool:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Couper ici"
                              action:@selector(contextCutClip:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer"
                              action:@selector(contextDeleteSelection:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Activer / désactiver le plan"
                              action:@selector(menuToggleClipsEnabled:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self autoAlignMenuItem]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        return menu;
    }
    const auto gap = HitTestTimelineGap(
        self.state->document, self.state->viewport, viewPoint.x, timelineY,
        self.metalView.bounds.size.width, self.state->duration.rate);
    if (gap) {
        self.state->contextGap = gap;
        self.state->contextTrackId = gap->track_id;
        [menu addItem:[self menuItem:@"Fermer le gap"
                              action:@selector(contextCloseGap:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        return menu;
    }
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    const NSInteger index = static_cast<NSInteger>(
        (timelineY - kTimelineRulerHeight) / self.state->viewport.track_height);
    if (index >= 0 && index < (NSInteger)tracks.size())
        self.state->contextTrackId = tracks[index]->id;
    if (!self.state->contextTrackId.empty()) {
        [menu addItem:[self menuItem:@"Supprimer la piste"
                              action:@selector(removeContextTrackPressed:)
                                 key:@""]];
        [menu addItem:[self menuItem:@"Supprimer les pistes vides"
                              action:@selector(removeEmptyTracksPressed:)
                                 key:@""]];
        [menu addItem:NSMenuItem.separatorItem];
    }
    [menu addItem:[self menuItem:@"Ajouter une piste vidéo"
                          action:@selector(menuAddVideoTrack:)
                             key:@""]];
    [menu addItem:[self menuItem:@"Ajouter une piste audio"
                          action:@selector(menuAddAudioTrack:)
                             key:@""]];
    [menu addItem:NSMenuItem.separatorItem];
    [menu addItem:[self menuItem:@"Cadrer toute la timeline"
                          action:@selector(menuFitTimeline:)
                             key:@""]];
    return menu;
}

- (void)contextOpenClipSource:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (clip)
        [self openMediaIdentifierInSourceMonitor:
                  [NSString stringWithUTF8String:clip->source_id.c_str()]];
}

- (void)contextFindClipInMediaPool:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip) return;
    self.mediaSearchField.stringValue = @"";
    [self refreshBinControlsSelecting:@"__all__"];
    NSString* source = [NSString stringWithUTF8String:clip->source_id.c_str()];
    const NSUInteger row = [self.visibleMediaIds indexOfObject:source];
    if (row != NSNotFound) {
        self.mediaViewToggle.selectedSegment = 0;
        [self mediaViewChanged:nil];
        [self.mediaTable selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                     byExtendingSelection:NO];
        [self.mediaTable scrollRowToVisible:row];
        [self.window makeFirstResponder:self.mediaTable];
    }
}

- (void)contextCutClip:(id)sender {
    (void)sender;
    const DocumentClip* clip =
        self.state->document.FindClip(self.state->contextClipId);
    if (!clip || self.state->contextTime <= clip->timeline_in ||
        self.state->contextTime >= clip->timeline_in.add(clip->duration))
        return;
    Operation operation = TimelineCutOperationForClip(
        self.state->document, *clip, self.state->contextTime,
        self.state->linkedSelection);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self updateSelectionInfo];
    }
}

- (void)contextDeleteSelection:(id)sender {
    (void)sender;
    [self deleteCurrentTimelineSelection];
}

- (void)contextCloseGap:(id)sender {
    (void)sender;
    if (!self.state->contextGap) return;
    const TimelineGapSelection gap = *self.state->contextGap;
    Operation operation = GapDeleteOperationForSelection(
        self.state->document, gap, self.state->linkedSelection);
    EditError error = EditError::None;
    std::string message;
    const RationalTime playhead = self.state->requestedPosition;
    if ([self applyAndPersistTimelineOperation:std::move(operation)
                                         error:error
                                       message:message]) {
        self.state->interaction->SelectClip("");
        [self refreshTimelineAfterEditFromPosition:playhead];
        [self updateSelectionInfo];
    }
}

- (void)advancePlayback {
    if (self.state->playbackDirection == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const int64_t elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            now - self.state->playbackStarted)
            .count();
    if (elapsedUs <= 0) return;
    const RationalTime delta{
        elapsedUs * static_cast<int64_t>(self.state->playbackDirection),
        1000000};
    const RationalTime next = self.state->playbackAnchor.add(delta);
    if (next <= RationalTime{0, 1}) {
        [self requestTimelinePosition:{0, self.state->duration.rate}];
        [self setPlaybackDirection:0];
    } else if (next >= self.state->duration) {
        [self requestTimelinePosition:self.state->duration];
        [self setPlaybackDirection:0];
    } else {
        [self requestTimelinePosition:next];
    }
}

// Out of lot -- the author, 29 October 2026: "I need to play in the source
// monitor, picture and sound, with an indicator of the player". The shown
// rush alone, on an audio track of its own, for the source monitor's player:
// the timeline's mix is never rebuilt for it.
- (void)prepareSourceAudioForSource:(const DocumentSource&)source {
    if (self.state->sourceAudioPlayback &&
        self.state->sourceAudioMediaId == source.id)
        return;
    Document rush;
    rush.sources = {source};
    rush.sequence.frame_rate = source.rate;
    DocumentClip clip;
    clip.id = GenerateUlid();
    clip.source_id = source.id;
    clip.source_in = {0, source.duration.rate};
    clip.duration = source.duration;
    clip.timeline_in = {0, source.duration.rate};
    DocumentTrack track;
    track.id = GenerateUlid();
    track.kind = "audio";
    track.clips = {clip};
    rush.sequence.tracks = {track};
    if (!self.state->sourceAudioPlayback) {
        auto player = std::make_unique<AudioPlayback>();
        const std::filesystem::path base =
            std::filesystem::absolute(
                std::filesystem::path(self.documentPath.UTF8String ?: ""))
                .parent_path();
        std::string error;
        if (!player->Open(rush, base.string(), error)) {
            std::fprintf(stderr, "Source audio unavailable: %s\n",
                         error.c_str());
            return;
        }
        self.state->sourceAudioPlayback = std::move(player);
    } else {
        self.state->sourceAudioPlayback->RebuildTimeline(rush);
    }
    self.state->sourceAudioMediaId = source.id;
}

// The source monitor's J/K/L: one direction at a time, the timeline's
// playback stopped while it plays (and the other way round,
// -setPlaybackDirection:).
- (void)setSourcePlaybackDirection:(int)direction {
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source || !self.state->sourceMonitor) direction = 0;
    direction = std::clamp(direction, -4, 4);
    if (direction != 0 && self.state->playbackDirection != 0)
        [self setPlaybackDirection:0];
    const bool wasPlaying = self.state->sourcePlaybackDirection != 0;
    self.state->sourcePlaybackDirection = direction;
    if (direction == 0) {
        if (self.state->sourceAudioPlayback)
            self.state->sourceAudioPlayback->Stop();
        if (wasPlaying) self.state->overlayDirty = true;
        return;
    }
    RationalTime anchor = self.state->sourceMonitorPosition;
    // From the end, a play starts the rush again; from the start, J plays
    // back from the end.
    const RationalTime lastFrame = source->duration.sub(
        {static_cast<int64_t>(source->rate.den), source->rate.num});
    if (direction > 0 && anchor >= lastFrame)
        anchor = {0, source->duration.rate};
    if (direction < 0 && anchor <= RationalTime{0, 1})
        anchor = source->duration;
    self.state->sourcePlaybackAnchor = anchor;
    [self prepareSourceAudioForSource:*source];
    if (self.state->sourceAudioPlayback) {
        std::string error;
        if (!self.state->sourceAudioPlayback->PlayFrom(anchor, direction,
                                                       error))
            std::fprintf(stderr, "Source audio playback failed: %s\n",
                         error.c_str());
    }
    // As for the timeline: the clock starts once the sound has.
    self.state->sourcePlaybackStarted = std::chrono::steady_clock::now();
    self.state->overlayDirty = true;
}

- (void)advanceSourcePlayback {
    if (self.state->sourcePlaybackDirection == 0) return;
    const DocumentSource* source =
        self.state->document.FindSource(self.state->sourceMonitorId);
    if (!source || !self.state->sourceMonitorVisible) {
        [self setSourcePlaybackDirection:0];
        return;
    }
    const int64_t elapsedUs =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() -
            self.state->sourcePlaybackStarted)
            .count();
    const SourcePlaybackStep step = AdvanceSourcePlayback(
        self.state->sourcePlaybackAnchor, self.state->sourcePlaybackDirection,
        elapsedUs, source->duration);
    [self requestSourcePosition:step.position];
    if (step.ended) [self setSourcePlaybackDirection:0];
}

- (void)displayTick:(NSTimer*)timer {
    (void)timer;
    [self advancePlayback];
    [self checkPlayAround];
    [self advanceSourcePlayback];
    [self processCompletedMediaImports];
    [self processCompletedMediaProxies];
    [self processCompletedMediaWaveforms];
    [self processCompletedMediaThumbnails];
    [self processCompletedMediaTranscriptions];
    [self processCompletedMediaRelinks];
    [self processCompletedBatchRelinks];
    [self refreshMediaTaskStatus];
    // PRD37 -- a stop leaves the sound output open for the next play; it
    // closes here once silent for long enough (PlaybackPresentation.h).
    if (self.state && self.state->audioPlayback)
        self.state->audioPlayback->ReleaseIdleOutput(
            std::chrono::steady_clock::now());
    if (self.state && self.state->sourceAudioPlayback)
        self.state->sourceAudioPlayback->ReleaseIdleOutput(
            std::chrono::steady_clock::now());
    // FC4 -- J or L running in trim mode: the offset follows the clock.
    if (self.state) [self refreshDynamicTrim];
    [self presentNearestFrameAtDeadline:YES];
}

- (void)refreshMediaTaskStatus {
    if (!self.state || !self.state->mediaTasks || !self.mediaTaskLabel) return;
    const std::vector<MediaTaskSnapshot> tasks =
        self.state->mediaTasks->Snapshot();
    // PRD37 -- ROADMAP.md. Which task, and what is said of it, in French,
    // by MediaPanelModel.h: the engine's English word for the state never
    // shows, and a task over leaves no bar behind.
    const MediaTaskSnapshot* shown = ui::media_panel::ShownMediaTask(tasks);
    const ui::media_panel::MediaTaskLine line =
        ui::media_panel::DescribeMediaTask(shown);
    self.displayedMediaTaskId =
        shown != nullptr ? [NSString stringWithUTF8String:shown->id.c_str()]
                         : nil;
    self.mediaTaskLabel.stringValue = CMStringFromUtf8(line.text);
    self.mediaTaskProgress.hidden = !line.shows_progress;
    self.mediaTaskProgress.doubleValue = line.progress * 100.0;
    self.mediaTaskCancelButton.enabled = line.cancellable;
}

- (void)cancelDisplayedMediaTask:(id)sender {
    (void)sender;
    if (!self.displayedMediaTaskId || !self.state->mediaTasks) return;
    self.state->mediaTasks->Cancel(self.displayedMediaTaskId.UTF8String ?: "");
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaImports {
    if (!self.state || self.state->pendingImports.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingImports.begin();
         item != self.state->pendingImports.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            [self commitMediaImportBatch:item->second.get()];
        } else if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Import média annulé.";
        } else {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Import média échoué : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingImports.erase(item);
    }
}

- (void)processCompletedMediaProxies {
    if (!self.state || self.state->pendingProxies.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    bool reload = false;
    for (auto item = self.state->pendingProxies.begin();
         item != self.state->pendingProxies.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            LibraryMedia* media =
                self.state->document.FindLibraryMedia(item->second.media_id);
            if (media) {
                const std::string previousProxyPath = media->proxy_path;
                media->proxy_path = item->second.stored_path;
                std::string message;
                if ([self persistEdits:message]) {
                    self.binSummaryLabel.stringValue = [NSString
                        stringWithFormat:@"Proxy prêt · %@",
                                         CMStringFromUtf8(media->filename)];
                    reload = true;
                } else {
                    media->proxy_path = previousProxyPath;
                    std::error_code ignored;
                    std::filesystem::remove(item->second.absolute_path,
                                            ignored);
                    self.binSummaryLabel.stringValue = [NSString
                        stringWithFormat:@"Proxy créé mais non attaché : %@",
                                         CMStringFromUtf8(message)];
                }
            }
        } else if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Génération proxy annulée.";
        } else {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec du proxy : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingProxies.erase(item);
    }
    if (reload) {
        [self reloadDecodeWorkers];
        [self rebuildMediaList];
    }
}

- (void)loadOrEnqueueWaveformForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    if (self.state->waveforms.count(mediaId) != 0) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path output = projectPath.parent_path() /
                                         ".cutmachine" / "waveforms" /
                                         (mediaId + ".waveform");
    AudioWaveform waveform;
    std::string error;
    if (LoadAudioWaveform(output.string(), waveform, error)) {
        self.state->waveforms.emplace(mediaId, std::move(waveform));
        self.state->overlayDirty = true;
        return;
    }
    [self enqueueWaveformForMediaIdentifier:identifier];
}

- (void)enqueueWaveformForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!source) return;
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    for (const auto& pending : self.state->pendingWaveforms)
        if (pending.second.media_id == mediaId) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(source->path);
    if (input.is_relative()) input = base / input;
    const std::filesystem::path output =
        base / ".cutmachine" / "waveforms" / (mediaId + ".waveform");
    const std::string label =
        "Waveform " + (media ? media->filename : input.filename().string());
    WaveformSettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Waveform, label,
        [input = input.lexically_normal(), output, duration = source->duration,
         settings](MediaTaskContext& context, std::string& taskError) {
            return GenerateAudioWaveform(input.string(), output.string(),
                                         duration, settings, context,
                                         taskError);
        });
    self.state->pendingWaveforms.emplace(taskId,
                                         PendingWaveform{mediaId, output});
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaWaveforms {
    if (!self.state || self.state->pendingWaveforms.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingWaveforms.begin();
         item != self.state->pendingWaveforms.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            AudioWaveform waveform;
            std::string error;
            if (LoadAudioWaveform(item->second.absolute_path.string(), waveform,
                                  error)) {
                self.state->waveforms[item->second.media_id] =
                    std::move(waveform);
                self.state->overlayDirty = true;
            } else {
                self.binSummaryLabel.stringValue =
                    [NSString stringWithFormat:@"Waveform illisible : %@",
                                               CMStringFromUtf8(error)];
            }
        } else if (task->second.state == MediaTaskState::Failed) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec waveform : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingWaveforms.erase(item);
    }
}

- (void)processCompletedMediaTranscriptions {
    if (!self.state || (self.state->pendingTranscriptions.empty() &&
                        self.state->pendingTimelineTranscriptions.empty()))
        return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingTranscriptions.begin();
         item != self.state->pendingTranscriptions.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        const PendingTranscription pending = item->second;
        item = self.state->pendingTranscriptions.erase(item);
        if (!pending.batch_id.empty()) {
            auto batch = self.state->pendingTimelineTranscriptions.find(
                pending.batch_id);
            if (batch != self.state->pendingTimelineTranscriptions.end()) {
                batch->second.task_ids.erase(task->first);
                if (task->second.state == MediaTaskState::Cancelled)
                    batch->second.error = "timeline transcription cancelled";
                else if (task->second.state != MediaTaskState::Succeeded)
                    batch->second.error = task->second.error;
            }
            continue;
        }
        if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Transcription annulée.";
            continue;
        }
        if (task->second.state != MediaTaskState::Succeeded) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec de la transcription : %@",
                                 CMStringFromUtf8(task->second.error)];
            continue;
        }

        Transcript transcript;
        std::string message;
        // ITW3 -- captions built here cut on the same words as every tool.
        if (!LoadSourceWords(pending.absolute_path.parent_path().parent_path(),
                             pending.media_id, transcript, message)) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Transcript illisible : %@",
                                           CMStringFromUtf8(message)];
            continue;
        }
        if (pending.timeline_id != self.state->activeTimelineId) {
            self.binSummaryLabel.stringValue =
                @"Transcript prêt · revenez sur la timeline d’origine puis "
                 "relancez l’action pour créer S1.";
            continue;
        }

        std::vector<SubtitleCue> cues;
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            if (track.kind != "video") continue;
            for (const DocumentClip& clip : track.clips) {
                if (clip.source_id != pending.media_id) continue;
                std::vector<SubtitleCue> clipCues;
                std::string ignored;
                if (SubtitleCuesForClip(transcript, clip, clipCues, ignored))
                    cues.insert(cues.end(), clipCues.begin(), clipCues.end());
            }
        }
        std::stable_sort(cues.begin(), cues.end(),
                         [](const SubtitleCue& left, const SubtitleCue& right) {
                             return left.timeline_in < right.timeline_in;
                         });
        if (cues.empty()) {
            self.binSummaryLabel.stringValue =
                @"Transcript prêt · ce rush n’est pas présent sur la "
                 "timeline active.";
            continue;
        }
        bool overlaps = false;
        for (size_t index = 1; index < cues.size(); ++index) {
            if (cues[index].timeline_in <
                cues[index - 1].timeline_in.add(cues[index - 1].duration)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            self.binSummaryLabel.stringValue =
                @"Transcript prêt · les occurrences du rush se chevauchent; "
                 "impossible de créer une seule piste S1.";
            continue;
        }

        int32_t trackIndex = 0;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            trackIndex = std::max(trackIndex, track.index + 1);
        EditError editError = EditError::None;
        AddTrackOperation operation = BuildSubtitleTrackEdit(cues, trackIndex);
        if (![self
                applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                           error:editError
                                         message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Sous-titres non créés (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        [self
            refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
        self.state->overlayDirty = true;
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Transcript prêt · %lu sous-titre%@ ajouté%@ "
                              "sur S1",
                             (unsigned long)cues.size(),
                             cues.size() == 1 ? @"" : @"s",
                             cues.size() == 1 ? @"" : @"s"];
    }

    for (auto item = self.state->pendingTimelineTranscriptions.begin();
         item != self.state->pendingTimelineTranscriptions.end();) {
        if (!item->second.task_ids.empty()) {
            ++item;
            continue;
        }
        PendingTimelineTranscription batch = std::move(item->second);
        item = self.state->pendingTimelineTranscriptions.erase(item);
        if (!batch.error.empty()) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Transcription de la timeline échouée : %@",
                                 CMStringFromUtf8(batch.error)];
            continue;
        }
        if (batch.timeline_id != self.state->activeTimelineId) {
            self.binSummaryLabel.stringValue =
                @"Transcripts prêts · revenez sur la timeline d’origine puis "
                 "relancez l’action pour créer S1.";
            continue;
        }

        std::map<Ulid, Transcript> transcripts;
        std::string message;
        bool loaded = true;
        for (const auto& transcriptPath : batch.transcript_paths) {
            Transcript transcript;
            if (!LoadSourceWords(
                    transcriptPath.second.parent_path().parent_path(),
                    transcriptPath.first, transcript, message)) {
                loaded = false;
                break;
            }
            transcripts.emplace(transcriptPath.first, std::move(transcript));
        }
        if (!loaded) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Transcript de timeline illisible : %@",
                                 CMStringFromUtf8(message)];
            continue;
        }

        const bool hasSolo =
            std::any_of(self.state->document.sequence.tracks.begin(),
                        self.state->document.sequence.tracks.end(),
                        [](const DocumentTrack& track) {
                            return track.kind == "audio" && track.solo;
                        });
        std::vector<SubtitleCue> cues;
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            if (track.kind != "audio" || track.muted ||
                (hasSolo && !track.solo))
                continue;
            for (const DocumentClip& clip : track.clips) {
                if (!ClipIsHeard(clip)) continue;
                const auto transcript = transcripts.find(clip.source_id);
                if (transcript == transcripts.end()) continue;
                std::vector<SubtitleCue> clipCues;
                std::string ignored;
                if (SubtitleCuesForClip(transcript->second, clip, clipCues,
                                        ignored))
                    cues.insert(cues.end(), clipCues.begin(), clipCues.end());
            }
        }
        std::stable_sort(cues.begin(), cues.end(),
                         [](const SubtitleCue& left, const SubtitleCue& right) {
                             return left.timeline_in < right.timeline_in;
                         });
        if (cues.empty()) {
            self.binSummaryLabel.stringValue =
                @"Transcription terminée, mais aucun mot ne correspond aux "
                 "coupes de la timeline.";
            continue;
        }
        bool overlaps = false;
        for (size_t index = 1; index < cues.size(); ++index) {
            if (cues[index].timeline_in <
                cues[index - 1].timeline_in.add(cues[index - 1].duration)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            self.binSummaryLabel.stringValue =
                @"Transcripts prêts · plusieurs pistes audio parlées se "
                 "chevauchent; impossible de créer une seule piste S1.";
            continue;
        }

        int32_t trackIndex = 0;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            trackIndex = std::max(trackIndex, track.index + 1);
        EditError editError = EditError::None;
        AddTrackOperation operation = BuildSubtitleTrackEdit(cues, trackIndex);
        if (![self
                applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                           error:editError
                                         message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Sous-titres non créés (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        [self
            refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
        self.state->overlayDirty = true;
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Timeline transcrite · %lu sous-titre%@ sur S1",
                             (unsigned long)cues.size(),
                             cues.size() == 1 ? @"" : @"s"];
    }
}

- (void)loadOrEnqueueThumbnailForMediaIdentifier:(NSString*)identifier {
    if ([self stageThumbnailForMediaIdentifier:identifier])
        [self.mediaCollection reloadData];
}

// PERF-2026-09. Returns whether the collection view now shows something it
// did not before, so a caller loading a whole library can reload it once
// instead of once per rush. -reloadData is O(items), so the startup loop was
// quadratic in the number of rushes on top of reading each thumbnail off the
// drive on the main thread.
- (BOOL)stageThumbnailForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state || self.mediaThumbnails[identifier])
        return NO;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    if (media && !media->has_video) return NO;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path output = projectPath.parent_path() /
                                         ".cutmachine" / "thumbnails" /
                                         (mediaId + ".png");
    NSImage* image = [[NSImage alloc]
        initWithContentsOfFile:[NSString stringWithUTF8String:output.c_str()]];
    if (image) {
        self.mediaThumbnails[identifier] = image;
        return YES;
    }
    [self enqueueThumbnailForMediaIdentifier:identifier];
    return NO;
}

- (void)enqueueThumbnailForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!source) return;
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    if (media && !media->has_video) {
        self.binSummaryLabel.stringValue =
            @"Ce média est audio seul · aucune vignette à générer.";
        return;
    }
    for (const auto& pending : self.state->pendingThumbnails)
        if (pending.second.media_id == mediaId) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(source->path);
    if (input.is_relative()) input = base / input;
    const std::filesystem::path output =
        base / ".cutmachine" / "thumbnails" / (mediaId + ".png");
    const std::string label =
        "Thumbnail " + (media ? media->filename : input.filename().string());
    ThumbnailSettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Thumbnail, label,
        [input = input.lexically_normal(), output, duration = source->duration,
         settings](MediaTaskContext& context, std::string& taskError) {
            return GenerateMediaThumbnail(input.string(), output.string(),
                                          duration, settings, context,
                                          taskError);
        });
    self.state->pendingThumbnails.emplace(taskId,
                                          PendingThumbnail{mediaId, output});
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaThumbnails {
    if (!self.state || self.state->pendingThumbnails.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    bool reload = false;
    for (auto item = self.state->pendingThumbnails.begin();
         item != self.state->pendingThumbnails.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            NSString* identifier =
                [NSString stringWithUTF8String:item->second.media_id.c_str()];
            NSImage* image = [[NSImage alloc]
                initWithContentsOfFile:
                    [NSString stringWithUTF8String:item->second.absolute_path
                                                       .c_str()]];
            if (image) {
                self.mediaThumbnails[identifier] = image;
                reload = true;
            } else {
                self.binSummaryLabel.stringValue =
                    @"La vignette générée est illisible.";
            }
        } else if (task->second.state == MediaTaskState::Failed) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec vignette : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingThumbnails.erase(item);
    }
    if (reload) [self.mediaCollection reloadData];
}

- (void)processCompletedMediaRelinks {
    if (!self.state || self.state->pendingRelinks.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingRelinks.begin();
         item != self.state->pendingRelinks.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        PendingRelink pending = item->second;
        item = self.state->pendingRelinks.erase(item);
        if (task->second.state != MediaTaskState::Succeeded) {
            if (task->second.state == MediaTaskState::Failed)
                self.binSummaryLabel.stringValue = [NSString
                    stringWithFormat:@"Reconnexion refusée : %@",
                                     CMStringFromUtf8(task->second.error)];
            continue;
        }

        const LibraryMedia* previousMedia =
            self.state->document.FindLibraryMedia(pending.media_id);
        if (!previousMedia) continue;
        const LibraryMedia previous = *previousMedia;
        Project stagedProject = self.state->project;
        ProjectEditLog stagedProjectLog = self.state->projectEditLog;
        std::string message;
        EditError editError = EditError::None;
        ProjectOperation relink = RelinkProjectMediaOperation{
            {{pending.media_id, pending.result->media, pending.stored_path}},
            std::nullopt};
        if (!stagedProjectLog.Apply(stagedProject, std::move(relink), editError,
                                    message) ||
            ![self commitProjectCandidate:stagedProject
                                  editLog:self.state->editLog
                               projectLog:stagedProjectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Reconnexion impossible (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        self.state->project = std::move(stagedProject);
        self.state->projectEditLog = std::move(stagedProjectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        self.state->document =
            self.state->project.MakeDocument(self.state->activeTimelineId);

        for (auto pendingTask = self.state->pendingProxies.begin();
             pendingTask != self.state->pendingProxies.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingProxies.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingWaveforms.begin();
             pendingTask != self.state->pendingWaveforms.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingWaveforms.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingThumbnails.begin();
             pendingTask != self.state->pendingThumbnails.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingThumbnails.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingTranscriptions.begin();
             pendingTask != self.state->pendingTranscriptions.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                const Ulid batchId = pendingTask->second.batch_id;
                if (!batchId.empty()) {
                    auto batch =
                        self.state->pendingTimelineTranscriptions.find(batchId);
                    if (batch !=
                        self.state->pendingTimelineTranscriptions.end()) {
                        batch->second.task_ids.erase(pendingTask->first);
                        batch->second.error =
                            "source relinked during timeline transcription";
                    }
                }
                pendingTask =
                    self.state->pendingTranscriptions.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }

        const std::filesystem::path projectPath = std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""));
        const std::filesystem::path base = projectPath.parent_path();
        const std::filesystem::path expectedProxy =
            base / ".cutmachine" / "proxies" / (pending.media_id + ".mov");
        std::filesystem::path oldProxy(previous.proxy_path);
        if (oldProxy.is_relative()) oldProxy = base / oldProxy;
        std::error_code ignored;
        if (!previous.proxy_path.empty() &&
            oldProxy.lexically_normal() == expectedProxy.lexically_normal())
            std::filesystem::remove(expectedProxy, ignored);
        std::filesystem::remove(base / ".cutmachine" / "waveforms" /
                                    (pending.media_id + ".waveform"),
                                ignored);
        std::filesystem::remove(
            base / ".cutmachine" / "thumbnails" / (pending.media_id + ".png"),
            ignored);
        std::filesystem::remove(
            base / ".cutmachine" / "transcripts" / (pending.media_id + ".json"),
            ignored);
        self.state->waveforms.erase(pending.media_id);
        NSString* identifier =
            [NSString stringWithUTF8String:pending.media_id.c_str()];
        [self.mediaThumbnails removeObjectForKey:identifier];
        if (const LibraryMedia* media =
                self.state->document.FindLibraryMedia(pending.media_id))
            self.state->mediaMetadata[pending.media_id] = *media;

        [self reloadDecodeWorkers];
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        const bool audioReady =
            audio->Open(self.state->document, base.string(), audioError);
        self.state->audioPlayback = std::move(audio);
        [self rebuildMediaList];
        if (self.state->sourceMonitorId == pending.media_id) {
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(pending.media_id);
            if (media)
                self.sourceMonitorTitleLabel.stringValue = [NSString
                    stringWithFormat:@"SOURCE — %@",
                                     CMStringFromUtf8(media->filename)];
            self.sourceOfflineMediaLabel.hidden =
                self.state->offlineSourceIds.count(pending.media_id) == 0;
        }
        [self loadOrEnqueueThumbnailForMediaIdentifier:identifier];
        const LibraryMedia* replacement =
            self.state->document.FindLibraryMedia(pending.media_id);
        if (replacement && replacement->has_audio)
            [self loadOrEnqueueWaveformForMediaIdentifier:identifier];
        if (replacement) {
            std::string codec = replacement->codec;
            std::transform(
                codec.begin(), codec.end(), codec.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (self.state->automaticProxiesEnabled &&
                (replacement->width > 1920 || codec == "hevc" ||
                 codec == "h265" || codec == "av1"))
                [self enqueueProxyForMediaIdentifier:identifier];
        }
        self.binSummaryLabel.stringValue =
            audioReady
                ? @"Média reconnecté · caches en cours de régénération"
                : [NSString stringWithFormat:
                                @"Média reconnecté · audio indisponible : %@",
                                CMStringFromUtf8(audioError)];
    }
}

- (void)processCompletedBatchRelinks {
    if (!self.state || self.state->pendingBatchRelinks.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingBatchRelinks.begin();
         item != self.state->pendingBatchRelinks.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        std::shared_ptr<BatchRelinkResult> result = item->second.result;
        item = self.state->pendingBatchRelinks.erase(item);
        if (task->second.state != MediaTaskState::Succeeded) {
            if (task->second.state == MediaTaskState::Failed)
                self.binSummaryLabel.stringValue = [NSString
                    stringWithFormat:@"Relink en lot échoué : %@",
                                     CMStringFromUtf8(task->second.error)];
            continue;
        }
        if (result->replacements.empty()) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Aucun média reconnecté · %lu introuvable%@ "
                                 @"· %lu ambigu%@ · %lu incompatible%@",
                                 (unsigned long)result->unmatched,
                                 result->unmatched == 1 ? @"" : @"s",
                                 (unsigned long)result->ambiguous,
                                 result->ambiguous == 1 ? @"" : @"s",
                                 (unsigned long)result->incompatible,
                                 result->incompatible == 1 ? @"" : @"s"];
            continue;
        }

        std::map<Ulid, LibraryMedia> previousMedia;
        for (const RelinkReplacement& replacement : result->replacements) {
            const LibraryMedia* previous =
                self.state->document.FindLibraryMedia(replacement.media_id);
            if (previous) previousMedia[replacement.media_id] = *previous;
        }
        Project stagedProject = self.state->project;
        ProjectEditLog stagedProjectLog = self.state->projectEditLog;
        std::string message;
        std::vector<ProjectRelinkItem> projectReplacements;
        projectReplacements.reserve(result->replacements.size());
        for (const RelinkReplacement& replacement : result->replacements)
            projectReplacements.push_back({replacement.media_id,
                                           replacement.media,
                                           replacement.stored_path});
        EditError editError = EditError::None;
        ProjectOperation relink = RelinkProjectMediaOperation{
            std::move(projectReplacements), std::nullopt};
        if (!stagedProjectLog.Apply(stagedProject, std::move(relink), editError,
                                    message) ||
            ![self commitProjectCandidate:stagedProject
                                  editLog:self.state->editLog
                               projectLog:stagedProjectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Relink en lot annulé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        self.state->project = std::move(stagedProject);
        self.state->projectEditLog = std::move(stagedProjectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        self.state->document =
            self.state->project.MakeDocument(self.state->activeTimelineId);
        std::set<Ulid> relinkedIds;
        for (const RelinkReplacement& replacement : result->replacements)
            relinkedIds.insert(replacement.media_id);

        for (auto pendingTask = self.state->pendingProxies.begin();
             pendingTask != self.state->pendingProxies.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingProxies.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingWaveforms.begin();
             pendingTask != self.state->pendingWaveforms.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingWaveforms.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingThumbnails.begin();
             pendingTask != self.state->pendingThumbnails.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingThumbnails.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingTranscriptions.begin();
             pendingTask != self.state->pendingTranscriptions.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                const Ulid batchId = pendingTask->second.batch_id;
                if (!batchId.empty()) {
                    auto batch =
                        self.state->pendingTimelineTranscriptions.find(batchId);
                    if (batch !=
                        self.state->pendingTimelineTranscriptions.end()) {
                        batch->second.task_ids.erase(pendingTask->first);
                        batch->second.error =
                            "source relinked during timeline transcription";
                    }
                }
                pendingTask =
                    self.state->pendingTranscriptions.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }

        const std::filesystem::path projectPath = std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""));
        const std::filesystem::path base = projectPath.parent_path();
        for (const Ulid& mediaId : relinkedIds) {
            const std::filesystem::path expectedProxy =
                base / ".cutmachine" / "proxies" / (mediaId + ".mov");
            const auto previous = previousMedia.find(mediaId);
            if (previous != previousMedia.end() &&
                !previous->second.proxy_path.empty()) {
                std::filesystem::path oldProxy(previous->second.proxy_path);
                if (oldProxy.is_relative()) oldProxy = base / oldProxy;
                std::error_code ignored;
                if (oldProxy.lexically_normal() ==
                    expectedProxy.lexically_normal())
                    std::filesystem::remove(expectedProxy, ignored);
            }
            std::error_code ignored;
            std::filesystem::remove(
                base / ".cutmachine" / "waveforms" / (mediaId + ".waveform"),
                ignored);
            std::filesystem::remove(
                base / ".cutmachine" / "thumbnails" / (mediaId + ".png"),
                ignored);
            std::filesystem::remove(
                base / ".cutmachine" / "transcripts" / (mediaId + ".json"),
                ignored);
            self.state->waveforms.erase(mediaId);
            NSString* identifier =
                [NSString stringWithUTF8String:mediaId.c_str()];
            [self.mediaThumbnails removeObjectForKey:identifier];
            if (const LibraryMedia* media =
                    self.state->document.FindLibraryMedia(mediaId))
                self.state->mediaMetadata[mediaId] = *media;
        }

        [self reloadDecodeWorkers];
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        const bool audioReady =
            audio->Open(self.state->document, base.string(), audioError);
        self.state->audioPlayback = std::move(audio);
        [self rebuildMediaList];
        for (const Ulid& mediaId : relinkedIds) {
            NSString* identifier =
                [NSString stringWithUTF8String:mediaId.c_str()];
            [self loadOrEnqueueThumbnailForMediaIdentifier:identifier];
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(mediaId);
            if (!media) continue;
            if (media->has_audio)
                [self loadOrEnqueueWaveformForMediaIdentifier:identifier];
            std::string codec = media->codec;
            std::transform(
                codec.begin(), codec.end(), codec.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (self.state->automaticProxiesEnabled &&
                (media->width > 1920 || codec == "hevc" || codec == "h265" ||
                 codec == "av1"))
                [self enqueueProxyForMediaIdentifier:identifier];
            if (self.state->sourceMonitorId == mediaId) {
                self.sourceMonitorTitleLabel.stringValue = [NSString
                    stringWithFormat:@"SOURCE — %@",
                                     CMStringFromUtf8(media->filename)];
                self.sourceOfflineMediaLabel.hidden =
                    self.state->offlineSourceIds.count(mediaId) == 0;
            }
        }
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"%lu média%@ reconnecté%@ · %lu introuvable%@ · "
                             @"%lu ambigu%@ · %lu incompatible%@%@",
                             (unsigned long)relinkedIds.size(),
                             relinkedIds.size() == 1 ? @"" : @"s",
                             relinkedIds.size() == 1 ? @"" : @"s",
                             (unsigned long)result->unmatched,
                             result->unmatched == 1 ? @"" : @"s",
                             (unsigned long)result->ambiguous,
                             result->ambiguous == 1 ? @"" : @"s",
                             (unsigned long)result->incompatible,
                             result->incompatible == 1 ? @"" : @"s",
                             audioReady ? @"" : @" · audio indisponible"];
    }
}

- (void)refreshAfterProjectRename {
    // A rename changes browser/title metadata only. Rehydrate the active
    // document so a later timeline save cannot restore its previous name,
    // while keeping decode workers and playback caches alive.
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    self.programMonitorTitleLabel.stringValue =
        CMRecordTitle(self.state->document.sequence.name);
    self.window.title = CMWindowTitle(self.state->project.name,
                                      self.state->document.sequence.name);
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (void)refreshAfterProjectMutation {
    if (!self.state->project.FindTimeline(self.state->activeTimelineId))
        self.state->activeTimelineId = self.state->project.active_timeline_id;
    if (!self.state->project.FindTimeline(self.state->activeTimelineId) &&
        !self.state->project.timelines.empty())
        self.state->activeTimelineId = self.state->project.timelines.front().id;
    self.state->document =
        self.state->project.MakeDocument(self.state->activeTimelineId);
    const auto storedLog =
        self.state->timelineEditLogs.find(self.state->activeTimelineId);
    self.state->editLog = storedLog == self.state->timelineEditLogs.end()
                              ? EditLog{}
                              : storedLog->second;
    self.state->targetedTrackIds.clear();
    const auto storedTargets =
        self.state->timelineTargetTracks.find(self.state->activeTimelineId);
    if (storedTargets != self.state->timelineTargetTracks.end()) {
        for (const Ulid& id : storedTargets->second)
            if (self.state->document.FindTrack(id))
                self.state->targetedTrackIds.insert(id);
    }
    if (self.state->targetedTrackIds.empty())
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            self.state->targetedTrackIds.insert(track.id);
    self.state->timeline = std::make_unique<Timeline>(self.state->document);
    self.state->interaction = std::make_unique<TimelineInteraction>(
        self.state->document, self.state->editLog, self.state->viewport);
    self.state->interaction->SetLinkedSelectionEnabled(
        self.state->linkedSelection);
    // PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The video tracks follow the
    // new document before the decoders reload: -reloadDecodeWorkers resolves
    // the decoded positions on them, and when the active timeline has just
    // gone (a creation undone) the old list named tracks the new timeline
    // does not have, which threw out of Timeline::ResolveTrack.
    [self rebuildVideoTrackIds];
    [self reloadDecodeWorkers];
    [self refreshTimelineAfterEditFromPosition:RationalTime{
                                                   0,
                                                   self.state->document.sequence
                                                       .frame_rate.num}];
    [self refreshBinControlsSelecting:self.selectedBinId ?: @"__all__"];
    [self updateSelectionInfo];
    self.state->overlayDirty = true;
}

- (std::filesystem::path)decodeBasePath {
    return std::filesystem::absolute(
               std::filesystem::path(self.documentPath.UTF8String ?: ""))
        .parent_path();
}

// PERF10 -- DecoderPool.h. Opening a project used to open a decoder for
// every video source, off the main thread but all at once; on a 169-rush 4K
// shoot that was 1,030 threads and 2 GB before the first frame, and the
// decoders then kept what they decoded. What opening a project needs to know
// up front is only which media are missing, so the relink flow and the
// offline badges are right: that is a stat per file, done here in the
// background. Decoders open when the monitor asks for a source
// (-requestFrame:ofSource:).
//
// The trade: a file that is there but does not match what the project
// declares (another frame rate, fewer frames) used to be marked offline at
// open; it is now marked offline the first time it is decoded.
- (void)checkSourceAvailabilityInBackground {
    if (!self.state || !self.state->frameCache) return;
    const std::filesystem::path base = [self decodeBasePath];
    auto pending = std::make_shared<std::vector<PendingDecodeOpen>>();
    for (const DocumentSource& source : self.state->document.sources) {
        if (auto item = DecodeOpenFor(self.state->document, source, base,
                                      self.state->proxiesEnabled))
            pending->push_back(std::move(*item));
    }
    self.state->checkingSourceAvailability = !pending->empty();
    if (pending->empty()) {
        // PERF12 -- nothing to check: what playback shows next may open.
        self.state->sourceAvailabilityKnown = true;
        [self openDecodersAhead:self.state->requestedPosition];
        return;
    }
    auto available = std::make_shared<std::vector<char>>(pending->size(), 0);
    AppState* state = self.state;
    const uint64_t generation = state->sourceAvailabilityGeneration;
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      ParallelForEachIndex(pending->size(), [&](size_t index) {
          (*available)[index] = DecodeMediaIsAvailable((*pending)[index]);
      });
      // Scheduled on the run loop rather than dispatched to the main queue.
      // The main queue is serial, so a block sent to it cannot run while
      // another one is still executing -- which is exactly the situation any
      // caller that spins the run loop waiting for this is in, the
      // in-process smoke test among them. Common modes additionally let the
      // install land during event tracking, the same reason the display
      // timer is registered there.
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || strongSelf.state != state ||
            state->sourceAvailabilityGeneration != generation)
            return;
        state->checkingSourceAvailability = false;
        bool changed = false;
        for (size_t index = 0; index < pending->size(); ++index) {
            const PendingDecodeOpen& item = (*pending)[index];
            // A decoder that opened meanwhile has already answered.
            if ((*available)[index] ||
                !state->document.FindSource(item.source_id) ||
                state->workers.count(item.source_id) != 0)
                continue;
            std::fprintf(stderr, "Source offline %s at %s\n",
                         item.source_id.c_str(), item.original.c_str());
            changed |= state->offlineSourceIds.insert(item.source_id).second;
        }
        // PERF12 -- the missing media are known: what playback shows next
        // may open, ahead of it (a refresh plans it too).
        state->sourceAvailabilityKnown = true;
        if (changed)
            [strongSelf refreshDecodedPositions];
        else
            [strongSelf openDecodersAhead:state->requestedPosition];
      });
      CFRunLoopWakeUp(mainLoop);
    });
}

// PERF10. Every frame the monitors ask for goes through here. A source with
// a decoder gets the request and counts as used now; one without is opened
// in the background, and asked again once it is installed.
- (void)requestFrame:(int64_t)frame ofSource:(const Ulid&)sourceId {
    [self requestFrame:frame ofSource:sourceId plan:{}];
}

- (void)requestFrame:(int64_t)frame
            ofSource:(const Ulid&)sourceId
                plan:(const DecodeWorker::ReadPlan&)plan {
    const auto worker = self.state->workers.find(sourceId);
    if (worker != self.state->workers.end()) {
        self.state->decoderUsage.Touch(sourceId);
        worker->second->RequestFrame(frame, plan);
        return;
    }
    [self openDecoderForSource:sourceId];
}

// PERF15 -- PlaybackReader.h. The one reader reads ahead of forward
// playback, where the frames shown follow on in their files; played
// backward, or at rest, each decoder reads its own file as before. Started
// again on a copy of the document after an edit or a zoom, so that what it
// reads is what is shown.
- (void)syncPlaybackReader {
    AppState* state = self.state;
    bool wanted = state->playbackDirection > 0 && state->timeline != nullptr;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    wanted = wanted && gUiPlayProbeReader;
#endif
    if (!wanted) {
        state->playbackReader.Stop();
        return;
    }
    state->playbackReader.Start(
        std::make_unique<Document>(state->document),
        state->programMonitorZoom > 0.0, state->requestedPosition,
        playback_reader::kReaderHorizonSeconds * state->playbackDirection);
}

// PERF14 -- how playback reads a clip ahead (ReadAhead.h): within the
// source frames it shows, seen from `position` -- its out point, or the
// first frame a cutaway above hides --, and its bytes asked of the system
// ahead once it is on screen, its first second while it is still to come.
// Nothing at rest, where a decoder fills a few frames and nobody waits on
// the disk.
- (DecodeWorker::ReadPlan)readPlanForClip:(const Ulid&)clipId
                                  onTrack:(const Ulid&)trackId
                                       at:(RationalTime)position
                                 onScreen:(BOOL)onScreen {
    DecodeWorker::ReadPlan plan;
    const int direction = self.state->playbackDirection;
    if (direction == 0 || !self.state->timeline) return plan;
    int64_t byteSeconds = read_ahead::kByteHorizonSeconds;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (!gUiPlayProbeReadAhead) return plan;
    byteSeconds = gUiPlayProbeByteSeconds;
#endif
    const DocumentClip* clip = self.state->document.FindClip(clipId);
    if (clip == nullptr) return plan;
    // PERF15 -- the playback reader reads this clip's bytes: asked of the
    // system a second time here, they would be read twice at once.
    if (self.state->playbackReader.Running() &&
        self.state->playbackReader.Covers(clip->source_id))
        byteSeconds = 0;
    const int64_t reach =
        std::max<int64_t>(byteSeconds, read_ahead::kByteChunkSeconds * 2) *
        std::abs(direction);
    const monitor_occlusion::PictureLookup pictures =
        playback_reads::PicturesFromLibrary(self.state->document);
    const bool zoomed = self.state->programMonitorZoom > 0.0;
    plan.span = playback_reads::VisibleReadSpan(
        self.state->document, *self.state->timeline, trackId, *clip, position,
        direction, RationalTime{reach, 1}, pictures, zoomed);
    // PERF17 -- the next piece of the same rush, decoded on into.
    bool nextPiece = true;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    nextPiece = gUiPlayProbeNextPiece;
#endif
    if (nextPiece)
        plan.next_piece = playback_reads::NextPieceFrame(
            self.state->document, *self.state->timeline, trackId, *clip,
            position, direction, RationalTime{reach, 1}, pictures, zoomed);
    if (byteSeconds <= 0) return plan;
    plan.byte_horizon = playback_reads::SourceFramesIn(
        self.state->document, *clip,
        onScreen ? byteSeconds : read_ahead::kByteChunkSeconds);
    plan.byte_chunk = playback_reads::SourceFramesIn(
        self.state->document, *clip, read_ahead::kByteChunkSeconds);
    return plan;
}

// Whether an open was started: none for a source already open or on its
// way, offline, closing (it reopens once closed), or with no video to decode.
- (BOOL)openDecoderForSource:(const Ulid&)sourceId {
    AppState* state = self.state;
    if (!state || !state->frameCache || state->workers.count(sourceId) != 0 ||
        state->openingDecoders.count(sourceId) != 0 ||
        state->offlineSourceIds.count(sourceId) != 0)
        return NO;
    if (state->closingDecoders.count(sourceId) != 0) {
        state->reopenAfterClose.insert(sourceId);
        return NO;
    }
    const DocumentSource* source = state->document.FindSource(sourceId);
    if (!source) return NO;
    const std::optional<PendingDecodeOpen> item =
        DecodeOpenFor(state->document, *source, [self decodeBasePath],
                      state -> proxiesEnabled);
    if (!item) return NO;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) ++gUiSmokeDecodeOpens;
    if (!gUiPlayProbeProject.empty()) {
        ++gUiPlayProbeOpens;
        if (!gUiPlayProbeOpened.insert(sourceId).second) ++gUiPlayProbeReopens;
    }
#endif
    state->openingDecoders.insert(sourceId);
    state->openingSince[sourceId] = std::chrono::steady_clock::now();
    // Both outlive this work and are internally locked, so the opening
    // thread may use them directly.
    FrameCache* cache = state->frameCache.get();
    PerformanceMetrics* metrics = state->performanceMetrics.get();
    const DocumentSource declared = *source;
    const PendingDecodeOpen pending = *item;
    // A unique_ptr cannot be captured by a block copied between queues; a
    // shared_ptr holding one can.
    auto opened = std::make_shared<std::unique_ptr<DecodeWorker>>();
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      auto worker =
          std::make_unique<DecodeWorker>(pending.source_id, *cache, *metrics);
      bool ready = worker->Open(pending.selected, 5);
      // A proxy that does not open, or is not the media the project
      // declares, falls back to the original, as it always has.
      if (pending.using_proxy &&
          (!ready || !DecodeWorkerMatchesSource(*worker, declared))) {
          worker = std::make_unique<DecodeWorker>(pending.source_id, *cache,
                                                  *metrics);
          ready = worker->Open(pending.original, 5);
      }
      if (ready) *opened = std::move(worker);
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || strongSelf.state != state) return;
        [strongSelf installDecoder:opened opening:pending];
      });
      CFRunLoopWakeUp(mainLoop);
    });
    return YES;
}

- (void)installDecoder:(std::shared_ptr<std::unique_ptr<DecodeWorker>>)opened
               opening:(const PendingDecodeOpen&)pending {
    AppState* state = self.state;
    const Ulid sourceId = pending.source_id;
    state->openingDecoders.erase(sourceId);
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    // PERF12 -- what an open costs on the probed drive, at rest and while
    // playback streams.
    const auto openedSince = state->openingSince.find(sourceId);
    if (!gUiPlayProbeProject.empty() &&
        openedSince != state->openingSince.end())
        std::fprintf(
            stderr, "probe open %s took %.2f s (%s)\n",
            std::filesystem::path(pending.selected).filename().string().c_str(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          openedSince->second)
                .count(),
            state->playbackDirection != 0 ? "playing" : "at rest");
#endif
    state->openingSince.erase(sourceId);
    std::unique_ptr<DecodeWorker> worker = std::move(*opened);
    const DocumentSource* source = state->document.FindSource(sourceId);
    const std::optional<PendingDecodeOpen> current =
        source ? DecodeOpenFor(state->document, *source, [self decodeBasePath],
                               state -> proxiesEnabled)
               : std::nullopt;
    // The source went away, or names other media than when the open started
    // (relinked, proxies toggled): the next request opens the right file.
    // PERF12 -- every path below plans the next open ahead, a refresh too.
    if (!current || current->selected != pending.selected ||
        state->workers.count(sourceId) != 0) {
        worker.reset();
        if (current)
            [self refreshDecodedPositions];
        else
            [self openDecodersAhead:state->requestedPosition];
        return;
    }
    if (!worker || !DecodeWorkerMatchesSource(*worker, *source)) {
        if (worker)
            std::fprintf(
                stderr, "Source %s declares rate %d/%d but media is %d/%d\n",
                sourceId.c_str(), source->rate.num, source->rate.den,
                worker->FrameRateNumerator(), worker->FrameRateDenominator());
        else
            std::fprintf(stderr, "Source offline %s at %s\n", sourceId.c_str(),
                         pending.original.c_str());
        worker.reset();
        state->offlineSourceIds.insert(sourceId);
        [self refreshDecodedPositions];
        return;
    }
    state->offlineSourceIds.erase(sourceId);
    worker->SetPlaying(state->playbackDirection != 0);
    // PERF15 -- where the playback reader finds this source's pictures.
    state->playbackReader.SetSource(sourceId, pending.selected,
                                    worker->SampleTable());
    worker->Start();
    state->workers.emplace(sourceId, std::move(worker));
    state->decoderUsage.Touch(sourceId);
    [self trimDecoderPool];
    [self refreshDecodedPositions];
}

// PERF10 -- DecoderPool.h. Past the capacity, close the least recently used
// decoders that neither monitor shows and the next second of playback does
// not cross into. PERF12 -- nor one that playback reaches next, as last
// planned (-openDecodersAhead:).
- (void)trimDecoderPool {
    AppState* state = self.state;
    if (state->workers.size() <= state->decoderCapacity) return;
    std::set<std::string> needed(state->upcomingSourceIds.begin(),
                                 state->upcomingSourceIds.end());
    needed.insert(state->plannedSourceIds.begin(),
                  state->plannedSourceIds.end());
    for (const ResolvedSlot& slot : state->requested)
        if (slot.active) needed.insert(slot.sourceId);
    // PERF13 -- a layer hidden under an opaque shot is on screen all the
    // same: closed, it would reopen when the shot ends, seconds on this drive.
    for (const ResolvedSlot& slot : state->hiddenSlots)
        needed.insert(slot.sourceId);
    if (state->sourceMonitor && !state->sourceMonitorId.empty())
        needed.insert(state->sourceMonitorId);
    std::vector<std::string> open;
    open.reserve(state->workers.size());
    for (const auto& worker : state->workers) open.push_back(worker.first);
    for (const std::string& sourceId :
         state->decoderUsage.ToClose(open, needed, state->decoderCapacity))
        [self closeDecoderForSource:sourceId];
}

// PERF12 -- DecoderPool.h. On the external drive projects are cut from, an
// open takes seconds while playback streams another rush from it, and the
// one-second warm-up above came far too late: every cut to a rush not yet
// open stopped the picture. So the rushes playback shows next -- forward of
// the playhead at rest, in the sense of travel in playback -- are opened
// before it gets there, soonest first, one at a time, within the capacity.
// Planned again at every position asked for, every install and every close;
// what is planned is held (-trimDecoderPool).
- (void)openDecodersAhead:(RationalTime)position {
    AppState* state = self.state;
    if (!state) return;
    state->plannedSourceIds.clear();
    if (!state->timeline || !state->frameCache) return;
    std::vector<decoder_pool::Need> needs = decoder_pool::UpcomingNeeds(
        state->document, position, state->playbackDirection,
        RationalTime{decoder_pool::kLookaheadSeconds, 1});
    if (state->sourceMonitor && !state->sourceMonitorId.empty())
        needs.insert(needs.begin(),
                     decoder_pool::Need{state->sourceMonitorId, {0, 1}});
    decoder_pool::PoolState pool;
    for (const auto& worker : state->workers) pool.open.insert(worker.first);
    const auto now = std::chrono::steady_clock::now();
    for (const Ulid& sourceId : state->openingDecoders) {
        const auto since = state->openingSince.find(sourceId);
        pool.opening[sourceId] = since == state->openingSince.end()
                                     ? std::chrono::steady_clock::duration{}
                                     : now - since->second;
    }
    pool.closing = state->closingDecoders;
    pool.offline = state->offlineSourceIds;
    const decoder_pool::Plan plan =
        decoder_pool::PlanOpens(needs, pool, state->decoderCapacity);
    state->plannedSourceIds.insert(plan.keep.begin(), plan.keep.end());
    if (!state->sourceAvailabilityKnown || state->checkingSourceAvailability)
        return;
    size_t started = 0;
    for (const Ulid& sourceId : plan.open) {
        if (started >= plan.budget) break;
        if ([self openDecoderForSource:sourceId]) ++started;
    }
}

// Stopping joins the decode thread, possibly mid-picture, and destroying the
// worker frees the codec and its threads: tens of milliseconds for a 4K
// decoder, which would be a hitch at the cut that made room for another, so
// it happens off the main thread. The frames it cached stay in the frame
// cache until its budget evicts them.
- (void)closeDecoderForSource:(const Ulid&)sourceId {
    AppState* state = self.state;
    auto node = state->workers.extract(sourceId);
    if (node.empty()) return;
    state->decoderUsage.Forget(sourceId);
    state->closingDecoders.insert(sourceId);
    auto closing = std::make_shared<std::unique_ptr<DecodeWorker>>(
        std::move(node.mapped()));
    const Ulid closed = sourceId;
    __weak AppDelegate* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      closing->reset();
      CFRunLoopRef mainLoop = CFRunLoopGetMain();
      CFRunLoopPerformBlock(mainLoop, kCFRunLoopCommonModes, ^{
        AppDelegate* strongSelf = weakSelf;
        if (!strongSelf || strongSelf.state != state) return;
        state->closingDecoders.erase(closed);
        if (state->reopenAfterClose.erase(closed) != 0)
            [strongSelf openDecoderForSource:closed];
        else
            [strongSelf openDecodersAhead:state->requestedPosition];
      });
      CFRunLoopWakeUp(mainLoop);
    });
}

// Recomposite both monitors from what the decoders now hold, and ask the
// decoders for their current positions again.
- (void)refreshDecodedPositions {
    AppState* state = self.state;
    state->rendered.clear();
    state->sourceRendered = {};
    [self requestResolvedPosition:state->requestedPosition];
    if (state->sourceMonitor) {
        [self requestSourcePosition:state->sourceMonitorPosition];
        self.sourceOfflineMediaLabel.hidden =
            state->offlineSourceIds.count(state->sourceMonitorId) == 0;
    }
    state->overlayDirty = true;
}

// PERF-2026-09. This used to stop every decoder and reopen every one of
// them, on the main thread, on each call -- and it is called after every
// project mutation and every proxy toggle. Reopening is an
// avformat_open_input plus avformat_find_stream_info per rush, which on the
// external drives this project is cut from is hundreds of milliseconds each,
// so a project holding twenty rushes froze the editor for seconds every time
// a source list was touched at all. Clearing each source from the frame
// cache alongside threw away decoded pictures that were still valid, making
// the next few frames of playback re-decode as well.
//
// A worker already open on exactly the media the document now names is still
// correct, and so is everything it has cached, so it is kept as-is. Since
// PERF10 nothing else is opened here: availability is a stat per file, and
// the monitors open what they show when they ask for it.
//
// One case still reopens every time: a source whose proxy exists but is
// rejected as incompatible falls back to the original, so the worker's path
// never equals the proxy path the next call selects. That is the old
// behaviour, not a regression, and it stops as soon as the bad proxy is
// removed or regenerated.
- (void)reloadDecodeWorkers {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) ++gUiSmokeDecodeReloads;
#endif
    if (!self.state->frameCache) return;
    AppState* state = self.state;
    // An availability check still in flight answers for the sources as they
    // were; this pass answers for them as they are.
    ++state->sourceAvailabilityGeneration;
    state->checkingSourceAvailability = false;
    const std::filesystem::path base = [self decodeBasePath];
    std::map<Ulid, std::unique_ptr<DecodeWorker>> retained;
    std::set<Ulid> offline;
    for (const DocumentSource& source : state->document.sources) {
        const std::optional<PendingDecodeOpen> item =
            DecodeOpenFor(state->document, source, base, state->proxiesEnabled);
        if (!item) {
            state->frameCache->ClearSource(source.id);
            continue;
        }
        if (!DecodeMediaIsAvailable(*item)) offline.insert(source.id);
        const auto existing = state->workers.find(source.id);
        if (existing != state->workers.end()) {
            if (existing->second->Path() == item->selected &&
                DecodeWorkerMatchesSource(*existing->second, source)) {
                retained.emplace(source.id, std::move(existing->second));
                state->workers.erase(existing);
                continue;
            }
            // Stopped before its cached pictures are cleared, so that none
            // of the old media lands in the cache afterwards.
            existing->second->Stop();
        }
        state->frameCache->ClearSource(source.id);
    }
    // Whatever is still here belongs to a source the document no longer has,
    // or to one whose media changed.
    std::vector<Ulid> stale;
    for (const auto& worker : state->workers) stale.push_back(worker.first);
    for (const Ulid& sourceId : stale) [self closeDecoderForSource:sourceId];
    state->workers = std::move(retained);
    state->offlineSourceIds = std::move(offline);
    // PERF12 -- this pass answered for every source, so what playback shows
    // next may open ahead even when the check it supersedes never lands.
    state->sourceAvailabilityKnown = true;
    [self refreshDecodedPositions];
}

- (void)commitMediaImportBatch:(ProbedImportBatch*)batch {
    if (!batch) return;
    const std::string targetBin =
        batch->target_bin.empty() ||
                self.state->document.FindBin(batch->target_bin)
            ? batch->target_bin
            : std::string{};
    IngestOutcome outcome;
    std::string errorName;
    std::string message;
    [self commitProbedMedia:batch->files
               documentPath:batch->document_path
                  targetBin:targetBin
                    outcome:outcome
                  errorName:errorName
                    message:message];
}

// PRD7 -- docs/roadmap/PRODUIT-2026-10.md. The engine adds what the task
// read (AddProbedMedia, Ingest.h), as it does for --ingest: known files, new
// ones, the target bin, the audio level. What stays here is the window's:
// putting the document back if it does not validate or cannot be written,
// and what a new rush needs on screen.
//
// F2.4 -- ROADMAP.md. The File menu's import and an agent's ingest_media
// (McpLiveBackend::SetIngestCommitCallback) both end here, so that a rush an
// agent imports reaches the browser as one imported by hand. An unset
// `targetBin` leaves a known rush in its bin, as --ingest does; a set one,
// the root (empty) included, files it there.
//
// DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. The new rushes and the
// moves are the plan's IngestMediaOperation, applied through the timeline's
// journal on copies and published once written (S3): Cmd+Z takes an import
// back. A target bin the document no longer has is refused by name.
- (BOOL)commitProbedMedia:(const std::vector<ProbedIngestFile>&)files
             documentPath:(const std::filesystem::path&)documentPath
                targetBin:(const std::optional<Ulid>&)targetBin
                  outcome:(IngestOutcome&)outcome
                errorName:(std::string&)errorName
                  message:(std::string&)message {
    errorName.clear();
    message.clear();
    const IngestPlan plan = PlanProbedMedia(
        self.state->document, documentPath.parent_path(), files, targetBin);
    outcome = plan.outcome;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    ApplyIngestRepairs(stagedDocument, plan);
    if (plan.HasJournalEntry()) {
        EditError editError = EditError::None;
        if (!stagedLog.Apply(stagedDocument, plan.operation, editError,
                             message))
            errorName = EditErrorName(editError);
    }
    const size_t added = outcome.added_ids.size();
    const size_t completed = outcome.completed_ids.size();
    const size_t moved = outcome.moved;
    const size_t rejected = outcome.errors.size();
    // The rushes whose metadata the document has just learnt: the new ones,
    // and those it knew without having read them.
    std::vector<Ulid> readMedia = outcome.added_ids;
    readMedia.insert(readMedia.end(), outcome.completed_ids.begin(),
                     outcome.completed_ids.end());

    if (errorName.empty() && outcome.changed) {
        if (!stagedDocument.Validate(message)) {
            errorName = "ValidationFailed";
        } else if (![self persistStagedDocument:stagedDocument
                                        editLog:stagedLog
                                        message:message]) {
            errorName = "IoError";
        }
    }
    if (!errorName.empty()) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Import impossible : %@",
                                       CMStringFromUtf8(message)];
        return NO;
    }
    if (outcome.changed) {
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
    }
    // PERF10 -- no decoder is opened for an imported rush: the monitor opens
    // one when it first shows it. The file was probed a moment ago, so it is
    // not offline.
    for (const Ulid& mediaId : readMedia) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(mediaId);
        if (media) self.state->mediaMetadata[mediaId] = *media;
    }
    if (!readMedia.empty()) {
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        if (audio->Open(self.state->document,
                        documentPath.parent_path().string(), audioError))
            self.state->audioPlayback = std::move(audio);
    }
    for (const Ulid& mediaId : readMedia) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(mediaId);
        if (!media) continue;
        std::string codec = media->codec;
        std::transform(codec.begin(), codec.end(), codec.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (self.state->automaticProxiesEnabled &&
            (media->width > 1920 || codec == "hevc" || codec == "h265" ||
             codec == "av1"))
            [self enqueueProxyForMediaIdentifier:
                      [NSString stringWithUTF8String:mediaId.c_str()]];
        if (media->has_audio)
            [self loadOrEnqueueWaveformForMediaIdentifier:
                      [NSString stringWithUTF8String:mediaId.c_str()]];
        [self loadOrEnqueueThumbnailForMediaIdentifier:
                  [NSString stringWithUTF8String:mediaId.c_str()]];
    }
    // The bin the rushes went to is shown; without one, new rushes are at
    // the root, and an ingest that added none leaves the selection alone.
    NSString* selected = nil;
    if (targetBin)
        selected = targetBin->empty()
                       ? @"__root__"
                       : [NSString stringWithUTF8String:targetBin->c_str()];
    else if (added)
        selected = @"__root__";
    [self refreshBinControlsSelecting:selected];
    NSMutableArray<NSString*>* results = [NSMutableArray array];
    if (added)
        [results addObject:[NSString stringWithFormat:@"%lu rush%@ importé%@",
                                                      (unsigned long)added,
                                                      added == 1 ? @"" : @"es",
                                                      added == 1 ? @"" : @"s"]];
    if (completed)
        [results
            addObject:[NSString stringWithFormat:@"%lu rush%@ complété%@",
                                                 (unsigned long)completed,
                                                 completed == 1 ? @"" : @"es",
                                                 completed == 1 ? @"" : @"s"]];
    if (moved)
        [results addObject:[NSString stringWithFormat:@"%lu rush%@ déplacé%@",
                                                      (unsigned long)moved,
                                                      moved == 1 ? @"" : @"es",
                                                      moved == 1 ? @"" : @"s"]];
    if (rejected)
        [results
            addObject:[NSString stringWithFormat:@"%lu refusé%@",
                                                 (unsigned long)rejected,
                                                 rejected == 1 ? @"" : @"s"]];
    if (results.count == 0) [results addObject:@"Aucun changement"];
    self.binSummaryLabel.stringValue =
        [results componentsJoinedByString:@" · "];
    return YES;
}

// R6 -- docs/roadmap/REEL-2026-09.md. Reads the sequence's look out of the
// project package when the document names a digest the app does not hold
// yet, and drops it when the document names none. The read checks the file
// against that digest (LoadSequenceLookLut), so a .cube replaced under the
// same name is refused rather than shown.
//
// A read that fails leaves the monitor without the look, which is a picture
// that is not the film's: it is reported, once per digest, because there is
// nothing else the monitor can draw and silence would be the worse of the
// two.
- (void)refreshSequenceLookLut {
    if (!self.state) return;
    const SequenceLut& look = self.state->document.sequence.look_lut;
    if (look.digest == self.state->lookLut.digest) return;
    self.state->lookLut = LoadedLookLut{};
    if (look.digest.empty()) return;
    const std::string projectDirectory =
        self.documentPath.length == 0
            ? std::string()
            : std::filesystem::path(self.documentPath.UTF8String)
                  .parent_path()
                  .string();
    LoadedLookLut loaded;
    std::string error;
    if (projectDirectory.empty() ||
        !LoadSequenceLookLut(self.state->document.sequence,
                             self.state->document.color_management,
                             projectDirectory, loaded, error)) {
        if (self.state->reportedLookLuts.insert(look.digest).second) {
            std::fprintf(stderr, "Unable to read the sequence look LUT: %s\n",
                         projectDirectory.empty()
                             ? "the project has no package on disk yet"
                             : error.c_str());
        }
        return;
    }
    self.state->lookLut = std::move(loaded);
}

// The width of the ruler's widest timecode, the one at the timeline's end.
- (double)rulerLabelWidth {
    NSFont* font =
        [NSFont monospacedSystemFontOfSize:ui::theme::kFontSizeCaption
                                    weight:NSFontWeightBold];
    NSString* label = CMStringFromUtf8(
        FormatTimecode(self.state->duration, [self playheadFrameRate]));
    return std::ceil(
        [label sizeWithAttributes:@{NSFontAttributeName : font}].width);
}

// D2 -- docs/roadmap/DIALOGUE-2026-10.md. A marker's note as the atlas draws
// it: its only regular sans face (MetalUiAtlas.mm), so the box behind the
// note is as wide as the note.
- (double)markerNoteWidth:(const std::string&)note {
    NSFont* font = [NSFont systemFontOfSize:ui::theme::kFontSizeSmall
                                     weight:NSFontWeightMedium];
    return std::ceil([CMStringFromUtf8(note)
                         sizeWithAttributes:@{NSFontAttributeName : font}]
                         .width);
}

- (TimelineRenderData)timelineRenderData {
    TimelineRenderData data;
    data.color_management = self.state->document.color_management;
    data.display_sdr_preview = true;
    data.sequence_width = self.state->document.sequence.width;
    data.sequence_height = self.state->document.sequence.height;
    const double width = self.metalView.bounds.size.width;
    const double timelineHeight = [self timelineHeight];
    data.video_height = [self videoHeight];
    const double top = data.video_height;
    auto add = [&](double x, double y, double w, double h, float r, float g,
                   float b, float a = 1.0f) {
        if (w > 0.0 && h > 0.0)
            data.overlays.emplace_back(MetalRect{x, y, w, h, r, g, b, a});
    };
    const auto addColor = [&](double x, double y, double w, double h,
                              ui::theme::Color color) {
        add(x, y, w, h, color.r, color.g, color.b, color.a);
    };
    const auto addV = [&](double x, double y, double w, double h,
                          ui::theme::Color topColor,
                          ui::theme::Color bottomColor) {
        if (w > 0.0 && h > 0.0)
            data.overlays.emplace_back(
                MetalRect{x, y, w, h, topColor, bottomColor});
    };
    const auto addText =
        [&](double x, double y, double maxWidth, double pointSize,
            const std::string& text, ui::theme::Color color,
            MetalFontFace face = MetalFontFace::Mono, bool bold = false) {
            if (!text.empty() && maxWidth > 0.0)
                data.overlays.emplace_back(MetalText{x, y, maxWidth, pointSize,
                                                     text, color, face, bold});
        };
    const auto addIcon = [&](double x, double y, double size,
                             const std::string& name, ui::theme::Color color) {
        if (size > 0.0 && !name.empty())
            data.overlays.emplace_back(
                MetalIcon{x, y, size, size, name, color});
    };

    const auto topmost = std::find_if(
        self.state->requested.rbegin(), self.state->requested.rend(),
        [](const ResolvedSlot& slot) { return slot.active; });
    if (topmost != self.state->requested.rend() &&
        self.state->offlineSourceIds.count(topmost->sourceId) != 0)
        add(0.0, 0.0, width, data.video_height, 0.025f, 0.025f, 0.025f);

    addColor(0.0, top, width, timelineHeight, ui::theme::kSurfaceBase);
    addV(0.0, top, width, kTimelineToolbarHeight, ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(0.0, top + kTimelineToolbarHeight, width, kTimelineScaleHeight,
             ui::theme::kSurfaceRaised);
    addColor(0.0, top + kTimelineToolbarHeight + kTimelineScaleHeight, width,
             kTimelineRenderBandHeight, ui::theme::kSurfaceSunken);
    addColor(0.0, top + kTimelineRulerHeight - 1.0, width, 1.0,
             ui::theme::kSeparator);

    constexpr double toolStart = 4.0;
    constexpr double toolWidth = 24.0;
    const std::array<const char*, 5> toolIcons{{
        "mouse-pointer-2",
        "hand",
        "search",
        "scissors",
        "unfold-horizontal",
    }};
    for (int index = 0; index < 5; ++index) {
        const bool active = static_cast<int>([self effectiveTool]) == index;
        const bool hovered = self.state->hoveredTimelineTool == index;
        const double x = toolStart + index * toolWidth;
        addV(x, top + 2.0, toolWidth - 2.0, 22.0,
             active || hovered ? ui::theme::kSurfaceControlHi
                               : ui::theme::kSurfaceControl,
             active ? ui::theme::kSurfaceControl : ui::theme::kSurfaceRaised);
        addColor(x, top + 2.0, toolWidth - 2.0, 1.0, ui::theme::kEdgeLight);
        if (active)
            addColor(x, top + 22.0, toolWidth - 2.0, 2.0, ui::theme::kAccent);
        if (hovered)
            addColor(x, top + 2.0, toolWidth - 2.0, 2.0,
                     ui::theme::kTextPrimary);
        addIcon(x + 5.0, top + 6.0, 12.0, toolIcons[index],
                active ? ui::theme::kTextPrimary : ui::theme::kTextTertiary);
    }
    const bool snapping = self.state->interaction->SnappingEnabled();
    addV(128.0, top + 2.0, 38.0, 22.0,
         snapping ? ui::theme::kSurfaceControlHi : ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(128.0, top + 2.0, 38.0, 1.0, ui::theme::kEdgeLight);
    if (snapping) addColor(128.0, top + 22.0, 38.0, 2.0, ui::theme::kAccent);
    addIcon(134.0, top + 7.0, 11.0, "magnet",
            snapping ? ui::theme::kAccent : ui::theme::kTextTertiary);
    addText(148.0, top + 6.0, 20.0, ui::theme::kFontSizeCaption, "8",
            ui::theme::kTextSecondary, MetalFontFace::Mono, true);
    addV(170.0, top + 2.0, 44.0, 22.0,
         self.state->linkedSelection ? ui::theme::kSurfaceControlHi
                                     : ui::theme::kSurfaceControl,
         ui::theme::kSurfaceRaised);
    addColor(170.0, top + 2.0, 44.0, 1.0, ui::theme::kEdgeLight);
    if (self.state->linkedSelection)
        addColor(170.0, top + 22.0, 44.0, 2.0, ui::theme::kAccent);
    const ui::theme::Color linkColor = self.state->linkedSelection
                                           ? ui::theme::kTextPrimary
                                           : ui::theme::kTextTertiary;
    addIcon(174.0, top + 7.0, 11.0, "link", linkColor);
    addText(188.0, top + 6.0, 28.0, ui::theme::kFontSizeCaption, "A/V",
            linkColor, MetalFontFace::Mono, true);
    const std::array<const char*, 3> transportLabels{{"J", "K", "L"}};
    for (int index = 0; index < 3; ++index) {
        const bool active = index == 0   ? self.state->playbackDirection < 0
                            : index == 1 ? self.state->playbackDirection == 0
                                         : self.state->playbackDirection > 0;
        const double x = 222.0 + index * 24.0;
        addV(x, top + 2.0, 22.0, 22.0,
             active ? ui::theme::kSurfaceControlHi : ui::theme::kSurfaceControl,
             ui::theme::kSurfaceRaised);
        addColor(x, top + 2.0, 22.0, 1.0, ui::theme::kEdgeLight);
        if (active) addColor(x, top + 22.0, 22.0, 2.0, ui::theme::kAccent);
        addText(x + 7.0, top + 6.0, 8.0, ui::theme::kFontSizeCaption,
                transportLabels[index],
                active ? ui::theme::kTextPrimary : ui::theme::kTextSecondary,
                MetalFontFace::Mono, true);
    }
    addText(
        std::max(300.0, width - 102.0), top + 6.0, 96.0,
        ui::theme::kFontSizeCaption,
        FormatTimecode(self.state->requestedPosition, [self playheadFrameRate]),
        ui::theme::kTextSecondary, MetalFontFace::Mono, true);
    if ([self hasValidTimelineRange]) {
        const double rawIn =
            self.state->viewport.TimeToX(*self.state->timelineIn);
        const double rawOut =
            self.state->viewport.TimeToX(*self.state->timelineOut);
        const double left = std::max(self.state->viewport.header_width, rawIn);
        const double right = std::min(width, rawOut);
        if (right > left) {
            add(left, top + kTimelineToolbarHeight, right - left,
                kTimelineScaleHeight + kTimelineRenderBandHeight,
                ui::theme::kAccent.r, ui::theme::kAccent.g,
                ui::theme::kAccent.b, 0.18f);
        }
        if (rawIn >= self.state->viewport.header_width && rawIn <= width)
            addColor(rawIn, top + kTimelineToolbarHeight, 1.0,
                     timelineHeight - kTimelineToolbarHeight,
                     ui::theme::kMarkIn);
        if (rawOut >= self.state->viewport.header_width && rawOut <= width)
            addColor(rawOut - 1.0, top + kTimelineToolbarHeight, 1.0,
                     timelineHeight - kTimelineToolbarHeight,
                     ui::theme::kMarkOut);
    } else {
        if (self.state->timelineIn) {
            const double x =
                self.state->viewport.TimeToX(*self.state->timelineIn);
            if (x >= self.state->viewport.header_width && x <= width)
                addColor(x, top + kTimelineToolbarHeight, 1.0,
                         timelineHeight - kTimelineToolbarHeight,
                         ui::theme::kMarkIn);
        }
        if (self.state->timelineOut) {
            const double x =
                self.state->viewport.TimeToX(*self.state->timelineOut);
            if (x >= self.state->viewport.header_width && x <= width)
                addColor(x - 1.0, top + kTimelineToolbarHeight, 1.0,
                         timelineHeight - kTimelineToolbarHeight,
                         ui::theme::kMarkOut);
        }
    }
    const auto tracks = TimelineTracksInDisplayOrder(self.state->document);
    bool sawVideoTrack = false;
    bool drewAudioDivider = false;
    for (size_t index = 0; index < tracks.size(); ++index) {
        const double y = top + kTimelineRulerHeight +
                         index * self.state->viewport.track_height;
        if (y >= top + timelineHeight) break;
        const bool video = tracks[index]->kind == "video";
        const bool caption = tracks[index]->kind == "caption";
        if (video) sawVideoTrack = true;
        if (!video && sawVideoTrack && !drewAudioDivider) {
            addColor(0.0, y - 2.0, width, 2.0, ui::theme::kBorderStrong);
            drewAudioDivider = true;
        }
        addColor(0.0, y, width, self.state->viewport.track_height,
                 ui::theme::kSurfaceLane);
        addV(0.0, y, self.state->viewport.header_width,
             self.state->viewport.track_height, ui::theme::kSurfaceRaised,
             ui::theme::kSurfacePanel);
        addColor(0.0, y, 4.0, self.state->viewport.track_height,
                 ui::theme::TrackTint(video));
        addText(8.0, y + 4.0, 28.0, ui::theme::kFontSizeSmall,
                TrackDisplayName(self.state->document, tracks[index]->id),
                ui::theme::kTextPrimary, MetalFontFace::Mono, true);
        addText(8.0, y + 20.0, 30.0, ui::theme::kFontSizeCaption,
                video     ? "VID."
                : caption ? "SUB."
                          : "AUD.",
                ui::theme::kTextTertiary, MetalFontFace::Mono, true);

        const auto addPill = [&](double x, const std::string& label,
                                 bool active, int control,
                                 bool disabled = false) {
            const bool hovered =
                self.state->hoveredTrackId == tracks[index]->id &&
                self.state->hoveredTrackControl == control;
            addV(x, y + 15.0, 16.0, 14.0,
                 disabled            ? ui::theme::kSurfacePanel
                 : active || hovered ? ui::theme::kSurfaceControlHi
                                     : ui::theme::kSurfaceControl,
                 ui::theme::kSurfaceRaised);
            if (!disabled)
                addColor(x, y + 15.0, 16.0, 1.0, ui::theme::kEdgeLight);
            if (active) addColor(x, y + 27.0, 16.0, 2.0, ui::theme::kAccent);
            if (hovered && !disabled)
                addColor(x, y + 15.0, 16.0, 2.0, ui::theme::kTextPrimary);
            addText(x + 5.0, y + 16.0, 8.0, ui::theme::kFontSizeCaption, label,
                    disabled ? ui::theme::kTextTertiary
                    : active ? ui::theme::kTextPrimary
                             : ui::theme::kTextSecondary,
                    MetalFontFace::Mono, true);
        };
        const auto addIconPill = [&](double x, const std::string& icon,
                                     bool active, int control) {
            const bool hovered =
                self.state->hoveredTrackId == tracks[index]->id &&
                self.state->hoveredTrackControl == control;
            addV(x, y + 15.0, 16.0, 14.0,
                 active || hovered ? ui::theme::kSurfaceControlHi
                                   : ui::theme::kSurfaceControl,
                 ui::theme::kSurfaceRaised);
            addColor(x, y + 15.0, 16.0, 1.0, ui::theme::kEdgeLight);
            if (active) addColor(x, y + 27.0, 16.0, 2.0, ui::theme::kAccent);
            if (hovered)
                addColor(x, y + 15.0, 16.0, 2.0, ui::theme::kTextPrimary);
            addIcon(
                x + 3.0, y + 17.0, 10.0, icon,
                active ? ui::theme::kTextPrimary : ui::theme::kTextTertiary);
        };
        if (video || caption) {
            addIconPill(40.0, "eye", tracks[index]->visible, 0);
            addIconPill(58.0, "lock", tracks[index]->locked, 1);
        } else {
            addPill(40.0, "S", tracks[index]->solo, 0);
            addPill(58.0, "M", tracks[index]->muted, 1);
            addIconPill(76.0, "lock", tracks[index]->locked, 2);
        }
        addColor(0.0, y + self.state->viewport.track_height - 1.0, width, 1.0,
                 ui::theme::kSeparator);
    }
    // Out of lot -- the author, 29 October 2026. No `+` row under the headers any
    // more: a drop creates the tracks it needs (TimelineDropTracksAt), and
    // the Timeline menu, the timeline's own menu and Cmd+Shift+T still add
    // one.
    addColor(self.state->viewport.header_width - 1.0,
             top + kTimelineToolbarHeight, 1.0,
             timelineHeight - kTimelineToolbarHeight, ui::theme::kBorderStrong);
    // Graduations far enough apart for their timecodes: the widest label is
    // the one at the timeline's end, measured in the face the atlas draws
    // it with (monospaced, bold, caption size), plus its 4-point inset and
    // an 8-point gap before the next one.
    const double rulerLabelWidth = [self rulerLabelWidth];
    const std::vector<double> tickXs = self.state->viewport.TickXs(
        width, std::max(kTimelineMinTickSpacing, rulerLabelWidth + 12.0));
    for (size_t tickIndex = 0; tickIndex < tickXs.size(); ++tickIndex) {
        const double tickX = tickXs[tickIndex];
        const double rulerTop = top + kTimelineToolbarHeight;
        addColor(tickX, rulerTop + kTimelineScaleHeight - 7.0, 1.0, 7.0,
                 ui::theme::kTextTertiary);
        add(tickX, top + kTimelineRulerHeight, 1.0,
            std::max(0.0, timelineHeight - kTimelineRulerHeight),
            ui::theme::kLaneGrid.r, ui::theme::kLaneGrid.g,
            ui::theme::kLaneGrid.b, 0.58f);
        const RationalTime tickTime = self.state->viewport.XToTime(
            tickX, self.state->document.sequence.frame_rate.num);
        addText(tickX + 4.0, rulerTop + 4.0, rulerLabelWidth,
                ui::theme::kFontSizeCaption,
                FormatTimecode(tickTime, [self playheadFrameRate]),
                ui::theme::kTextTertiary, MetalFontFace::Mono, true);
        if (tickIndex + 1 < tickXs.size()) {
            const double interval = tickXs[tickIndex + 1] - tickX;
            for (int subdivision = 1; subdivision < 4; ++subdivision) {
                addColor(tickX + interval * subdivision / 4.0,
                         rulerTop + kTimelineScaleHeight - 4.0, 1.0, 4.0,
                         ui::theme::kBorderStrong);
            }
        }
    }

    if (const auto& gap = self.state->interaction->SelectedGap()) {
        const auto track = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* value) {
                                            return value->id == gap->track_id;
                                        });
        if (track != tracks.end()) {
            const double rawLeft = self.state->viewport.TimeToX(gap->start);
            const double rawRight =
                self.state->viewport.TimeToX(gap->start.add(gap->duration));
            const double left = std::max(self.state->viewport.header_width,
                                         std::min(rawLeft, rawRight));
            const double right = std::min(width, std::max(rawLeft, rawRight));
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), track) *
                                 self.state->viewport.track_height;
            if (right > left && y < top + timelineHeight) {
                const double height = self.state->viewport.track_height;
                addColor(left, y, right - left, height,
                         ui::theme::WithAlpha(ui::theme::kAccent, 0.18f));
                addColor(left, y, right - left, 2.0, ui::theme::kAccent);
                addColor(left, y + height - 2.0, right - left, 2.0,
                         ui::theme::kAccent);
                addColor(left, y, 2.0, height, ui::theme::kAccent);
                addColor(right - 2.0, y, 2.0, height, ui::theme::kAccent);
                for (double stripe = left + 8.0; stripe < right; stripe += 12.0)
                    addColor(stripe, y + 5.0, 1.0, std::max(0.0, height - 10.0),
                             ui::theme::WithAlpha(ui::theme::kAccentHi, 0.35f));
            }
        }
    }

    // FC4 -- while J or L runs, the clips as the dynamic trim has them so
    // far; nothing is in the document before K.
    const Document& shownDocument = [self trimShownDocument];
    const auto clips =
        VisibleTimelineClips(shownDocument, self.state->viewport, width,
                             self.state->interaction->SelectedClipIds(),
                             self.state->interaction->TrimPreview(),
                             self.state->interaction->MovePreview());
    for (const TimelineClipRect& clip : clips) {
        double left = std::min(clip.x, clip.x + clip.width);
        double right = std::max(clip.x, clip.x + clip.width);
        const bool headVisible =
            left >= self.state->viewport.header_width && left <= width;
        left = std::max(left, self.state->viewport.header_width);
        right = std::min(right, width);
        if (right <= left) continue;
        const double y = top + clip.y;
        if (y >= top + timelineHeight) continue;
        // REM5 -- a motion clip reads as one: its own colour, not a shot's
        // (the timeline draws no picture of any clip; M4's usage gap).
        const DocumentClip* drawnClip =
            self.state->document.FindClip(clip.clip_id);
        // Out of lot -- the author, 29 October 2026: a disabled clip is greyed,
        // its waveform and its name with it (ClipEnabled.h, UiTheme.h).
        const bool disabledClip = drawnClip && !drawnClip->enabled;
        const auto color =
            disabledClip ? std::array<float, 3>{ui::theme::kClipDisabled.r,
                                                ui::theme::kClipDisabled.g,
                                                ui::theme::kClipDisabled.b}
            : drawnClip && IsMotionClip(*drawnClip)
                ? std::array<float, 3>{ui::theme::kTrackMotion.r,
                                       ui::theme::kTrackMotion.g,
                                       ui::theme::kTrackMotion.b}
                : ClipColor(clip.source_id, clip.audio);
        if (!clip.valid)
            addColor(left, y + 2.0, right - left, 40.0,
                     ui::theme::WithAlpha(ui::theme::kError, 0.92f));
        else {
            const float previewLift = clip.preview ? 0.08f : 0.0f;
            // The moving rectangle is the exact overwrite candidate, not a
            // decorative ghost. Keep it opaque so the incoming result remains
            // readable over the clip it is about to replace.
            const float movingAlpha = 1.0f;
            const ui::theme::Color clipTop{
                std::min(1.0f, color[0] + 0.08f + previewLift),
                std::min(1.0f, color[1] + 0.08f + previewLift),
                std::min(1.0f, color[2] + 0.08f + previewLift), movingAlpha};
            const ui::theme::Color clipBottom{
                std::max(0.0f, color[0] - 0.05f + previewLift),
                std::max(0.0f, color[1] - 0.05f + previewLift),
                std::max(0.0f, color[2] - 0.05f + previewLift), movingAlpha};
            addV(left, y + 2.0, right - left, 40.0, clipTop, clipBottom);
        }
        addColor(left, y + 2.0, right - left, 1.0, ui::theme::kEdgeLight);
        if (headVisible)
            addColor(left, y + 3.0, 3.0, 38.0,
                     ui::theme::WithAlpha(ui::theme::kTextClip, 0.32f));
        if (clip.audio && clip.width > 0.0) {
            // The source window comes with the rectangle (TimelineView.h):
            // the trim preview's while a trim is held, speed included.
            const auto waveform = self.state->waveforms.find(clip.source_id);
            if (waveform != self.state->waveforms.end() &&
                clip.source_in.rate > 0 && clip.source_span.rate > 0 &&
                clip.source_span.value > 0) {
                const AudioWaveform& data = waveform->second;
                const long double sourceStart =
                    static_cast<long double>(clip.source_in.value) /
                    clip.source_in.rate;
                const long double sourceDuration =
                    static_cast<long double>(clip.source_span.value) /
                    clip.source_span.rate;
                const double center = y + clip.height * 0.53;
                const double availableHeight = std::max(2.0, clip.height - 9.0);
                for (double x = std::floor(left) + 1.0; x < right; x += 2.0) {
                    const long double firstFraction = std::clamp(
                        static_cast<long double>((x - clip.x) / clip.width),
                        0.0L, 1.0L);
                    const long double lastFraction = std::clamp(
                        static_cast<long double>(
                            (std::min(right, x + 2.0) - clip.x) / clip.width),
                        0.0L, 1.0L);
                    size_t first = static_cast<size_t>(std::max<long double>(
                        0.0L, (sourceStart + sourceDuration * firstFraction) *
                                  data.peaks_per_second));
                    size_t last = static_cast<size_t>(std::max<long double>(
                        0.0L, (sourceStart + sourceDuration * lastFraction) *
                                  data.peaks_per_second));
                    first = std::min(first, data.peaks.size());
                    last =
                        std::min(std::max(last, first + 1), data.peaks.size());
                    if (first >= last) continue;
                    float peak = 0.0f;
                    for (size_t index = first; index < last; ++index)
                        peak = std::max(peak, data.peaks[index]);
                    const double barHeight = std::max(
                        1.0, static_cast<double>(peak) * availableHeight);
                    if (disabledClip)
                        addColor(x, center - barHeight * 0.5,
                                 std::min(1.25, right - x), barHeight,
                                 ui::theme::kClipDisabledWaveform);
                    else
                        add(x, center - barHeight * 0.5,
                            std::min(1.25, right - x), barHeight, 0.64f, 0.94f,
                            0.79f, 0.82f);
                }
            }
        }
        const double clipSpan = right - left;
        add(left, y + ui::theme::kTimelineClipNameBandTop, clipSpan, 14.0, 0.0f,
            0.0f, 0.0f, 0.40f);
        if (clipSpan >= ui::theme::kTimelineClipNameMinWidth) {
            const DocumentClip* documentClip =
                self.state->document.FindClip(clip.clip_id);
            const std::string name =
                TimelineDrawnClipName(self.state->document, self.state->project,
                                      documentClip, clip.source_id);
            addText(left + 5.0, y + (ui::theme::kTimelineClipNameBandTop + 1.0),
                    clipSpan - 10.0, ui::theme::kFontSizeSmall, name,
                    disabledClip ? ui::theme::kClipDisabledText
                                 : ui::theme::kTextClip,
                    MetalFontFace::Mono, true);
        }
        add(left, y + 40.0, clipSpan, 2.0, 0.0f, 0.0f, 0.0f, 0.28f);
        // PRD24 -- how far the sync badge reaches along the badge row, from
        // the row's own inset: what the clip carries is written after it.
        double syncBadgeReach = 0.0;
        if (clip.audio && clip.sync_drift && clip.sync_drift->value != 0) {
            const std::string label =
                SyncDriftLabel(*clip.sync_drift, [self playheadFrameRate]);
            // PRD25 -- one plate of the badge row (ClipBadges.h): its label
            // used to sit four points too low, over the waveform.
            const PlacedClipBadge plate =
                PlaceClipPlate(label, ui::theme::kTimelineClipBadgeInset);
            if (clipSpan >=
                plate.x + plate.width + ui::theme::kTimelineClipBadgeEndInset) {
                add(left + plate.x, y + plate.y, plate.width, plate.height,
                    0.66f, 0.12f, 0.07f, 0.94f);
                addText(left + plate.text_x, y + plate.text_y, plate.text_width,
                        ui::theme::kFontSizeCaption, label,
                        ui::theme::kTextClip, MetalFontFace::Mono, true);
                syncBadgeReach = plate.width;
            } else {
                add(left + 3.0, y + 7.0, 3.0, std::min(11.0, clip.height - 9.0),
                    0.94f, 0.18f, 0.08f);
                syncBadgeReach = 3.0 + 3.0 - ui::theme::kTimelineClipBadgeInset;
            }
        }
        // PRD24 -- ROADMAP.md. What the clip carries, said on its rectangle:
        // ClipBadges.h decides which badges, in which order and where; this
        // only draws them. A clip that carries nothing gets no command here,
        // so its display list is the one it had before this ticket.
        bool signageShown = drawnClip != nullptr;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        // The smoke run builds the list once more without this pass, to hold
        // that last sentence to the commands themselves.
        signageShown = signageShown && !gUiSmokeClipSignageOff;
#endif
        if (signageShown) {
            // PRD25 -- the agent's mark, while its last call's clips are
            // marked (AgentJournal.h).
            const ClipSignage signage = ClipSignageFor(
                self.state->document, *drawnClip,
                self.state->agentJournal.MarkedClips().count(clip.clip_id) !=
                    0);
            for (const PlacedClipBadge& badge :
                 PlaceClipBadges(signage.badges, clipSpan, syncBadgeReach)) {
                addColor(left + badge.x, y + badge.y, badge.width, badge.height,
                         ClipBadgeFillColor(badge.kind));
                addText(left + badge.text_x, y + badge.text_y, badge.text_width,
                        ui::theme::kFontSizeCaption, badge.label,
                        ClipBadgeTextColor(badge.kind), MetalFontFace::Mono,
                        true);
            }
            // F1.5 -- an angle its multicam group keeps off screen: the
            // whole clip under a veil, badges and name included, down to
            // the descenders the name hangs under the body. Its edges and
            // its selection are drawn after, over the veil.
            if (signage.hidden_angle)
                addColor(left, y + ui::theme::kTimelineClipBodyTop, clipSpan,
                         ui::theme::kTimelineClipBodyHeight +
                             ui::theme::kTimelineClipNameOverhang,
                         ui::theme::kClipHiddenAngleVeil);
        }
        // Every edit boundary remains readable when adjacent clips share the
        // same source. Selection remains the only clip-level emphasis.
        const double outlineWidth = std::min(1.0, right - left);
        addColor(left, y + 2.0, outlineWidth, 40.0, ui::theme::kSeparator);
        addColor(right - outlineWidth, y + 2.0, outlineWidth, 40.0,
                 ui::theme::kSeparator);
        if (clip.selected) {
            const ui::theme::Color selection =
                clip.valid ? ui::theme::kTextPrimary : ui::theme::kError;
            addColor(left, y + 2.0, clipSpan, 2.0, selection);
            addColor(left, y + 40.0, clipSpan, 2.0, selection);
            addColor(left, y + 2.0, std::min(2.0, clipSpan), 40.0, selection);
            addColor(std::max(left, right - 2.0), y + 2.0,
                     std::min(2.0, clipSpan), 40.0, selection);
        }
        if (clip.selected && self.window.firstResponder == self.metalView &&
            clipSpan > 8.0) {
            addColor(left + 4.0, y + 6.0, clipSpan - 8.0, 1.0,
                     ui::theme::kAccent);
            addColor(left + 4.0, y + 37.0, clipSpan - 8.0, 1.0,
                     ui::theme::kAccent);
            addColor(left + 4.0, y + 6.0, 1.0, 32.0, ui::theme::kAccent);
            addColor(right - 5.0, y + 6.0, 1.0, 32.0, ui::theme::kAccent);
        }
        if (clip.moving) {
            addColor(left, y + 2.0, clipSpan, 2.0, ui::theme::kAccent);
            addColor(left, y + 40.0, clipSpan, 2.0, ui::theme::kAccent);
            addColor(left, y + 2.0, std::min(2.0, clipSpan), 40.0,
                     ui::theme::kAccent);
            addColor(std::max(left, right - 2.0), y + 2.0,
                     std::min(2.0, clipSpan), 40.0, ui::theme::kAccent);
        }
    }

    // FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. The Retime controls shown:
    // a band with the clip's speed, and its two ends, where a press takes
    // them (VisibleTimelineRetimeBands).
    for (const TimelineRetimeBand& band :
         VisibleTimelineRetimeBands(shownDocument, self.state->viewport, width,
                                    self.state->retimeClipIds)) {
        const double y = top + band.y;
        if (y >= top + timelineHeight) continue;
        addColor(band.x, y, band.width, band.height, ui::theme::kRetimeBand);
        addColor(band.head_x, y, band.head_width, band.height,
                 ui::theme::kRetimeHandle);
        addColor(band.tail_x, y, band.tail_width, band.height,
                 ui::theme::kRetimeHandle);
        addText(
            band.x + kTimelineEdgeHitWidth + ui::theme::kSpaceXs, y + 1.0,
            band.width - (2.0 * (kTimelineEdgeHitWidth + ui::theme::kSpaceXs)),
            ui::theme::kFontSizeCaption, band.label, ui::theme::kTextClip,
            MetalFontFace::Mono, true);
    }

    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The edit points the trim
    // mode holds, on the zone a press finds them in (VisibleTimelineEdgeMarks):
    // green with media left beyond them, red at its end.
    if (self.state->trimSession.Active()) {
        for (const TimelineEdgeMarkRect& mark : VisibleTimelineEdgeMarks(
                 shownDocument, self.state->viewport, width,
                 trim::TrimSessionEdgeMarks(shownDocument,
                                            self.state->trimSession))) {
            const double y = top + mark.y;
            if (y >= top + timelineHeight) continue;
            addColor(mark.x, y + ui::theme::kTimelineClipBodyTop, mark.width,
                     ui::theme::kTimelineClipBodyHeight,
                     mark.at_limit ? ui::theme::kTrimPointAtLimit
                                   : ui::theme::kTrimPoint);
        }
    }

    // Locked lanes stay visible but receive a sparse diagonal safety hatch
    // above their clips. Axis-aligned stair steps keep this compatible with
    // the timeline's rectangle-only Metal display list.
    for (size_t index = 0; index < tracks.size(); ++index) {
        if (!tracks[index]->locked) continue;
        const double y = top + kTimelineRulerHeight +
                         index * self.state->viewport.track_height;
        const double height = self.state->viewport.track_height;
        if (y >= top + timelineHeight) continue;
        add(self.state->viewport.header_width, y,
            width - self.state->viewport.header_width, height, 0.0f, 0.0f, 0.0f,
            0.26f);
        for (double baseX = self.state->viewport.header_width - height;
             baseX < width; baseX += 16.0) {
            for (double offset = 0.0; offset < height; offset += 3.0) {
                const double x = baseX + offset;
                if (x >= self.state->viewport.header_width && x < width)
                    add(x, y + offset, std::min(4.0, width - x), 1.5,
                        ui::theme::kTextTertiary.r, ui::theme::kTextTertiary.g,
                        ui::theme::kTextTertiary.b, 0.42f);
            }
        }
    }

    // The range belongs to the timeline, not to its empty background. Draw
    // its translucent body after clips so In/Out remains readable across
    // picture, waveform and gaps alike.
    if ([self hasValidTimelineRange]) {
        const double rawIn =
            self.state->viewport.TimeToX(*self.state->timelineIn);
        const double rawOut =
            self.state->viewport.TimeToX(*self.state->timelineOut);
        const double left = std::max(self.state->viewport.header_width, rawIn);
        const double right = std::min(width, rawOut);
        if (right > left)
            add(left, top + kTimelineRulerHeight, right - left,
                std::max(0.0, timelineHeight - kTimelineRulerHeight -
                                  kTimelineZoomBarHeight),
                ui::theme::kAccent.r, ui::theme::kAccent.g,
                ui::theme::kAccent.b, 0.10f);
        if (rawIn >= self.state->viewport.header_width && rawIn <= width)
            addColor(
                rawIn, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kMarkIn);
        if (rawOut >= self.state->viewport.header_width && rawOut <= width)
            addColor(
                rawOut - 1.0, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kMarkOut);
    }

    for (const DocumentTransition& transition :
         self.state->document.sequence.transitions) {
        const DocumentTrack* track =
            self.state->document.FindTrack(transition.track_id);
        if (!track) continue;
        const auto displayTrack =
            std::find(tracks.begin(), tracks.end(), track);
        if (displayTrack == tracks.end()) continue;
        const MediaRate rate = self.state->document.sequence.frame_rate;
        // R5 -- docs/roadmap/REEL-2026-09.md. Where the transition falls,
        // by the one function the monitor, the export and Validate ask.
        // This block used to work the split out for itself and to read the
        // right-hand clip to find the cut -- which drew nothing at all for
        // a colour dip that closes a track, since that one has no
        // right-hand clip.
        TransitionSpan span;
        std::string spanError;
        if (!ResolveTransitionSpan(self.state->document.sequence, transition,
                                   span, spanError))
            continue;
        const RationalTime cutTime{span.cut_frame * rate.den, rate.num};
        const RationalTime pre{span.pre_frames * rate.den, rate.num};
        const RationalTime post{span.post_frames * rate.den, rate.num};
        double leftX = self.state->viewport.TimeToX(cutTime.sub(pre));
        double rightX = self.state->viewport.TimeToX(cutTime.add(post));
        leftX = std::max(leftX, self.state->viewport.header_width);
        rightX = std::min(rightX, width);
        if (rightX <= leftX) continue;
        const double y = top + kTimelineRulerHeight +
                         std::distance(tracks.begin(), displayTrack) *
                             self.state->viewport.track_height +
                         4.0;
        const double height =
            std::max(8.0, self.state->viewport.track_height - 8.0);
        addColor(leftX, y, rightX - leftX, height,
                 ui::theme::WithAlpha(ui::theme::kSurfaceControlHi, 0.72f));
        addColor(leftX, y, rightX - leftX, 2.0, ui::theme::kAccent);
        addColor(leftX, y + height - 2.0, rightX - leftX, 2.0,
                 ui::theme::kAccent);
        const double cutX = self.state->viewport.TimeToX(cutTime);
        addColor(cutX - 1.0, y, 2.0, height, ui::theme::kTextPrimary);
    }

    // EF1 -- docs/roadmap/EFFETS-2026-10.md. The cut a dragged effect would
    // land on: a band of the accent either side of it, on its track.
    // EF3 -- a clip effect lands on a whole clip: the band covers it, edged
    // with the accent above and below.
    if (self.state->effectDropTarget && self.state->effectDropTarget->valid &&
        !self.state->effectDropTarget->clip_id.empty()) {
        const ui::effects::DropTarget& target = *self.state->effectDropTarget;
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* track) {
                                            return track->id == target.track_id;
                                        });
        const DocumentClip* clip =
            self.state->document.FindClip(target.clip_id);
        if (found != tracks.end() && clip != nullptr) {
            const double left =
                std::max(self.state->viewport.header_width,
                         self.state->viewport.TimeToX(clip->timeline_in));
            const double right =
                std::min(width, self.state->viewport.TimeToX(
                                    clip->timeline_in.add(clip->duration)));
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), found) *
                                 self.state->viewport.track_height +
                             2.0;
            const double height =
                std::max(8.0, self.state->viewport.track_height - 4.0);
            if (right > left) {
                addColor(left, y, right - left, height,
                         ui::theme::WithAlpha(ui::theme::kAccent, 0.35f));
                addColor(left, y, right - left, 2.0, ui::theme::kAccent);
                addColor(left, y + height - 2.0, right - left, 2.0,
                         ui::theme::kAccent);
            }
        }
    } else if (self.state->effectDropTarget &&
               self.state->effectDropTarget->valid) {
        const ui::effects::DropTarget& target = *self.state->effectDropTarget;
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [&](const DocumentTrack* track) {
                                            return track->id == target.track_id;
                                        });
        const double cutX = self.state->viewport.TimeToX(target.cut);
        if (found != tracks.end() &&
            cutX >= self.state->viewport.header_width && cutX <= width) {
            const double y = top + kTimelineRulerHeight +
                             std::distance(tracks.begin(), found) *
                                 self.state->viewport.track_height +
                             2.0;
            const double height =
                std::max(8.0, self.state->viewport.track_height - 4.0);
            constexpr double kBand = 12.0;
            const double left =
                target.left_clip_id.empty() ? cutX : cutX - kBand;
            const double right =
                target.right_clip_id.empty() ? cutX : cutX + kBand;
            addColor(left, y, right - left, height,
                     ui::theme::WithAlpha(ui::theme::kAccent, 0.35f));
            addColor(cutX - 1.0, y, 2.0, height, ui::theme::kAccent);
        }
    }

    if (self.state->lassoDragging) {
        const double left = std::max(
            self.state->viewport.header_width,
            std::min(self.state->lassoStartX, self.state->lassoCurrentX));
        const double right = std::min(
            width,
            std::max(self.state->lassoStartX, self.state->lassoCurrentX));
        const double lassoTop = std::max(
            kTimelineRulerHeight,
            std::min(self.state->lassoStartY, self.state->lassoCurrentY));
        const double bottom =
            std::max(self.state->lassoStartY, self.state->lassoCurrentY);
        if (right > left && bottom > lassoTop) {
            addColor(left, top + lassoTop, right - left, bottom - lassoTop,
                     ui::theme::WithAlpha(ui::theme::kAccent, 0.12f));
            addColor(left, top + lassoTop, right - left, 1.0,
                     ui::theme::kAccent);
            addColor(left, top + bottom - 1.0, right - left, 1.0,
                     ui::theme::kAccent);
            addColor(left, top + lassoTop, 1.0, bottom - lassoTop,
                     ui::theme::kAccent);
            addColor(right - 1.0, top + lassoTop, 1.0, bottom - lassoTop,
                     ui::theme::kAccent);
        }
    }

    if (self.state->interaction->SnapGuideTime()) {
        const double snapX = self.state->viewport.TimeToX(
            *self.state->interaction->SnapGuideTime());
        if (snapX >= self.state->viewport.header_width && snapX <= width)
            addColor(
                snapX, top + kTimelineRulerHeight, 1.0,
                timelineHeight - kTimelineRulerHeight - kTimelineZoomBarHeight,
                ui::theme::kAccent);
    }
    // Out of lot -- the author, 29 October 2026: the trim's frame counter, as in
    // Resolve, on a plate just above the trimmed clip, beside its moved edge
    // (inside the clip when the clip is on the top track).
    if (const auto readout = TimelineTrimReadoutFor(
            self.state->document, self.state->interaction->TrimPreview(),
            [self playheadFrameRate])) {
        const auto rect = std::find_if(
            clips.begin(), clips.end(), [&](const TimelineClipRect& clip) {
                return clip.clip_id == readout->clip_id;
            });
        const double edgeX = self.state->viewport.TimeToX(readout->edge);
        if (rect != clips.end() && edgeX >= self.state->viewport.header_width &&
            edgeX <= width) {
            const double plateWidth =
                10.0 + 7.0 * static_cast<double>(readout->text.size());
            const double plateHeight = 16.0;
            const double plateX =
                std::min(edgeX + 6.0, width - plateWidth - 2.0);
            double plateY = top + rect->y - plateHeight - 2.0;
            if (plateY < top + kTimelineRulerHeight)
                plateY = top + rect->y + 3.0;
            add(plateX, plateY, plateWidth, plateHeight, 0.0f, 0.0f, 0.0f,
                0.82f);
            addText(plateX + 5.0, plateY + 2.0, plateWidth - 10.0,
                    ui::theme::kFontSizeSmall, readout->text,
                    ui::theme::kTextClip, MetalFontFace::Mono, true);
        }
    }
    if (self.state->cutPreviewX && self.state->cutPreviewY) {
        addColor(*self.state->cutPreviewX - 1.0, top + *self.state->cutPreviewY,
                 2.0, self.state->viewport.track_height, ui::theme::kAccent);
    }

    // D2 -- docs/roadmap/DIALOGUE-2026-10.md. Sequence markers. Each flag is
    // the rectangle the pointer hits (VisibleTimelineMarkers, TimelineView.h):
    // only its colour and the note beside it are decided here. The note is
    // shown for the flag under the pointer, else for the marker the paused
    // playhead rests on -- not during playback, where it would flash by.
    const DocumentMarker* playheadMarker =
        self.state->playbackDirection == 0
            ? TimelineMarkerCoveringPlayhead(self.state->document,
                                             self.state->requestedPosition,
                                             [self playheadFrameRate])
            : nullptr;
    // D6 -- a marker being stretched is drawn as the gesture has it.
    std::optional<TimelineMarkerPreview> stretched;
    if (self.state->markerStretch && self.state->markerStretch->reached)
        stretched =
            TimelineMarkerPreview{self.state->markerStretch->marker_id,
                                  self.state->markerStretch->reached->time,
                                  self.state->markerStretch->reached->duration};
    const Ulid notedMarkerId = !self.state->hoveredMarkerId.empty()
                                   ? self.state->hoveredMarkerId
                               : playheadMarker ? playheadMarker->id
                                                : Ulid{};
    for (const TimelineMarkerFlag& flag : VisibleTimelineMarkers(
             self.state->document, self.state->viewport, width, stretched)) {
        const DocumentMarker* marker =
            self.state->document.FindMarker(flag.marker_id);
        if (!marker) continue;
        const ui::theme::Color color = MarkerDisplayColor(*marker);
        // A filled tag on the ruler, its point on the marker's time, and
        // nothing in the tracks: a line down through them read as a second
        // playhead (Visual QA of 1 October 2026). Rows that would cross the
        // track headers are cut at their edge.
        const double headerEdge = self.state->viewport.header_width;
        const auto addClipped = [&](double x, double y, double w, double h,
                                    ui::theme::Color fill) {
            // D6 -- a span may also run past the right edge of the view.
            const double left = std::max(headerEdge, x);
            const double right = std::min(width, x + w);
            addColor(left, y, right - left, h, fill);
        };
        const auto shape = TimelineMarkerFlagShape(flag);
        for (const TimelineMarkerFlagRow& row : shape)
            addClipped(row.x, top + row.y, row.width, row.height, color);
        if (flag.marker_id == self.state->hoveredMarkerId && !shape.empty())
            addClipped(shape.front().x, top + shape.front().y,
                       shape.front().width, 2.0, ui::theme::kTextPrimary);
        if (flag.marker_id != notedMarkerId) continue;
        constexpr double kNoteHeight = 18.0;
        constexpr double kNoteMaxWidth = 360.0;
        const double room = width - ui::theme::kSpaceXs;
        double noteWidth =
            std::min(kNoteMaxWidth, [self markerNoteWidth:marker->name] +
                                        2.0 * ui::theme::kSpaceS);
        // Beside the tag; for a span begun left of the view, beside what
        // shows of it.
        const double tagX = std::max(flag.x, self.state->viewport.header_width);
        double noteX = tagX + ui::theme::kSpaceS;
        // No room on the right of the tag: the note goes to its left.
        if (noteX + noteWidth > room)
            noteX = tagX - ui::theme::kSpaceS - noteWidth;
        noteX = std::max(
            noteX, self.state->viewport.header_width + ui::theme::kSpaceXs);
        noteWidth = std::min(noteWidth, room - noteX);
        const double noteY = top + kTimelineRulerHeight + ui::theme::kSpaceXs;
        addColor(noteX, noteY, noteWidth, kNoteHeight,
                 ui::theme::kSurfaceControlHi);
        addColor(noteX, noteY, 2.0, kNoteHeight, color);
        addText(noteX + ui::theme::kSpaceS, noteY + 2.0,
                noteWidth - 2.0 * ui::theme::kSpaceS + 1.0,
                ui::theme::kFontSizeSmall, marker->name,
                ui::theme::kTextPrimary, MetalFontFace::Sans, false);
    }

    const double zoomY = top + timelineHeight - kTimelineZoomBarHeight;
    addColor(0.0, zoomY, width, kTimelineZoomBarHeight,
             ui::theme::kSurfaceSunken);
    addColor(0.0, zoomY, self.state->viewport.header_width,
             kTimelineZoomBarHeight, ui::theme::kSurfacePanel);
    // While a drag holds the bar, the rail keeps the span the drag began
    // with: the handle stays under the pointer (TimelineView.h).
    const TimelineZoomBarGeometry zoomGeometry =
        CalculateTimelineZoomBarGeometry(
            self.state->viewport, self.state->duration, width,
            self.state->zoomBarDrag
                ? self.state->zoomBarDrag->initial_geometry.ruler_seconds
                : 0.0);
    if (zoomGeometry.thumb_width > 0.0) {
        addV(zoomGeometry.thumb_x, zoomY + 2.0, zoomGeometry.thumb_width, 10.0,
             ui::theme::kSurfaceControlHi, ui::theme::kSurfaceControl);
        addColor(zoomGeometry.thumb_x, zoomY + 2.0, zoomGeometry.handle_width,
                 10.0, ui::theme::kTextTertiary);
        addColor(zoomGeometry.thumb_x + zoomGeometry.thumb_width -
                     zoomGeometry.handle_width,
                 zoomY + 2.0, zoomGeometry.handle_width, 10.0,
                 ui::theme::kTextTertiary);
        if (self.state->zoomBarDrag)
            addColor(zoomGeometry.thumb_x, zoomY, zoomGeometry.thumb_width, 2.0,
                     ui::theme::kAccent);
    }

    const double playheadX =
        self.state->viewport.TimeToX(self.state->requestedPosition);
    if (playheadX >= self.state->viewport.header_width && playheadX <= width) {
        addColor(
            playheadX, top + kTimelineToolbarHeight, 1.0,
            timelineHeight - kTimelineToolbarHeight - kTimelineZoomBarHeight,
            ui::theme::kAccent);
    }
    return data;
}

// LG5 -- docs/roadmap/LEGENDES-2026-10.md. Record shows the cartons as the
// export draws them: the caption renderer's picture of the frame, each
// carton in its own style, its lines broken where the file breaks them,
// drawn at the pixels the frame is shown at -- never more than the
// sequence's own: zoomed past one to one, it is enlarged with the video
// under it. Drawn again only when what it shows changes.
- (void)showProgramCaptions:(const std::vector<CaptionCarton>&)cartons
                       text:(NSString*)text {
    [self showCaptions:cartons
                  text:text
                  view:self.programCaptionView
               monitor:self.programMonitorView
                  zoom:self.state->programMonitorZoom];
}

- (void)showCaptions:(const std::vector<CaptionCarton>&)cartons
                text:(NSString*)text
                view:(CMCaptionView*)view
             monitor:(NSView*)monitor
                zoom:(double)zoom {
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    const DocumentSequence& sequence = self.state->document.sequence;
    const NSRect bounds = monitor.bounds;
    const double layerScale =
        monitor.layer.contentsScale > 0.0 ? monitor.layer.contentsScale : 1.0;
    // Zoomed, the renderer draws a sequence pixel as `zoom` of its layer's
    // pixels: that many over the layer's scale in points.
    const double zoomPoints = zoom > 0.0 ? zoom / layerScale : 0.0;
    const ui::caption_overlay::Layout canvas = ui::caption_overlay::Compute(
        sequence.width, sequence.height, bounds.size.width, bounds.size.height,
        zoomPoints);
    const auto hide = [&] {
        view.hidden = YES;
        view.captionText = @"";
    };
    if (cartons.empty() || renderer == nullptr || canvas.canvas_width <= 0.0 ||
        canvas.canvas_height <= 0.0) {
        hide();
        return;
    }
    // The screen's pixels per point, asked of the view.
    const double pixelScale = std::max(
        1.0, [monitor convertSizeToBacking:NSMakeSize(1.0, 1.0)].width);
    const int32_t width = std::clamp<int32_t>(
        static_cast<int32_t>(std::lround(canvas.canvas_width * pixelScale)), 1,
        sequence.width);
    const int32_t height = std::max<int32_t>(
        1, static_cast<int32_t>(std::lround(static_cast<double>(width) *
                                            sequence.height / sequence.width)));
    std::ostringstream key;
    key << width << 'x' << height << '/' << sequence.width;
    for (const CaptionCarton& carton : cartons) {
        const CaptionStyle& style = carton.style;
        key << '\x1e' << carton.text << '\x1f' << style.font_family << '\x1f'
            << style.font_size << '\x1f' << style.font_weight << '\x1f'
            << style.color << '\x1f' << style.position << '\x1f'
            << style.outline_width << '\x1f' << style.outline_color << '\x1f'
            << style.shadow_offset << '\x1f' << style.shadow_color << '\x1f'
            << style.background_color << '\x1f' << style.background_padding
            << '\x1f' << style.vertical_position.num << '/'
            << style.vertical_position.den << '\x1f' << carton.opacity.num
            << '/'
            << carton.opacity.den
            // PA4 -- a carton is redrawn when its next word starts.
            << '\x1f' << style.word_animation << '\x1f' << style.highlight_color
            << '\x1f' << carton.words_spoken;
    }
    NSString* drawnKey = CMStringFromUtf8(key.str());
    if (![view.drawnKey isEqualToString:drawnKey]) {
        std::vector<CaptionStyle> styles;
        std::vector<std::string> texts;
        for (const CaptionCarton& carton : cartons) {
            styles.push_back(carton.style);
            texts.push_back(carton.text);
        }
        std::string error;
        std::vector<CaptionCarton> delivered = cartons;
        CGImageRef picture = nullptr;
        CGRect ink = CGRectNull;
        if (WrapCaptionTextsForDelivery(styles, sequence, texts, error)) {
            for (size_t index = 0; index < delivered.size(); ++index)
                delivered[index].text = texts[index];
            CaptionDrawing drawing;
            drawing.width = width;
            drawing.height = height;
            drawing.sequence_width = sequence.width;
            renderer->draw(
                drawing, {delivered},
                [&](size_t, CaptionImage& image) {
                    picture = CMCaptionPicture(image);
                    for (const CaptionCartonLayout& layout : image.cartons) {
                        const caption_layout::Placement& placed =
                            layout.placement;
                        CGRect painted = CGRectMake(
                            placed.block.x - placed.outline,
                            placed.block.y - placed.outline,
                            placed.block.width + (2.0 * placed.outline) +
                                placed.shadow,
                            placed.block.height + (2.0 * placed.outline) +
                                placed.shadow);
                        if (placed.box) {
                            painted = CGRectUnion(
                                painted,
                                CGRectMake(placed.box_rect.x, placed.box_rect.y,
                                           placed.box_rect.width,
                                           placed.box_rect.height));
                        }
                        ink = CGRectIsNull(ink) ? painted
                                                : CGRectUnion(ink, painted);
                    }
                    return true;
                },
                error);
            if (!CGRectIsNull(ink)) {
                ink = CGRectMake(ink.origin.x / width, ink.origin.y / height,
                                 ink.size.width / width,
                                 ink.size.height / height);
            }
        }
        if (picture == nullptr) {
            hide();
            return;
        }
        view.layer.contents = (__bridge id)picture;
        CGImageRelease(picture);
        view.drawnKey = drawnKey;
        view.inkShare = ink;
    }
    view.frame =
        NSMakeRect(canvas.canvas_x,
                   bounds.size.height - canvas.canvas_y - canvas.canvas_height,
                   canvas.canvas_width, canvas.canvas_height);
    view.captionText = text;
    view.hidden = NO;
}

- (void)presentNearestFrameAtDeadline:(BOOL)isDisplayDeadline {
    if (!self.state->frameCache || !self.state->renderer ||
        !self.state->sourceRenderer || !self.state->programRenderer) {
        return;
    }
    // PERF7 -- ROADMAP.md. The timeline's display list is timed further
    // down. The report is off unless CUTMACHINE_RENDER_COST says where to
    // write it (RenderCostMeter.h), and off costs this null check. The
    // signposts are always emitted and cost an enabled check until
    // Instruments records Points of Interest. Statics, because both describe
    // this process, never the project.
    static const std::unique_ptr<render_cost::Recorder> renderCost = [] {
        gRenderCostOriginNs = render_cost::NowNanoseconds();
        return render_cost::Recorder::Open(
            render_cost::ParseDestination(
                std::getenv(render_cost::kEnvironmentVariable)),
            gRenderCostOriginNs);
    }();
    static os_log_t const renderLog = os_log_create(
        "com.cutmachine.editor", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    if (renderCost && isDisplayDeadline) {
        renderCost->Tick(render_cost::NowNanoseconds());
    }

    std::vector<AVFrame*> frames(self.state->requested.size(), nullptr);
    std::vector<RenderedSlot> candidates(self.state->requested.size());
    std::vector<playback::LayerReadiness> readiness(
        self.state->requested.size());
    // MOTION-2026-09 M4 -- a motion clip's frame is drawn here, not decoded:
    // rendered once (0.5 ms at 1080x1920, M4), kept in motionPreviews, and
    // handed to Renderer.mm as a planar picture with alpha. Its AVFrame only
    // points into the cache (no buffer references), and, like every entry of
    // `frames`, is freed by the loop at the end of this method, after
    // RenderFrames has copied it. It must have no other owner: a second
    // av_frame_free of the same struct corrupted the heap as soon as a
    // motion clip reached the Record monitor (visual QA, 19 September 2026).
    std::vector<uint8_t> displayReferred(self.state->requested.size(), 0);
    const std::string motionDirectory =
        self.documentPath.length > 0
            ? std::filesystem::absolute(self.documentPath.UTF8String)
                  .parent_path()
                  .string()
            : std::string();
    bool missing = false;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    // PERF13 -- see gUiPlayProbeTopDrops: overwritten by each active layer,
    // so the topmost one's is left.
    bool topmostLate = false;
#endif
    for (size_t slot = 0; slot < self.state->requested.size(); ++slot) {
        const ResolvedSlot& requested = self.state->requested[slot];
        if (!requested.active) {
            continue;
        }
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        topmostLate = false;
#endif
        MotionPreviewRequest motion;
        if (MotionPreviewFor(
                self.state->document,
                {requested.sourceId, requested.frame, requested.clipId},
                motion)) {
            const MotionPreviewImage* image =
                self.state->motionPreviews->Find(motion.key, motion.frame);
            if (image == nullptr && self.state->failedMotionFrames.count(
                                        {motion.key, motion.frame}) == 0) {
                MotionPreviewImage rendered;
                MotionError error;
                if (RenderMotionPreview(motion, self.state->document,
                                        motionDirectory, rendered, error)) {
                    image = &self.state->motionPreviews->Insert(
                        motion.key, motion.frame, std::move(rendered));
                } else {
                    self.state->failedMotionFrames.insert(
                        {motion.key, motion.frame});
                    if (self.state->reportedMotionKeys.insert(motion.key)
                            .second)
                        std::fprintf(stderr, "Motion clip %s: %s: %s\n",
                                     requested.clipId.c_str(),
                                     error.code.c_str(), error.message.c_str());
                }
            }
            candidates[slot] = {true, motion.key, motion.frame,
                                requested.opacity, requested.clipId};
            readiness[slot] = {true, image != nullptr, true};
            AVFrame* frame = image != nullptr ? av_frame_alloc() : nullptr;
            if (frame != nullptr) {
                frame->format = AV_PIX_FMT_GBRAP;
                frame->width = image->width;
                frame->height = image->height;
                frame->color_range = AVCOL_RANGE_JPEG;
                uint8_t* planes = const_cast<uint8_t*>(image->planes.data());
                for (int plane = 0; plane < 4; ++plane) {
                    frame->data[plane] = planes + (image->PlaneSize() * plane);
                    frame->linesize[plane] = image->width;
                }
                frames[slot] = frame;
                displayReferred[slot] = 1;
            }
#if defined(CUTMACHINE_UI_SMOKE_TEST)
            topmostLate = frame == nullptr;
#endif
            continue;
        }
        int64_t cachedFrame = -1;
        frames[slot] = self.state->frameCache->GetNearest(
            requested.sourceId, requested.frame, cachedFrame);
        // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. A media declared
        // a graphic is placed as a motion frame is: in the graphics view,
        // its code values being what it shows (Renderer.mm decodes its
        // YCbCr with its own matrix and range).
        if (self.state->document.IsGraphicsMedia(requested.sourceId))
            displayReferred[slot] = 1;
        candidates[slot] = {true, requested.sourceId, cachedFrame,
                            requested.opacity, requested.clipId};
        // PERF10 -- a source that is not offline has a decoder, or one on
        // its way: its layer is late, not missing.
        const bool decodable =
            self.state->offlineSourceIds.count(requested.sourceId) == 0;
        readiness[slot] = {true, frames[slot] != nullptr, decodable};
        if (!frames[slot] || cachedFrame != requested.frame) {
            missing = true;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
            topmostLate = true;
#endif
        }
    }
    if (isDisplayDeadline && missing) {
        self.state->performanceMetrics->RecordDrop();
    }
    // FC4 -- the Trim Monitor's two frames often come from one rush, whose
    // decoder holds one target at a time: the first frame not in the cache
    // is asked again, until both are.
    if (!self.state->trimMonitorPanes.empty()) {
        for (size_t slot = 0; slot < self.state->requested.size(); ++slot) {
            const ResolvedSlot& requested = self.state->requested[slot];
            if (requested.active && candidates[slot].frame != requested.frame) {
                [self requestFrame:requested.frame ofSource:requested.sourceId];
                break;
            }
        }
    }
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (isDisplayDeadline && !gUiPlayProbeProject.empty()) {
        ++gUiPlayProbeTicks;
        if (topmostLate) ++gUiPlayProbeTopDrops;
    }
#endif
    // PERF-2026-09. A layer still waiting on its decoder must not composite:
    // Renderer.mm skips a null frame, so drawing now would punch a hole where
    // the edit says an image is and flash the track underneath. Keep the
    // previous image until the decode lands -- see PlaybackPresentation.h.
    const playback::HoldDecision holdProgram = playback::DecideProgramHold(
        readiness, self.state->programPresented, self.state->programHeldTicks);
    self.state->programHeldTicks = holdProgram.held_ticks;
    const bool programChanged = candidates != self.state->rendered;
    // ON5 -- Media detaches Record and the timeline. Source keeps rendering,
    // and a clean feed, when active, remains a program presentation surface.
    const bool recordVisible =
        self.programMonitorView.window != nil &&
        !self.programMonitorView.hiddenOrHasHiddenAncestor;
    const bool timelineVisible = self.metalView.window == self.window &&
                                 !self.metalView.hiddenOrHasHiddenAncestor;
    const bool programVisible =
        recordVisible || self.state->videoOutputRenderer;
    // FC4 -- the Trim Monitor shows two frames of the cut, not the
    // program's captions.
    const auto activeSubtitles =
        self.state->trimMonitorPanes.empty()
            ? ActiveSubtitles(self.state->document,
                              self.state->requestedPosition)
            : std::vector<const DocumentClip*>{};
    std::vector<CaptionCarton> cartons;
    NSMutableArray<NSString*>* subtitleLines = [NSMutableArray array];
    for (const DocumentClip* subtitle : activeSubtitles) {
        // D7 -- a carton being typed in the Inspector shows what is typed.
        CaptionCarton carton;
        carton.text = ui::media_panel::DisplayedCartonText(
            *subtitle, self.state->cartonPreview);
        if (carton.text.empty()) continue;
        if (const CaptionStyle* stored = self.state->document.FindCaptionStyle(
                subtitle->caption_group_id))
            carton.style = *stored;
        carton.opacity = subtitle->opacity;
        // PA4 -- docs/roadmap/PARITE-2026-11.md. The words started at the
        // playhead, as the export counts them at a frame's start. A carton
        // being typed is drawn whole: its times are those of the text it
        // had.
        if (!carton.style.word_animation.empty() &&
            !subtitle->caption_word_times.empty() &&
            carton.text == subtitle->caption_text)
            carton.words_spoken =
                CaptionWordsSpoken(*subtitle, self.state->requestedPosition);
        [subtitleLines addObject:CMStringFromUtf8(carton.text)];
        cartons.push_back(std::move(carton));
    }
    [self showProgramCaptions:cartons
                         text:[subtitleLines componentsJoinedByString:@"\n"]];
    // CF1 -- a gap can keep the same decoded frames while captions change.
    // They advance independently, except when the compositor holds a frame.
    if (self.state->videoOutputRenderer && !holdProgram.hold)
        [self showCaptions:cartons
                      text:[subtitleLines componentsJoinedByString:@"\n"]
                      view:self.videoOutputCaptionView
                   monitor:self.videoOutputView
                      zoom:0.0];
    if ((self.state->overlayDirty && (programVisible || timelineVisible)) ||
        (programChanged && programVisible) ||
        self.state->grabReferenceRequested ||
        (self.state->videoOutputNeedsPresent &&
         self.state->videoOutputRenderer)) {
        [self refreshSequenceLookLut];
        TimelineRenderData programData;
        programData.color_management = self.state->document.color_management;
        programData.display_sdr_preview = true;
        programData.sequence_width = self.state->document.sequence.width;
        programData.sequence_height = self.state->document.sequence.height;
        programData.video_height = self.programMonitorView.bounds.size.height;
        programData.video_zoom = self.state->programMonitorZoom;
        // DEC42 -- Record's zoom frames the sequence, as Resolve's viewer.
        programData.video_zoom_frames_sequence = true;
        programData.video_scope = self.state->programVideoScope;
        programData.video_scope_bottom_inset = [self programScopeBottomInset];
        programData.video_rotation_degrees.resize(candidates.size(), 0);
        programData.video_opacities.resize(candidates.size(), 1.0f);
        // Record is the canonical edited output: both monitors share the same
        // SDR presentation transform, while Record additionally displays the
        // clip's creative grade. Source remains the ungraded rush reference.
        programData.video_color_grades.resize(candidates.size());
        programData.video_display_referred = displayReferred;
        // G3 -- docs/roadmap/GRADE-2026-09.md. Each frame read with its
        // media's own input, as the export reads it.
        programData.video_color_inputs.resize(candidates.size());
        // R1 -- docs/roadmap/REEL-2026-09.md. One entry per composited
        // frame, resolved by ClipFraming.h and never here: Record shows the
        // clip at exactly the rectangle the export writes. A clip that
        // frames nothing leaves its entry empty and is fitted as before.
        programData.video_framings.resize(candidates.size());
        // R5 -- docs/roadmap/REEL-2026-09.md. One entry per composited
        // frame as well: the colour dip over the track that frame came
        // from, at the exact strength TransitionRender.h computes, which
        // is the same number the export hands FFmpeg. A track that dips
        // nowhere leaves its entry at zero and is drawn as before.
        programData.video_veils.resize(candidates.size());
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. One entry per composited
        // frame as well: its clip's blur, the sigma in sequence pixels the
        // export carries into the delivery's (ClipBlur.h). A sharp clip
        // leaves it at zero and is drawn as before.
        programData.video_blurs.resize(candidates.size(), 0.0f);
        // PA8 -- docs/roadmap/PARITE-2026-11.md. And its clip's mask.
        programData.video_masks.resize(candidates.size(), MonitorMask{});
        // R6 -- docs/roadmap/REEL-2026-09.md. The look of the whole
        // sequence, read from the package and checked against its digest
        // by LoadSequenceLookLut: Record shows the film with the look the
        // export writes. Source stays the ungraded rush, as it stays
        // ungraded by the clip's own color.* stack.
        // G6 -- without the grade, the look goes too: it is the sequence's
        // grade.
        const bool bypassGrade = self.state->monitorCompare.bypass_grade;
        if (!bypassGrade) {
            programData.look_lut = self.state->lookLut.samples;
            programData.look_lut_digest = self.state->lookLut.digest;
            programData.look_lut_stage = self.state->lookLut.stage;
        }
        for (size_t slot = 0; slot < candidates.size(); ++slot) {
            if (!candidates[slot].active) continue;
            programData.video_opacities[slot] = candidates[slot].opacity;
            if (const DocumentTrack* veiledTrack =
                    self.state->document.FindTrackForClip(
                        candidates[slot].clipId)) {
                const std::optional<ResolvedColorVeil> veil =
                    self.state->timeline->ResolveTrackColorVeil(
                        veiledTrack->id, self.state->requestedPosition);
                if (veil) {
                    programData.video_veils[slot] = {
                        static_cast<float>(veil->color.red) / 255.0f,
                        static_cast<float>(veil->color.green) / 255.0f,
                        static_cast<float>(veil->color.blue) / 255.0f,
                        TransitionAlphaFloat(veil->alpha)};
                }
            }
            if (const DocumentClip* clip =
                    self.state->document.FindClip(candidates[slot].clipId)) {
                if (const LibraryMedia* media =
                        self.state->document.FindLibraryMedia(clip->source_id))
                    programData.video_color_inputs[slot] = media->color_input;
                if (ClipHasBlur(*clip) && clip->blur.den > 0)
                    programData.video_blurs[slot] =
                        static_cast<float>(clip->blur.num) /
                        static_cast<float>(clip->blur.den);
                programData.video_masks[slot] = MonitorMaskOf(clip->mask);
                if (!bypassGrade)
                    programData.video_color_grades[slot] = ResolveColorGrade(
                        ui::inspector::DisplayedEffects(
                            *clip, self.state->gradePreview),
                        ColorGradeContextFor(self.state->document, *clip));
                // R2 -- docs/roadmap/REEL-2026-09.md. The values THIS
                // frame shows, sampled by ClipAnimation.h -- the same
                // function the export, the sheets and verify_export call,
                // so Record shows the zoom the file will hold. A clip that
                // animates nothing keeps its constants, and its pixels.
                // R2c -- the opacity is the slot's already: Timeline samples
                // it (ResolveTrackLayers), the key replacing the constant as
                // in the export. Multiplying it here again showed a keyed
                // or framing-keyed translucent clip darker than delivered.
                // While an Inspector drag moves a key, the clip is sampled
                // with the keys it would commit.
                const std::optional<DocumentClip> previewed =
                    ui::inspector::PreviewedClip(*clip,
                                                 self.state->framingPreview);
                const DocumentClip& shown = previewed ? *previewed : *clip;
                ClipSampledParameters sampled;
                sampled.opacity = shown.opacity;
                sampled.framing = shown.framing;
                if (ClipHasAnimation(shown)) {
                    std::string animationError;
                    const RationalTime offset =
                        self.state->requestedPosition.sub(shown.timeline_in);
                    const MediaRate rate =
                        self.state->document.sequence.frame_rate;
                    SampleClipParameters(shown,
                                         offset.to_frames(rate.num, rate.den),
                                         rate, sampled, animationError);
                }
                if (ClipHasFraming(shown) || ClipHasAnimation(shown)) {
                    ClipFramingGeometry geometry;
                    int64_t displayedWidth = 0;
                    int64_t displayedHeight = 0;
                    std::string framingError;
                    if (ClipDisplayedSize(self.state->document, shown,
                                          displayedWidth, displayedHeight,
                                          framingError) &&
                        ResolveClipFraming(sampled.framing, displayedWidth,
                                           displayedHeight,
                                           self.state->document.sequence.width,
                                           self.state->document.sequence.height,
                                           geometry, framingError)) {
                        programData.video_framings[slot] =
                            ClipHasAnimation(shown)
                                ? MonitorFramingOfWholePicture(
                                      geometry, displayedWidth, displayedHeight)
                                : MonitorFramingOf(geometry, displayedWidth,
                                                   displayedHeight);
                    }
                }
            }
            const auto media =
                self.state->mediaMetadata.find(candidates[slot].sourceId);
            if (media != self.state->mediaMetadata.end() &&
                media->second.metadata_complete)
                programData.video_rotation_degrees[slot] =
                    media->second.rotation_degrees;
        }
        // FC4 -- the Trim Monitor: each frame whole in its pane, opaque,
        // under no dip, and the counters over it.
        if (!self.state->trimMonitorPanes.empty() &&
            self.state->trimMonitorPanes.size() == candidates.size())
            [self layOutTrimMonitor:programData];
        // CF1 -- the same decoded composite on the chosen screen, fitted
        // independently of Record's zoom, without scopes or trim labels.
        // Captions use that screen's own pixel scale and stay with the
        // picture when a late decoder makes both monitors hold.
        if (self.state->videoOutputRenderer && !holdProgram.hold) {
            const TimelineRenderData feed = CleanFeedRenderData(
                programData, self.videoOutputView.bounds.size.height);
            const bool presented =
                self.state->videoOutputRenderer->RenderFrames(frames, feed);
            self.state->videoOutputNeedsPresent = !presented;
            if (presented && !recordVisible) {
                self.state->rendered = candidates;
                self.state->programPresented = true;
            }
        }
        // Building the display list while holding costs a few vectors and is
        // thrown away; what must not happen is the present itself, which is
        // where the hole would reach the screen.
        // G6 -- docs/roadmap/GRADE-2026-09.md. A grab takes Record as it is
        // about to be shown, at the sequence's size and without the scopes
        // or the reference; then the reference is laid over its half.
        if (self.state->grabReferenceRequested && !holdProgram.hold) {
            self.state->grabReferenceRequested = false;
            TimelineRenderData grab = programData;
            const int32_t width = self.state->document.sequence.width;
            const int32_t height = self.state->document.sequence.height;
            grab.video_height = height;
            grab.video_zoom = 0.0;
            grab.video_scope = VideoScopeMode::Off;
            grab.reference_still = nullptr;
            std::vector<float> pixels;
            if (width <= 0 || height <= 0 ||
                !self.state->programRenderer->RenderFramesToPixels(
                    frames, grab, static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height), pixels) ||
                ![self keepReferenceStill:ReferenceStillFromRgba(pixels, width,
                                                                 height)])
                NSBeep();
        }
        if (self.state->monitorCompare.split) {
            programData.reference_still = self.state->referenceStill;
            programData.reference_position =
                self.state->monitorCompare.split_position;
        }
        programData.curve_histogram =
            recordVisible && [self.inspectorView wantsCurveHistogram] &&
            self.state->trimMonitorPanes.empty();
        if (recordVisible && !holdProgram.hold &&
            self.state->programRenderer->RenderFrames(frames, programData)) {
            self.state->rendered = candidates;
            self.state->programPresented = true;
            [self.inspectorView showCurveHistogram:self.state->programRenderer
                                                       ->CurveHistogram()];
        }
        if (timelineVisible) {
            // PERF7 -- ROADMAP.md. Measured, not changed: the list is rebuilt
            // whenever the overlay is dirty or the Record composite moved,
            // which in playback is every tick. "Submit" is Renderer.mm's whole
            // call -- one draw call per quad, the wait for a drawable, then for
            // the GPU
            // -- i.e. how long the main thread is held, not only encoding.
            const bool timelineSignposts = os_signpost_enabled(renderLog);
            const bool measureTimeline = renderCost || timelineSignposts;
            const int64_t buildStart =
                measureTimeline ? render_cost::NowNanoseconds() : 0;
            os_signpost_interval_begin(renderLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                       "Timeline display list");
            TimelineRenderData timelineData = [self timelineRenderData];
            const int64_t buildEnd =
                measureTimeline ? render_cost::NowNanoseconds() : 0;
            render_cost::DisplayListCounts timelineCounts;
            if (measureTimeline) {
                for (const MetalDrawCommand& command : timelineData.overlays) {
                    if (std::holds_alternative<MetalRect>(command)) {
                        ++timelineCounts.rects;
                    } else if (const MetalText* text =
                                   std::get_if<MetalText>(&command)) {
                        ++timelineCounts.texts;
                        timelineCounts.text_bytes += text->text.size();
                    } else {
                        ++timelineCounts.icons;
                    }
                }
            }
            os_signpost_interval_end(
                renderLog, OS_SIGNPOST_ID_EXCLUSIVE, "Timeline display list",
                "%llu rects, %llu texts, %llu icons",
                static_cast<unsigned long long>(timelineCounts.rects),
                static_cast<unsigned long long>(timelineCounts.texts),
                static_cast<unsigned long long>(timelineCounts.icons));
            const int64_t submitStart =
                measureTimeline ? render_cost::NowNanoseconds() : 0;
            os_signpost_interval_begin(renderLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                       "Timeline submit");
            const std::vector<AVFrame*> noFrames;
            self.state->renderer->RenderFrames(noFrames, timelineData);
            os_signpost_interval_end(renderLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                     "Timeline submit");
            if (renderCost) {
                renderCost->Record({buildEnd - buildStart,
                                    render_cost::NowNanoseconds() - submitStart,
                                    timelineCounts});
            }
        }
    }

    if (self.state->sourceMonitorVisible && self.state->sourceMonitor &&
        !self.state->sourceMonitorId.empty()) {
        const int64_t requestedFrame =
            self.state->sourceMonitorPosition.to_frames(
                self.state->sourceMonitorPosition.rate);
        int64_t cachedFrame = -1;
        AVFrame* sourceFrame = self.state->frameCache->GetNearest(
            self.state->sourceMonitorId, requestedFrame, cachedFrame);
        const RenderedSlot sourceCandidate{true, self.state->sourceMonitorId,
                                           cachedFrame};
        if (self.state->overlayDirty ||
            !(sourceCandidate == self.state->sourceRendered)) {
            TimelineRenderData sourceData;
            sourceData.color_management = self.state->document.color_management;
            sourceData.display_sdr_preview = true;
            // Both viewers use the sequence canvas. Portrait media is then
            // letterboxed at the same scale in Source and Record.
            sourceData.sequence_width = self.state->document.sequence.width;
            sourceData.sequence_height = self.state->document.sequence.height;
            sourceData.video_height = self.sourceMonitorView.bounds.size.height;
            sourceData.video_zoom = self.state->sourceMonitorZoom;
            sourceData.video_rotation_degrees = {0};
            // G3 -- the rush read with its own declared input. DEC31 -- a
            // graphic is shown as Record places it.
            if (const LibraryMedia* media =
                    self.state->document.FindLibraryMedia(
                        self.state->sourceMonitorId)) {
                sourceData.video_color_inputs = {media->color_input};
                if (media->color_input.graphics)
                    sourceData.video_display_referred = {1};
            }
            const auto metadata =
                self.state->mediaMetadata.find(self.state->sourceMonitorId);
            if (metadata != self.state->mediaMetadata.end()) {
                if (metadata->second.metadata_complete) {
                    sourceData.video_rotation_degrees[0] =
                        metadata->second.rotation_degrees;
                }
            }
            // Out of lot -- the author, 29 October 2026: the source player's
            // indicator. The whole width is the whole rush, as the click that
            // scrubs it reads it; In/Out on it, the playhead across it
            // (LayoutSourceBar, TransportBar.h).
            if (const DocumentSource* shown = self.state->document.FindSource(
                    self.state->sourceMonitorId)) {
                try {
                    const SourceBarGeometry bar = LayoutSourceBar(
                        self.sourceMonitorView.bounds.size.width,
                        self.sourceMonitorView.bounds.size.height,
                        ScrubBarRange{shown->duration},
                        self.state->sourceMonitorPosition, self.state->sourceIn,
                        self.state->sourceOut);
                    const ui::theme::Color track =
                        ui::theme::WithAlpha(ui::theme::kSurfaceSunken, 0.85f);
                    const ui::theme::Color zone =
                        ui::theme::WithAlpha(ui::theme::kAccent, 0.45f);
                    sourceData.overlays.emplace_back(
                        MetalRect{bar.track_x, bar.track_y, bar.track_width,
                                  bar.track_height, track, track});
                    if (bar.zone_width > 0.0)
                        sourceData.overlays.emplace_back(
                            MetalRect{bar.zone_x, bar.track_y, bar.zone_width,
                                      bar.track_height, zone, zone});
                    sourceData.overlays.emplace_back(
                        MetalRect{bar.playhead_x, bar.playhead_y,
                                  bar.playhead_width, bar.playhead_height,
                                  ui::theme::kAccent, ui::theme::kAccent});
                } catch (const std::exception&) {
                }
            }
            const std::vector<AVFrame*> sourceFrames{sourceFrame};
            if (self.state->sourceRenderer->RenderFrames(sourceFrames,
                                                         sourceData))
                self.state->sourceRendered = sourceCandidate;
        }
        av_frame_free(&sourceFrame);
    } else if (self.state->sourceMonitorVisible && self.state->overlayDirty) {
        TimelineRenderData sourceData;
        sourceData.video_height = self.sourceMonitorView.bounds.size.height;
        const std::vector<AVFrame*> noFrames;
        self.state->sourceRenderer->RenderFrames(noFrames, sourceData);
    }
    self.state->overlayDirty = false;
    for (AVFrame*& frame : frames) av_frame_free(&frame);
}

- (void)windowDidResize:(NSNotification*)notification {
    if (notification.object == self.agentWindow) {
        [self agentWindowDidChangeFrame];
        return;
    }
    if (self.state->renderer) {
        [self refreshTimelineChrome];
        self.state->overlayDirty = true;
    }
}

// FitDuration rejects a width at or below the track-header gutter, which is
// the right contract for it: below that there is no timeline left to scale
// into, so any answer it returned would be a lie. A view that AppKit has not
// laid out yet reports exactly such a width, though, and that is an ordinary
// transient state rather than a caller error -- so ask here instead of
// letting an uncaught C++ exception out of a Cocoa callback and abort the
// process. -timelineMetalViewDidResize: re-fits as soon as there is a real
// width, so nothing is lost by skipping.
- (void)fitTimelineToViewportWidth {
    if (!self.state) return;
    const double width = self.metalView.bounds.size.width;
    if (!std::isfinite(width) || width <= self.state->viewport.header_width)
        return;
    self.state->timelineFit =
        FitTimeline(self.state->viewport, self.state->duration, width);
    self.state->overlayDirty = true;
}

// VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The editor window as it
// stands, drawn without the screen (WindowCapture.h): each Metal surface on
// show presents once more and its renderer keeps a copy. Record keeps its
// previous picture, and has none to give, while a layer waits on its decoder
// (PlaybackPresentation.h): a few more ticks are tried, then the capture is
// refused rather than given with a hole where Record is.
- (NSBitmapImageRep*)drawEditorWindowOffscreen:(std::string&)error {
    error.clear();
    if (self.window == nil || !self.state || !self.state->renderer ||
        !self.state->sourceRenderer || !self.state->programRenderer) {
        error = "the editor window is not open";
        return nil;
    }
    struct Surface {
        TimelineMetalView* view;
        Renderer* renderer;
        const char* name;
    };
    const Surface all[] = {
        {self.metalView, self.state->renderer.get(), "the timeline"},
        {self.sourceMonitorView, self.state->sourceRenderer.get(), "Source"},
        {self.programMonitorView, self.state->programRenderer.get(), "Record"},
    };
    std::vector<const Surface*> waiting;
    for (const Surface& surface : all) {
        if (surface.view.window != self.window ||
            surface.view.isHiddenOrHasHiddenAncestor ||
            NSIsEmptyRect(surface.view.bounds))
            continue;
        surface.renderer->CaptureNextPresent();
        waiting.push_back(&surface);
    }
    std::vector<WindowCaptureSurface> surfaces;
    for (int attempt = 0; attempt < 10 && !waiting.empty(); ++attempt) {
        if (attempt > 0)
            [NSRunLoop.currentRunLoop
                   runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        self.state->overlayDirty = true;
        [self presentNearestFrameAtDeadline:NO];
        for (auto it = waiting.begin(); it != waiting.end();) {
            CGImageRef image = (*it)->renderer->TakePresentCapture();
            if (image != nullptr) {
                surfaces.push_back({(*it)->view, image});
                it = waiting.erase(it);
            } else {
                (*it)->renderer->CaptureNextPresent();
                ++it;
            }
        }
    }
    NSBitmapImageRep* drawn = nil;
    if (waiting.empty()) {
        drawn = DrawWindowOffscreen(self.window, surfaces, error);
    } else {
        error = std::string(waiting.front()->name) +
                " did not present again: a layer still waits on its decoder";
        // Disarmed: the playback that follows draws each picture once.
        for (const Surface* surface : waiting)
            CGImageRelease(surface->renderer->TakePresentCapture());
    }
    for (const WindowCaptureSurface& surface : surfaces)
        CGImageRelease(surface.image);
    return drawn;
}

- (void)timelineMetalViewDidResize:(TimelineMetalView*)view {
    if (view == self.metalView) [self layOutTrimBar];
    if (!self.state) return;
    if (view == self.metalView && self.state->renderer) {
        self.state->renderer->Resize(view.bounds);
        // PRD37 -- ROADMAP.md. A fit made while the window was still being
        // laid out follows the width the view settles at (QA of 3 October
        // 2026: fitted at 971 points, shown at 917.5, the end of the film
        // out of view); once a zoom or a scroll has moved it, the scale is
        // the editor's and a resize keeps it.
        if (TimelineFitFollowsResize(self.state->timelineFit,
                                     self.state->viewport,
                                     view.bounds.size.width))
            [self fitTimelineToViewportWidth];
        [self refreshTimelineChrome];
    } else if (view == self.sourceMonitorView && self.state->sourceRenderer) {
        self.state->sourceRenderer->Resize(view.bounds);
    } else if (view == self.programMonitorView && self.state->programRenderer) {
        self.state->programRenderer->Resize(view.bounds);
    } else if (view == self.videoOutputView &&
               self.state->videoOutputRenderer) {
        self.state->videoOutputRenderer->Resize(view.bounds);
        self.state->videoOutputNeedsPresent = true;
    }
    self.state->overlayDirty = true;
}

// The workspace is three panes -- media | editor | right dock -- so it has
// two dividers, and they cannot share one constraint. Divider 1 sits roughly
// a dock's width from the right edge, which is far past where divider 0 is
// allowed to go; answering divider 0's range for both is what collapsed the
// editor and the dock to zero width on launch, and a zero-width timeline is
// what made FitDuration throw. Every bound below is an absolute x in the
// split view, which is the coordinate space AppKit asks about.
static constexpr CGFloat kMediaPaneMinWidth = 220.0;
static constexpr CGFloat kEditorPaneMinWidth = 360.0;
static constexpr CGFloat kRightDockMinWidth = 260.0;

// ON1 -- docs/roadmap/ONGLETS-2026-10.md. A page shows one, two or three of
// the workspace's panes, so a divider's bounds are read off the panes
// actually around it: the pane before it keeps its own minimum from where it
// starts, the panes after it keep theirs up to the right edge.
- (CGFloat)workspacePaneMinimum:(NSView*)pane {
    if (pane == self.mediaPanel) return kMediaPaneMinWidth;
    if (pane == self.rightDockPanel) return kRightDockMinWidth;
    return kEditorPaneMinWidth;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainSplitPosition:(CGFloat)proposedPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    if (splitView == self.monitorSplitView && self.state &&
        self.state->sourceMonitorVisible && dividerIndex == 0) {
        CGFloat width = splitView.bounds.size.width;
        if ([splitView.arrangedSubviews
                containsObject:self.workspaceActivityScroll]) {
            width -= self.workspaceActivityScroll.frame.size.width +
                     splitView.dividerThickness;
        }
        return (width - splitView.dividerThickness) * 0.5;
    }
    return proposedPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMinCoordinate:(CGFloat)proposedMinimumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    if (splitView == self.workspaceSplitView) {
        NSArray<NSView*>* panes = splitView.arrangedSubviews;
        if (dividerIndex < 0 ||
            dividerIndex >= static_cast<NSInteger>(panes.count))
            return proposedMinimumPosition;
        NSView* before = panes[static_cast<NSUInteger>(dividerIndex)];
        return NSMinX(before.frame) + [self workspacePaneMinimum:before];
    }
    if (splitView == self.editorSplitView) return 180.0;
    if (splitView == self.monitorSplitView)
        return self.state && !self.state->sourceMonitorVisible ? 0.0 : 320.0;
    return proposedMinimumPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMaxCoordinate:(CGFloat)proposedMaximumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    const CGFloat divider = splitView.dividerThickness;
    if (splitView == self.workspaceSplitView) {
        NSArray<NSView*>* panes = splitView.arrangedSubviews;
        if (dividerIndex < 0 ||
            dividerIndex + 1 >= static_cast<NSInteger>(panes.count))
            return proposedMaximumPosition;
        CGFloat after = 0.0;
        for (NSUInteger index = static_cast<NSUInteger>(dividerIndex) + 1;
             index < panes.count; ++index)
            after += [self workspacePaneMinimum:panes[index]] + divider;
        NSView* before = panes[static_cast<NSUInteger>(dividerIndex)];
        return std::max<CGFloat>(
            NSMinX(before.frame) + [self workspacePaneMinimum:before],
            splitView.bounds.size.width - after);
    }
    if (splitView == self.editorSplitView)
        return std::max<CGFloat>(
            180.0, splitView.bounds.size.height - 200.0 - divider);
    if (splitView == self.monitorSplitView)
        return self.state && !self.state->sourceMonitorVisible
                   ? splitView.bounds.size.width
                   : std::max<CGFloat>(320.0,
                                       splitView.bounds.size.width - 320.0);
    return proposedMaximumPosition;
}

- (BOOL)splitView:(NSSplitView*)splitView canCollapseSubview:(NSView*)subview {
    return splitView == self.monitorSplitView &&
           subview == self.sourceMonitorPanel;
}

- (void)splitViewDidResizeSubviews:(NSNotification*)notification {
    if (notification.object == self.workspaceSplitView) {
        [self layOutMediaBrowser];
    }
    // ON2 -- a side panel dragged to a width keeps it on this page.
    if (notification.object == self.workspaceSplitView &&
        !self.applyingWorkspace && self.rememberWorkspace &&
        NSApp.currentEvent.type == NSEventTypeLeftMouseDragged) {
        NSArray<NSView*>* panes = self.workspaceSplitView.arrangedSubviews;
        ui::workspace::PanelWidths widths = [CMUiPreferences.sharedPreferences
            panelWidthsForPage:self.workspacePage];
        if ([panes containsObject:self.mediaPanel])
            widths.media = self.mediaPanel.frame.size.width;
        if ([panes containsObject:self.rightDockPanel])
            widths.inspector = self.rightDockPanel.frame.size.width;
        [CMUiPreferences.sharedPreferences setPanelWidths:widths
                                                  forPage:self.workspacePage];
    }
    if (!self.state) return;
    [self refreshTimelineChrome];
    self.state->overlayDirty = true;
}

// AX1 -- docs/roadmap/AX-2026-09.md. The MCP server Claude Code starts from
// `.mcp.json` relays to this endpoint when it is asked to open this project
// (McpEndpoint.h): the agent edits the document on screen, through the
// chat panel's live backend, one journal, one Undo. Announced in the
// project's lock directory, so only while this window holds the project.
- (void)startAgentEndpoint {
    if (self.agentEndpoint || !self.chatBackend ||
        self.documentPath.length == 0 || !self.state ||
        !self.state->projectLock || !self.state->projectLock->Held())
        return;
    std::string socketPath;
    std::string error;
    if (!PrepareMcpSocketPath(GenerateUlid(), socketPath, error)) {
        std::fprintf(stderr, "Agent endpoint not started: %s\n", error.c_str());
        return;
    }
    self.agentEndpointBackend = new MainThreadBackend(*self.chatBackend);
    McpEndpoint* endpoint = new McpEndpoint(*self.agentEndpointBackend);
    // DEC10 -- docs/roadmap/AX-2026-09.md (AX7). The relayed agent's tasks
    // (exports, analyses asked with `async: true`) run on the editor's
    // manager: the task bar shows them and cancels them, as it does
    // ingest_media. The manager outlives the endpoint, which is abandoned,
    // never destroyed, as the app quits.
    if (self.state->mediaTasks)
        endpoint->Server().Session().SetTaskManager(
            self.state->mediaTasks.get(), "Agent · ");
    __weak AppDelegate* weakSelf = self;
    endpoint->SetObserver([weakSelf](const McpEndpoint::Event& event) {
        const McpEndpoint::Event copy = event;
        dispatch_async(dispatch_get_main_queue(), ^{
          AppDelegate* strongSelf = weakSelf;
          if (strongSelf) [strongSelf showAgentEndpointEvent:copy];
        });
    });
    if (!endpoint->Start(socketPath, error) ||
        !endpoint->Announce(self.documentPath.UTF8String, error)) {
        std::fprintf(stderr, "Agent endpoint not started: %s\n", error.c_str());
        endpoint->Stop();
        return;
    }
    self.agentEndpoint = endpoint;
}

- (void)showAgentEndpointEvent:(const McpEndpoint::Event&)event {
    switch (event.kind) {
        case McpEndpoint::Event::Kind::Connected:
            self.infoLabel.stringValue = @"Agent connecté au projet";
            break;
        case McpEndpoint::Event::Kind::Disconnected:
            self.infoLabel.stringValue = @"Agent déconnecté";
            break;
        case McpEndpoint::Event::Kind::ToolCall:
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Agent : %@%@", CMStringFromUtf8(event.tool),
                                 event.ok ? @"" : @" (refusé)"];
            break;
    }
    // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The status line keeps
    // the last call; the journal keeps what the agent did (AgentJournal.h),
    // in the Agent panel whether it is shown or not, and the timeline marks
    // the clips its last call touched.
    AgentJournalContext context;
    context.time = [self agentJournalTime];
    AppState* state = self.state;
    context.timeline_name = [state](const std::string& timelineId) {
        if (timelineId == state->activeTimelineId)
            return state->document.sequence.name;
        const DocumentSequence* timeline =
            state->project.FindTimeline(timelineId);
        return timeline != nullptr ? timeline->name : std::string();
    };
    if (state->agentJournal.Record(event, context)) {
        [self.chatPanelView
            showAgentJournal:state->agentJournal.TranscriptEntries()];
        state->overlayDirty = true;
    }
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    ++gUiSmokeAgentEvents;
#endif
}

// PRD25 -- the local time a journal line is written at, "HH:MM:SS": read
// here, never in the journal's policy.
- (std::string)agentJournalTime {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (!gUiSmokeAgentJournalTime.empty()) return gUiSmokeAgentJournalTime;
#endif
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[16];
    std::strftime(text, sizeof text, "%H:%M:%S", &local);
    return text;
}

// PRD25 -- the person acted in the timeline (a click, a key, an undo): the
// marks of the agent's last call go, the journal stays.
- (void)dismissAgentMarks {
    if (self.state && self.state->agentJournal.DismissMarks())
        self.state->overlayDirty = true;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    // A gesture's save still on the commit queue reaches the disk before
    // anything that writes the project stops and the lock goes.
    [self drainProjectCommits];
    [self stopVideoOutput];
    if (self.state) self.state->exportCancel.store(true);
    if (self.state && self.state->mediaTasks)
        self.state->mediaTasks->CancelAll();
    if (self.state->audioPlayback) self.state->audioPlayback->Stop();
    if (self.state->sourceAudioPlayback)
        self.state->sourceAudioPlayback->Stop();
    [self.displayTimer invalidate];
    self.displayTimer = nil;
    if (self.timelineShortcutMonitor) {
        [NSEvent removeMonitor:self.timelineShortcutMonitor];
        self.timelineShortcutMonitor = nil;
    }
    for (auto& worker : self.state->workers) {
        worker.second->Stop();
    }
    // AX1 -- no agent reaches the project once the lock is gone: the
    // announcement and the socket go first. Abandoned, not stopped: a
    // connection may be waiting on this very thread.
    if (self.agentEndpoint) self.agentEndpoint->Abandon();
    // Visual QA of 19 September 2026 -- -terminate: exits without running
    // C++ destructors, so ProjectSessionLock's never released the lock on a
    // plain Cmd+Q: its directory stayed next to the project, reclaimed only
    // because its pid was gone. Everything that writes the project has
    // stopped above; the lock goes last.
    if (self.state && self.state->projectLock)
        self.state->projectLock->Release();
#if !defined(CUTMACHINE_UI_SMOKE_TEST)
    // DEC39 -- the editor reopens on the project chosen in the Fichier
    // menu, once this one's lock is gone (ProjectSwitch.h).
    if (self.pendingProjectSwitchPath.length > 0) {
        const std::string projectPath =
            self.pendingProjectSwitchPath.fileSystemRepresentation ?: "";
        const process::Result started =
            process::StartDetached(project_switch::EditorArgumentsFor(
                NSBundle.mainBundle.executablePath.fileSystemRepresentation
                    ?: "",
                projectPath));
        if (!started.Started())
            std::fprintf(stderr,
                         "Project switch: the editor did not start again on "
                         "%s: %s\n",
                         projectPath.c_str(),
                         started.SystemErrorText().c_str());
    }
    engine_log::StopProcessLog();  // PRD35
#endif
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_displayTimer invalidate];
    if (_timelineShortcutMonitor)
        [NSEvent removeMonitor:_timelineShortcutMonitor];
    delete _state;
}

@end

int main(int argc, char* argv[]) {
    // HEADLESS-2026-10 H1 -- docs/roadmap/HEADLESS-2026-10.md. The command
    // line is read by RunCliCommand (CliMain.h), the same reading as the
    // build without a window; this executable supplies what needs AppKit,
    // Core Text or Vision, and opens the editor when no command was given.
    CliHost host;
    // MOTION-2026-09 M3 -- every surface of the model that renders a motion
    // frame (the MCP tool, --call-tool, the app's agent) reaches Core Text
    // and Core Graphics through this renderer (MotionFrameRenderer.h).
    // AGENT-2026-09 A20 -- and a grade read on the face needs Vision.
    // GENERATIF-2026-10 GN1 -- and paid generation reaches fal.ai through
    // NSURLSession, its key through the Keychain (Generation.h).
    host.install_renderers = [] {
        InstallMotionFrameRenderer();
        InstallFaceFinder();
        generation::InstallGenerationTransport();
    };
    host.require_explicit_timeline = [] {
        return static_cast<bool>([[CMUiPreferences sharedPreferences]
            requiresExplicitTimelineForAgentEdits]);
    };
    host.motion_command = &MotionCommand;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    // The window test's own flags are not commands of the product: they
    // skip the command line and open the window.
    const bool harnessFlag =
        argc >= 2 && (std::string(argv[1]) == "--ui-smoke" ||
                      std::string(argv[1]) == "--play-probe");
    if (argc == 2 && std::string(argv[1]) == "--ui-smoke") {
        std::string error;
        if (!PrepareUiSmokeProject(error)) {
            std::string output;
            const int result = FailCliCommand(
                "IoError", "unable to prepare UI smoke project: " + error,
                output, 1);
            std::fwrite(output.data(), 1, output.size(), stdout);
            return result;
        }
        gUiSmokeTesting = true;
    }
    // A package folder stands for its project file (ProjectPathOfPackage).
    std::string packageProject;
    if (argc >= 3 && std::string(argv[1]) == "--play-probe") {
        packageProject = ProjectPathOfPackage(argv[2]);
        argv[2] = packageProject.data();
    }
    if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--play-probe") {
        gUiPlayProbeProject = argv[2];
        gUiPlayProbeSeconds = argc == 4 ? std::atof(argv[3]) : 600.0;
        // PERF13 -- the comparison with every layer composited, as before
        // hidden layers were left out, on the same binary.
        const char* hide = std::getenv("CUTMACHINE_PLAY_PROBE_HIDE");
        gUiPlayProbeHide = hide == nullptr || std::string(hide) != "0";
        const char* readAhead = std::getenv("CUTMACHINE_PLAY_PROBE_READ_AHEAD");
        gUiPlayProbeReadAhead =
            readAhead == nullptr || std::string(readAhead) != "0";
        if (const char* bytes =
                std::getenv("CUTMACHINE_PLAY_PROBE_BYTE_SECONDS"))
            gUiPlayProbeByteSeconds = std::atoll(bytes);
        if (const char* reappear =
                std::getenv("CUTMACHINE_PLAY_PROBE_REAPPEAR_SECONDS"))
            gUiPlayProbeReappearSeconds = std::atoll(reappear);
        if (const char* cut = std::getenv("CUTMACHINE_PLAY_PROBE_CUT_SECONDS"))
            gUiPlayProbeCutSeconds = std::atoll(cut);
        // PERF15 -- CUTMACHINE_PLAY_PROBE_KERNEL_READAHEAD=1 lets the system
        // read the decoders' files ahead as it did before PERF15.
        if (const char* kernel =
                std::getenv("CUTMACHINE_PLAY_PROBE_KERNEL_READAHEAD"))
            MediaSource::SetKernelReadAhead(std::string(kernel) == "1");
        const char* reader = std::getenv("CUTMACHINE_PLAY_PROBE_READER");
        gUiPlayProbeReader = reader == nullptr || std::string(reader) != "0";
        const char* nextPiece = std::getenv("CUTMACHINE_PLAY_PROBE_NEXT_PIECE");
        gUiPlayProbeNextPiece =
            nextPiece == nullptr || std::string(nextPiece) != "0";
        // PERF16 -- CUTMACHINE_PLAY_PROBE_FILL_BEHIND=1 has playback decode
        // the frames behind the one asked for, as before PERF16.
        if (const char* behind =
                std::getenv("CUTMACHINE_PLAY_PROBE_FILL_BEHIND"))
            DecodeWorker::SetPlaybackFillsBehind(std::string(behind) == "1");
    }
    if (harnessFlag && !gUiSmokeTesting && gUiPlayProbeProject.empty()) {
        // The window test's own flag, miscounted.
        std::string output;
        const int result = FailCliCommand(
            "ValidationFailed",
            "these arguments do not fit '" + std::string(argv[1]) + "'", output,
            2);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
#else
    const bool harnessFlag = false;
#endif
    if (!harnessFlag) {
        if (const std::optional<int> status = RunCliCommand(argc, argv, host)) {
            return *status;
        }
    }
    // Installed by RunCliCommand already, unless a harness flag skipped it;
    // installing sets two pointers, twice is harmless.
    host.install_renderers();

    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* documentPath = nil;
#if defined(CUTMACHINE_UI_SMOKE_TEST)
        if (gUiSmokeTesting) {
            documentPath =
                [NSString stringWithUTF8String:gUiSmokeProjectPath.c_str()];
        } else if (!gUiPlayProbeProject.empty()) {
            documentPath =
                [NSString stringWithUTF8String:gUiPlayProbeProject.c_str()];
        } else
#endif
        {
            // A package folder opens the project it holds, as --help says
            // (ProjectFileOf); kept as is, it failed the package check.
            if (argc == 2) {
                const std::string projectFile =
                    project_switch::ProjectFileOf(argv[1]);
                documentPath =
                    [NSString stringWithUTF8String:projectFile.c_str()];
            }
        }
        AppDelegate* delegate =
            [[AppDelegate alloc] initWithDocumentPath:documentPath];
        NSApp.delegate = delegate;
        [NSApp run];
    }
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    if (gUiSmokeTesting) {
        std::error_code ignored;
        std::filesystem::remove_all(gUiSmokeRoot, ignored);
        // PRD28 -- 1 on a failure, 77 (CTest: Skipped) when nothing failed
        // but the screen kept checks from being played, 0 otherwise.
        return ui_smoke::ExitStatus(gUiSmokeFailures, gUiSmokeNotRun);
    }
#endif
    return 0;
}
