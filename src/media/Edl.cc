// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/interchange/src/edl.rs (MIT OR Apache-2.0, commit 09aaf9b). See
// Edl.h for what is carried, what is refused and what is counted.

#include "Edl.h"

#include "Cli.h"
#include "ClipAnimation.h"
#include "ClipSpeed.h"
#include "Json.h"
#include "Multicam.h"
#include "PackageFiles.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "ResolveXmlExport.h"
#include "TimelineView.h"
#include "TransitionRender.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

namespace {

using mcp_json::Value;

// CMX 3600 counts events in three digits and dissolve lengths in three.
constexpr int64_t kMaxEdlEvents = 999;
constexpr int64_t kMaxEdlTransitionFrames = 999;
// M2 writes a rate as ddd.d frames per second.
constexpr int64_t kMaxM2Tenths = 9999;
constexpr const char* kAuxiliaryReel = "AX";
constexpr const char* kBlackReel = "BL";
constexpr const char* kMarkerReel = "001";

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0)
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
        --end;
    return text.substr(begin, end - begin);
}

std::string Upper(std::string text) {
    for (char& character : text)
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
    return text;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

// A title or a clip name stays on its line: a line break would start an
// unparseable event.
std::string OneLine(const std::string& text) {
    std::string out = text;
    for (char& character : out)
        if (character == '\n' || character == '\r') character = ' ';
    return out;
}

std::string Pad(const std::string& text, size_t width) {
    return text.size() >= width ? text
                                : text + std::string(width - text.size(), ' ');
}

std::string Digits3(int64_t value) {
    std::ostringstream out;
    out << std::setw(3) << std::setfill('0') << value;
    return out.str();
}

// p/q, reduced, q > 0.
struct Fraction {
    __int128 num = 1;
    __int128 den = 1;
};

__int128 Gcd(__int128 a, __int128 b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        const __int128 t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

Fraction Reduced(__int128 num, __int128 den) {
    if (den < 0) {
        num = -num;
        den = -den;
    }
    const __int128 g = Gcd(num, den);
    return {num / g, den / g};
}

bool SameRate(const MediaRate& left, const MediaRate& right) {
    return left.num > 0 && left.den > 0 && right.num > 0 && right.den > 0 &&
           static_cast<int64_t>(left.num) * right.den ==
               static_cast<int64_t>(right.num) * left.den;
}

bool IsUnit(const Fraction& speed) { return speed.num == speed.den; }

// The M2 rate, in tenths of a frame per second, when `speed` times the base
// is a whole number of them.
bool M2Tenths(const Fraction& speed, int64_t base, int64_t& tenths) {
    const __int128 numerator = speed.num * base * 10;
    if (numerator % speed.den != 0) return false;
    const __int128 value = numerator / speed.den;
    if (value < 0 || value > kMaxM2Tenths) return false;
    tenths = static_cast<int64_t>(value);
    return true;
}

std::string FormatM2Rate(int64_t tenths) {
    return Digits3(tenths / 10) + "." + std::to_string(tenths % 10);
}

// The colour tables. Hand-picked values; Yellow and Blue are the document's
// own default and the window's marker colour (TimelineView.h), so the two
// colours a montage holds most come back exactly.
const std::vector<EdlMarkerColor>& LocColors() {
    static const std::vector<EdlMarkerColor> kColors = {
        {"RED", "#ff4d4d"},   {"GREEN", "#4caf50"},   {"BLUE", "#4da3ff"},
        {"CYAN", "#3fd0d4"},  {"MAGENTA", "#d050c8"}, {"YELLOW", "#f5c542"},
        {"WHITE", "#ffffff"}, {"BLACK", "#000000"},
    };
    return kColors;
}

const std::vector<EdlMarkerColor>& ResolveColors() {
    static const std::vector<EdlMarkerColor> kColors = {
        {"Blue", "#4da3ff"},     {"Cyan", "#3fd0d4"},    {"Green", "#4caf50"},
        {"Yellow", "#f5c542"},   {"Red", "#ff4d4d"},     {"Pink", "#ff7fbf"},
        {"Purple", "#9a5cd0"},   {"Fuchsia", "#d050c8"}, {"Rose", "#ff9aa8"},
        {"Lavender", "#b9a6e8"}, {"Sky", "#8fd3ff"},     {"Mint", "#8fe0b8"},
        {"Lemon", "#f0f07a"},    {"Sand", "#d2b48c"},    {"Cocoa", "#8b5a3c"},
        {"Cream", "#f5ecd0"},
    };
    return kColors;
}

// The document colour of a word read in either style, case aside.
bool ColorOfWord(const std::string& word, std::string& hex) {
    const std::string upper = Upper(word);
    for (const auto* table : {&LocColors(), &ResolveColors()}) {
        for (const EdlMarkerColor& color : *table) {
            if (Upper(color.word) == upper) {
                hex = color.hex;
                return true;
            }
        }
    }
    return false;
}

std::string KnownColorWords() {
    std::set<std::string> words;
    std::string out;
    for (const auto* table : {&LocColors(), &ResolveColors()}) {
        for (const EdlMarkerColor& color : *table) {
            if (!words.insert(Upper(color.word)).second) continue;
            if (!out.empty()) out += ", ";
            out += color.word;
        }
    }
    return out;
}

// The word for `hex` in `table`: the exact one, or the nearest, `exact`
// saying which.
bool WordOfColor(const std::vector<EdlMarkerColor>& table,
                 const std::string& hex, std::string& word, bool& exact,
                 std::string& error) {
    TransitionColor color;
    if (!ParseTransitionColor(hex, color, error)) return false;
    int64_t best = -1;
    for (const EdlMarkerColor& entry : table) {
        TransitionColor candidate;
        std::string ignored;
        ParseTransitionColor(entry.hex, candidate, ignored);
        const int64_t dr = color.red - candidate.red;
        const int64_t dg = color.green - candidate.green;
        const int64_t db = color.blue - candidate.blue;
        const int64_t distance = dr * dr + dg * dg + db * db;
        if (best < 0 || distance < best) {
            best = distance;
            word = entry.word;
        }
    }
    exact = best == 0;
    return true;
}

// One line of an event: a source and its span on both sides, in frames.
// Record frames are the timeline's, the start timecode added on writing.
struct EdlLine {
    std::string reel;
    std::string transition = "C";
    int64_t duration = -1;
    int64_t source_in = 0;
    int64_t source_out = 0;
    int64_t record_in = 0;
    int64_t record_out = 0;
};

struct EdlM2 {
    int64_t tenths = 0;
    int64_t entry = 0;
};

struct EdlEvent {
    std::vector<EdlLine> lines;
    std::vector<EdlM2> speeds;
    std::vector<std::string> comments;
};

struct Writer {
    EdlTimecodeFormat format;
    int64_t start = 0;

    bool Tc(int64_t frames, std::string& text, std::string& error) const {
        return FormatEdlTimecode(frames, format, text, error);
    }

    bool Header(const std::string& title, std::ostringstream& out) const {
        out << "TITLE: " << OneLine(title) << "\n"
            << (format.drop_frame ? "FCM: DROP FRAME" : "FCM: NON-DROP FRAME")
            << "\n";
        return true;
    }

    bool Event(int64_t number, const EdlEvent& event, std::ostringstream& out,
               std::string& error) const {
        out << "\n";
        for (const EdlLine& line : event.lines) {
            std::string s0, s1, r0, r1;
            if (!Tc(line.source_in, s0, error) ||
                !Tc(line.source_out, s1, error) ||
                !Tc(start + line.record_in, r0, error) ||
                !Tc(start + line.record_out, r1, error))
                return false;
            out << Digits3(number) << "  " << Pad(line.reel, 8) << " "
                << Pad("V", 5) << " " << Pad(line.transition, 4) << " "
                << (line.duration < 0 ? "   " : Digits3(line.duration)) << " "
                << s0 << " " << s1 << " " << r0 << " " << r1 << "\n";
        }
        for (const EdlM2& speed : event.speeds) {
            std::string entry;
            if (!Tc(speed.entry, entry, error)) return false;
            out << "M2   " << Pad(kAuxiliaryReel, 8) << " "
                << FormatM2Rate(speed.tenths) << "                " << entry
                << "\n";
        }
        for (const std::string& comment : event.comments)
            out << "* " << comment << "\n";
        return true;
    }
};

// What one clip of one list is, once checked.
struct PlacedClip {
    const DocumentClip* clip = nullptr;
    const LibraryMedia* media = nullptr;
    int64_t record_in = 0;
    int64_t record_out = 0;
    int64_t source_in = 0;
    Fraction speed;
    bool ramp = false;
    const DocumentTransition* incoming = nullptr;
    TransitionSpan incoming_span;
    const DocumentTransition* outgoing = nullptr;
    TransitionSpan outgoing_span;
};

// The source frame `record` shows, at the clip's constant speed. Refused
// when it falls inside a source frame or before the media.
bool SourceFrameAt(const PlacedClip& placed, int64_t record, int64_t& frame,
                   std::string& error) {
    const __int128 offset =
        static_cast<__int128>(record - placed.record_in) * placed.speed.num;
    if (offset % placed.speed.den != 0) {
        error = "clip '" + placed.clip->id +
                "' shows no whole source frame at its transition's edge at "
                "speed " +
                std::to_string(placed.clip->speed.num) + "/" +
                std::to_string(placed.clip->speed.den);
        return false;
    }
    const __int128 value = placed.source_in + offset / placed.speed.den;
    if (value < 0) {
        error = "clip '" + placed.clip->id +
                "' has no media before its head for the dissolve to read";
        return false;
    }
    frame = static_cast<int64_t>(value);
    return true;
}

bool SpeedLine(const PlacedClip& placed, int64_t base, int64_t entry,
               std::vector<EdlM2>& speeds, std::string& error) {
    if (IsUnit(placed.speed)) return true;
    int64_t tenths = 0;
    if (!M2Tenths(placed.speed, base, tenths)) {
        error = "clip '" + placed.clip->id + "' plays at " +
                (placed.ramp ? "a mean speed of " : "speed ") +
                std::to_string(static_cast<int64_t>(placed.speed.num)) + "/" +
                std::to_string(static_cast<int64_t>(placed.speed.den)) +
                ", which is no whole number of tenths of a frame per second "
                "on a base of " +
                std::to_string(base) +
                ": an M2 line writes ddd.d and the speed would be rounded";
        return false;
    }
    speeds.push_back({tenths, entry});
    return true;
}

std::string ClipName(const PlacedClip& placed) {
    return OneLine(placed.media->filename);
}

Value LossValue(const EdlLoss& loss) {
    Value item = Value::MakeObject();
    item.Set("kind", Value::MakeString(loss.kind));
    item.Set("id", Value::MakeString(loss.id));
    return item;
}

int64_t LossCount(const std::vector<EdlLoss>& losses, const std::string& kind) {
    return std::count_if(
        losses.begin(), losses.end(),
        [&](const EdlLoss& loss) { return loss.kind == kind; });
}

RationalTime FrameTime(int64_t frame, const MediaRate& rate) {
    return {frame * rate.den, rate.num};
}

bool SameColor(const std::string& left, const std::string& right) {
    return Upper(left) == Upper(right);
}

bool LoadTimelineDocument(const std::string& projectPath,
                          const std::string& timelineId, Document& document,
                          std::string& output, int& status) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        status = FailCliCommand("InvalidDocument", error, output);
        return false;
    }
    const Ulid selected =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selected)) {
        status =
            FailCliCommand("UnknownSequence",
                           "unknown timeline_id '" + selected + "'", output);
        return false;
    }
    document = project.MakeDocument(selected);
    return true;
}

}  // namespace

bool EdlTimecodeFormatFor(const MediaRate& rate, EdlTimecodeFormat& format,
                          std::string& error) {
    if (rate.num <= 0 || rate.den <= 0) {
        error = "the sequence has no usable frame rate";
        return false;
    }
    const int64_t g = std::gcd<int64_t>(rate.num, rate.den);
    const int64_t num = rate.num / g;
    const int64_t den = rate.den / g;
    if (den == 1 && num <= 999) {
        format = {num, false};
        return true;
    }
    if (den == 1001 && num % 1000 == 0) {
        const int64_t base = num / 1000;
        if (base == 24 || base == 30 || base == 48 || base == 60) {
            format = {base, base == 30 || base == 60};
            return true;
        }
    }
    error = "the sequence cadence " + std::to_string(num) + "/" +
            std::to_string(den) +
            " has no CMX 3600 timecode: an EDL counts whole frames per "
            "second, or 24, 30, 48 and 60 slowed by 1000/1001";
    return false;
}

bool FormatEdlTimecode(int64_t frames, const EdlTimecodeFormat& format,
                       std::string& text, std::string& error) {
    const int64_t base = format.base;
    if (base <= 0) {
        error = "a timecode needs a positive base";
        return false;
    }
    int64_t labelled = frames;
    if (format.drop_frame) {
        if (base % 30 != 0) {
            error = "drop-frame timecode exists only on a base of 30 or 60";
            return false;
        }
        const int64_t drop = base / 15;
        const int64_t perTen = base * 600 - drop * 9;
        const int64_t perMinute = base * 60 - drop;
        if (frames < 0 || frames >= perTen * 144) {
            error = "frame " + std::to_string(frames) +
                    " has no timecode within 24 hours";
            return false;
        }
        const int64_t tens = frames / perTen;
        const int64_t rest = frames % perTen;
        labelled = frames + drop * 9 * tens +
                   (rest > drop ? drop * ((rest - drop) / perMinute) : 0);
    } else if (frames < 0 || frames >= base * 86400) {
        error = "frame " + std::to_string(frames) +
                " has no timecode within 24 hours";
        return false;
    }
    const int64_t ff = labelled % base;
    const int64_t seconds = labelled / base;
    std::ostringstream out;
    out << std::setfill('0') << std::setw(2) << seconds / 3600 << ':'
        << std::setw(2) << (seconds / 60) % 60 << ':' << std::setw(2)
        << seconds % 60 << (format.drop_frame ? ';' : ':') << std::setw(2)
        << ff;
    text = out.str();
    return true;
}

bool ParseEdlTimecode(const std::string& text, const EdlTimecodeFormat& format,
                      int64_t& frames, std::string& error) {
    const std::string value = Trim(text);
    const auto separator = [](char character) {
        return character == ':' || character == ';' || character == '.' ||
               character == ',';
    };
    int64_t fields[4] = {0, 0, 0, 0};
    size_t field = 0;
    size_t digits = 0;
    bool drop = format.drop_frame;
    for (size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (std::isdigit(static_cast<unsigned char>(character)) != 0) {
            if (++digits > 2) break;
            fields[field] = fields[field] * 10 + (character - '0');
        } else if (separator(character) && digits > 0 && field < 3) {
            if (character == ';' || character == ',') drop = true;
            ++field;
            digits = 0;
        } else {
            digits = 3;
            break;
        }
    }
    if (field != 3 || digits == 0 || digits > 2) {
        error = "'" + value + "' is not a timecode HH:MM:SS:FF";
        return false;
    }
    const int64_t base = format.base;
    const int64_t hours = fields[0];
    const int64_t minutes = fields[1];
    const int64_t seconds = fields[2];
    const int64_t ff = fields[3];
    if (hours > 23 || minutes > 59 || seconds > 59 || ff >= base) {
        error = "'" + value + "' is not a timecode on a base of " +
                std::to_string(base);
        return false;
    }
    int64_t total = ((hours * 60 + minutes) * 60 + seconds) * base + ff;
    if (drop) {
        if (base % 30 != 0) {
            error = "'" + value + "' is drop-frame, which a base of " +
                    std::to_string(base) + " does not have";
            return false;
        }
        const int64_t dropped = base / 15;
        if (seconds == 0 && ff < dropped && minutes % 10 != 0) {
            error = "'" + value +
                    "' names a frame number drop-frame skips at the start "
                    "of a minute";
            return false;
        }
        const int64_t allMinutes = hours * 60 + minutes;
        total -= dropped * (allMinutes - allMinutes / 10);
    }
    frames = total;
    return true;
}

bool BuildEdlExport(const Document& document, const EdlExportOptions& options,
                    EdlExport& exported, std::string& error) {
    EdlExport built;
    built.name = document.sequence.name;
    built.frame_rate = document.sequence.frame_rate;
    built.start_timecode = options.start_timecode;
    if (!EdlTimecodeFormatFor(document.sequence.frame_rate, built.timecode,
                              error))
        return false;
    Writer writer;
    writer.format = built.timecode;
    if (!ParseEdlTimecode(options.start_timecode, built.timecode, writer.start,
                          error)) {
        error = "start timecode: " + error;
        return false;
    }
    const MediaRate rate = document.sequence.frame_rate;
    const int64_t base = built.timecode.base;

    // Every video track takes its number, hidden or not, so that a file's
    // V<n> is the V<n> the window and the Resolve XML give the same track.
    std::vector<const DocumentTrack*> videoTracks;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "video") {
            videoTracks.push_back(&track);
        } else if (track.kind == "audio") {
            built.losses.push_back({"audio_track", track.id});
        } else if (track.kind == "caption") {
            built.losses.push_back({"caption_track", track.id});
        }
    }
    std::stable_sort(videoTracks.begin(), videoTracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index < right->index;
                     });
    const std::set<Ulid> hidden = MulticamHiddenClipIds(document.sequence);

    int32_t number = 0;
    for (const DocumentTrack* track : videoTracks) {
        ++number;
        if (!track->visible) {
            built.losses.push_back({"hidden_track", track->id});
            continue;
        }
        std::vector<PlacedClip> placed;
        std::set<Ulid> skipped;
        for (const DocumentClip& clip : track->clips) {
            // Out of lot (29 October 2026) -- a disabled clip is not in the
            // film (ClipEnabled.h), and a CMX list cannot say "disabled".
            if (!clip.enabled) {
                built.losses.push_back({"disabled_clip", clip.id});
                skipped.insert(clip.id);
                continue;
            }
            if (hidden.count(clip.id) != 0) {
                built.losses.push_back({"multicam_angle", clip.id});
                skipped.insert(clip.id);
                continue;
            }
            if (IsMotionClip(clip)) {
                built.losses.push_back({"motion_clip", clip.id});
                skipped.insert(clip.id);
                continue;
            }
            PlacedClip entry;
            entry.clip = &clip;
            for (const LibraryMedia& media : document.library)
                if (media.id == clip.source_id) entry.media = &media;
            if (entry.media == nullptr) {
                error = "clip '" + clip.id + "' references media '" +
                        clip.source_id + "' that is not in the library";
                return false;
            }
            if (!SameRate(entry.media->rate, rate)) {
                error =
                    "clip '" + clip.id + "' reads '" + entry.media->filename +
                    "' at " + std::to_string(entry.media->rate.num) + "/" +
                    std::to_string(entry.media->rate.den) +
                    " frames per second in a sequence at " +
                    std::to_string(rate.num) + "/" + std::to_string(rate.den) +
                    "; a CMX 3600 list counts source and record in one "
                    "timecode base";
                return false;
            }
            int64_t duration = 0;
            if (!ExactFrameCount(clip.timeline_in, rate, entry.record_in) ||
                !ExactFrameCount(clip.duration, rate, duration) ||
                duration <= 0) {
                error = "clip '" + clip.id +
                        "' does not start and end on whole sequence frames; "
                        "listing it would move the cut";
                return false;
            }
            entry.record_out = entry.record_in + duration;
            if (!ExactFrameCount(clip.source_in, entry.media->rate,
                                 entry.source_in) ||
                entry.source_in < 0) {
                error = "clip '" + clip.id +
                        "' opens inside a source frame of '" +
                        entry.media->filename + "'";
                return false;
            }
            if (ClipIsFreeze(clip)) {
                error = "clip '" + clip.id +
                        "' holds a frame (speed 0/1), which this list does "
                        "not carry yet";
                return false;
            }
            if (ClipHasSpeedRamp(clip)) {
                // The cut points stay exact: the clip reads the source span
                // the ramp reads, at its mean speed. The curve is the loss.
                RationalTime span;
                try {
                    span = ClipSourceSpan(clip);
                } catch (const std::exception& exception) {
                    error = "clip '" + clip.id +
                            "' ramps over a source span with no exact "
                            "spelling: " +
                            exception.what();
                    return false;
                }
                entry.speed = Reduced(
                    static_cast<__int128>(span.value) * clip.duration.rate,
                    static_cast<__int128>(span.rate) * clip.duration.value);
                entry.ramp = true;
                built.losses.push_back({"speed_ramp", clip.id});
            } else {
                entry.speed = Reduced(clip.speed.num, clip.speed.den);
            }
            if (entry.speed.num <= 0) {
                error = "clip '" + clip.id + "' plays no source forward";
                return false;
            }
            if (!clip.effects.empty())
                built.losses.push_back({"grade", clip.id});
            if (ClipHasFraming(clip) ||
                ClipAnimates(clip, ClipAnimatedParameter::FramingScale) ||
                ClipAnimates(clip, ClipAnimatedParameter::FramingPositionX) ||
                ClipAnimates(clip, ClipAnimatedParameter::FramingPositionY))
                built.losses.push_back({"framing", clip.id});
            if (clip.opacity.num != clip.opacity.den ||
                ClipAnimates(clip, ClipAnimatedParameter::Opacity))
                built.losses.push_back({"opacity", clip.id});
            // EF3 -- docs/roadmap/EFFETS-2026-10.md. A CMX 3600 event has
            // no effect field: the blur stays behind, said by clip.
            if (ClipHasBlur(clip)) built.losses.push_back({"blur", clip.id});
            // PA8 -- docs/roadmap/PARITE-2026-11.md. Nor a mask.
            if (ClipHasMask(clip)) built.losses.push_back({"mask", clip.id});
            placed.push_back(entry);
        }
        if (placed.empty()) continue;
        std::stable_sort(placed.begin(), placed.end(),
                         [](const PlacedClip& left, const PlacedClip& right) {
                             return left.record_in < right.record_in;
                         });
        std::map<Ulid, size_t> byId;
        for (size_t index = 0; index < placed.size(); ++index)
            byId[placed[index].clip->id] = index;

        for (const DocumentTransition& transition :
             document.sequence.transitions) {
            if (transition.track_id != track->id) continue;
            if (skipped.count(transition.left_clip_id) != 0 ||
                skipped.count(transition.right_clip_id) != 0) {
                error = "transition '" + transition.id +
                        "' touches a clip this list does not carry (a motion "
                        "clip or a hidden multicam angle)";
                return false;
            }
            TransitionSpan span;
            std::string spanError;
            if (!ResolveTransitionSpan(document.sequence, transition, span,
                                       spanError)) {
                error = "transition '" + transition.id + "' " + spanError;
                return false;
            }
            if (span.frames() <= 0 || span.frames() > kMaxEdlTransitionFrames) {
                error = "transition '" + transition.id + "' lasts " +
                        std::to_string(span.frames()) +
                        " frames; a CMX 3600 dissolve counts 1 to 999";
                return false;
            }
            const bool edge = TransitionIsAtTrackEdge(transition);
            if (TransitionIsColorDip(transition)) {
                TransitionColor color;
                if (!ParseTransitionColor(transition.color, color, error)) {
                    error = "transition '" + transition.id + "' " + error;
                    return false;
                }
                if (!edge) {
                    error = "transition '" + transition.id +
                            "' dips to a colour between two clips; an EDL "
                            "writes a fade only from or to black, at the "
                            "edge of a track";
                    return false;
                }
                if (!(color == TransitionColor{0, 0, 0})) {
                    error = "transition '" + transition.id + "' fades to '" +
                            transition.color +
                            "'; an EDL fades only from or to black (BL)";
                    return false;
                }
            } else if (edge) {
                error = "transition '" + transition.id +
                        "' dissolves at the edge of a track";
                return false;
            }
            // An edge fade lies on the clip it covers (Document::Validate):
            // nothing before the film's first frame or past its last.
            if ((transition.left_clip_id.empty() && span.pre_frames != 0) ||
                (transition.right_clip_id.empty() && span.post_frames != 0)) {
                error = "transition '" + transition.id +
                        "' fades past the edge of its track; an opening is "
                        "aligned start_at_cut and a closing end_at_cut";
                return false;
            }
            const auto left = byId.find(transition.left_clip_id);
            const auto right = byId.find(transition.right_clip_id);
            if ((!transition.left_clip_id.empty() && left == byId.end()) ||
                (!transition.right_clip_id.empty() && right == byId.end())) {
                error = "transition '" + transition.id +
                        "' names a clip that is not on its track";
                return false;
            }
            for (const auto& side : {left, right}) {
                if (side != byId.end() && placed[side->second].ramp) {
                    error = "transition '" + transition.id +
                            "' touches clip '" + side->first +
                            "', which ramps its speed; a list written at its "
                            "mean speed cannot say what the transition reads";
                    return false;
                }
            }
            if (left != byId.end()) {
                placed[left->second].outgoing = &transition;
                placed[left->second].outgoing_span = span;
            }
            if (right != byId.end()) {
                placed[right->second].incoming = &transition;
                placed[right->second].incoming_span = span;
            }
            ++built.transitions;
        }

        std::vector<EdlEvent> events;
        EdlTrackFile file;
        file.track_id = track->id;
        file.number = number;
        for (const PlacedClip& entry : placed) {
            ++file.clips;
            ++built.clips;
            if (!IsUnit(entry.speed)) ++built.retimed_clips;
            int64_t recordIn = entry.record_in;
            int64_t recordOut = entry.record_out;
            if (entry.outgoing != nullptr)
                recordOut = entry.outgoing_span.first_frame();
            EdlEvent event;
            if (entry.incoming != nullptr) {
                const TransitionSpan& span = entry.incoming_span;
                recordIn = span.first_frame();
                EdlLine from;
                if (entry.incoming->left_clip_id.empty()) {
                    from.reel = kBlackReel;
                } else {
                    const PlacedClip& outgoing =
                        placed[byId.at(entry.incoming->left_clip_id)];
                    if (!IsUnit(outgoing.speed)) {
                        error = "clip '" + outgoing.clip->id +
                                "' is retimed and dissolves into clip '" +
                                entry.clip->id +
                                "'; with every reel AX, the list cannot say "
                                "which side an M2 line speaks for";
                        return false;
                    }
                    int64_t frame = 0;
                    if (!SourceFrameAt(outgoing, recordIn, frame, error))
                        return false;
                    from.reel = kAuxiliaryReel;
                    from.source_in = frame;
                    from.source_out = frame;
                    event.comments.push_back("FROM CLIP NAME: " +
                                             ClipName(outgoing));
                }
                from.record_in = recordIn;
                from.record_out = recordIn;
                EdlLine to;
                to.reel = kAuxiliaryReel;
                to.transition = "D";
                to.duration = span.frames();
                if (!SourceFrameAt(entry, recordIn, to.source_in, error))
                    return false;
                to.source_out = to.source_in + (recordOut - recordIn);
                to.record_in = recordIn;
                to.record_out = recordOut;
                event.lines = {from, to};
                event.comments.push_back("TO CLIP NAME: " + ClipName(entry));
                if (!SpeedLine(entry, base, to.source_in, event.speeds, error))
                    return false;
            } else {
                EdlLine line;
                line.reel = kAuxiliaryReel;
                line.source_in = entry.source_in;
                line.source_out = entry.source_in + (recordOut - recordIn);
                line.record_in = recordIn;
                line.record_out = recordOut;
                event.lines = {line};
                event.comments.push_back("FROM CLIP NAME: " + ClipName(entry));
                if (!SpeedLine(entry, base, line.source_in, event.speeds,
                               error))
                    return false;
            }
            events.push_back(event);
            if (entry.outgoing != nullptr &&
                entry.outgoing->right_clip_id.empty()) {
                // A closing fade: the clip's last frames dissolve to black.
                const TransitionSpan& span = entry.outgoing_span;
                EdlEvent fade;
                EdlLine from;
                from.reel = kAuxiliaryReel;
                if (!SourceFrameAt(entry, span.first_frame(), from.source_in,
                                   error))
                    return false;
                from.source_out = from.source_in;
                from.record_in = span.first_frame();
                from.record_out = span.first_frame();
                EdlLine black;
                black.reel = kBlackReel;
                black.transition = "D";
                black.duration = span.frames();
                black.source_in = 0;
                black.source_out = span.frames();
                black.record_in = span.first_frame();
                black.record_out = span.end_frame();
                fade.lines = {from, black};
                fade.comments.push_back("FROM CLIP NAME: " + ClipName(entry));
                if (!SpeedLine(entry, base, from.source_in, fade.speeds, error))
                    return false;
                events.push_back(fade);
            }
        }
        if (static_cast<int64_t>(events.size()) > kMaxEdlEvents) {
            error = "video track V" + std::to_string(number) + " needs " +
                    std::to_string(events.size()) +
                    " events; a CMX 3600 list numbers 999";
            return false;
        }
        std::ostringstream out;
        writer.Header(built.name + " V" + std::to_string(number), out);
        for (size_t index = 0; index < events.size(); ++index) {
            if (!writer.Event(static_cast<int64_t>(index + 1), events[index],
                              out, error)) {
                error = "video track V" + std::to_string(number) + ": " + error;
                return false;
            }
        }
        file.events = static_cast<int32_t>(events.size());
        file.text = out.str();
        built.files.push_back(std::move(file));
    }
    if (built.files.empty()) {
        error = "the timeline has no video clip to list";
        return false;
    }
    if (!document.sequence.look_lut.digest.empty())
        built.losses.push_back({"look_lut", document.sequence.id});
    if (!document.sequence.markers.empty())
        built.losses.push_back({"markers", document.sequence.id});
    exported = std::move(built);
    error.clear();
    return true;
}

bool EdlTrackPaths(const std::string& path, const EdlExport& exported,
                   std::vector<std::string>& paths, std::string& error) {
    const std::filesystem::path target(path);
    if (Upper(target.extension().string()) != ".EDL" || target.stem().empty()) {
        error = "'" + path + "' does not name a .edl file";
        return false;
    }
    paths.clear();
    for (const EdlTrackFile& file : exported.files) {
        paths.push_back(
            (target.parent_path() / (target.stem().string() + "_V" +
                                     std::to_string(file.number) + ".edl"))
                .string());
    }
    return true;
}

bool ExportEdlFiles(const Document& document, const std::string& path,
                    const EdlExportOptions& options, std::string& reportJson,
                    std::string& errorName, std::string& error) {
    EdlExport exported;
    if (!BuildEdlExport(document, options, exported, error)) {
        errorName = "InvalidExport";
        return false;
    }
    std::vector<std::string> paths;
    if (!EdlTrackPaths(path, exported, paths, error)) {
        errorName = "ValidationFailed";
        return false;
    }
    // Every list or none: a montage half sent is worse than one refused.
    for (const std::string& target : paths) {
        std::error_code status;
        if (std::filesystem::exists(
                std::filesystem::symlink_status(target, status))) {
            errorName = "OutputExists";
            error = "'" + target + "' already exists";
            return false;
        }
    }
    for (size_t index = 0; index < paths.size(); ++index) {
        if (!PublishNewInterchangeFile(paths[index], exported.files[index].text,
                                       errorName, error)) {
            for (size_t written = 0; written < index; ++written) {
                std::error_code ignored;
                std::filesystem::remove(paths[written], ignored);
            }
            return false;
        }
    }

    Value report = Value::MakeObject();
    report.Set("ok", Value::MakeBool(true));
    report.Set("schema", Value::MakeString("cutmachine.edl.v1"));
    report.Set("name", Value::MakeString(exported.name));
    Value frameRate = Value::MakeObject();
    frameRate.Set("num", Value::MakeInt(exported.frame_rate.num));
    frameRate.Set("den", Value::MakeInt(exported.frame_rate.den));
    report.Set("frame_rate", std::move(frameRate));
    Value timecode = Value::MakeObject();
    timecode.Set("base", Value::MakeInt(exported.timecode.base));
    timecode.Set("drop_frame", Value::MakeBool(exported.timecode.drop_frame));
    timecode.Set("start", Value::MakeString(exported.start_timecode));
    report.Set("timecode", std::move(timecode));
    Value files = Value::MakeArray();
    for (size_t index = 0; index < paths.size(); ++index) {
        Value file = Value::MakeObject();
        file.Set("path", Value::MakeString(paths[index]));
        file.Set("track",
                 Value::MakeString(
                     "V" + std::to_string(exported.files[index].number)));
        file.Set("track_id", Value::MakeString(exported.files[index].track_id));
        file.Set("events", Value::MakeInt(exported.files[index].events));
        file.Set("clips", Value::MakeInt(exported.files[index].clips));
        files.Push(std::move(file));
    }
    report.Set("files", std::move(files));
    report.Set("clips", Value::MakeInt(exported.clips));
    report.Set("retimed_clips", Value::MakeInt(exported.retimed_clips));
    report.Set("transitions", Value::MakeInt(exported.transitions));
    const std::vector<std::pair<const char*, const char*>> counts = {
        {"grades_not_sent", "grade"},
        {"framing_not_sent", "framing"},
        {"opacity_not_sent", "opacity"},
        {"blurs_not_sent", "blur"},
        {"masks_not_sent", "mask"},
        {"speed_ramps_flattened", "speed_ramp"},
        {"motion_clips_not_sent", "motion_clip"},
        {"multicam_angles_not_sent", "multicam_angle"},
        {"disabled_clips_not_sent", "disabled_clip"},
        {"audio_tracks_not_sent", "audio_track"},
        {"caption_tracks_not_sent", "caption_track"},
        {"hidden_tracks_not_sent", "hidden_track"},
    };
    for (const auto& [key, kind] : counts)
        report.Set(key, Value::MakeInt(LossCount(exported.losses, kind)));
    report.Set("look_lut_not_sent",
               Value::MakeBool(LossCount(exported.losses, "look_lut") != 0));
    report.Set("markers_not_sent",
               Value::MakeInt(
                   LossCount(exported.losses, "markers") != 0
                       ? static_cast<int64_t>(document.sequence.markers.size())
                       : 0));
    Value losses = Value::MakeArray();
    for (const EdlLoss& loss : exported.losses) losses.Push(LossValue(loss));
    report.Set("losses", std::move(losses));
    report.Set("reel", Value::MakeString(kAuxiliaryReel));
    report.Set("source_timecodes", Value::MakeString("media_time_from_zero"));
    report.Set("verified_in_resolve", Value::MakeBool(false));
    reportJson = report.Dump() + "\n";
    errorName.clear();
    error.clear();
    return true;
}

bool ParseEdlMarkerStyle(const std::string& text, EdlMarkerStyle& style) {
    if (text == "loc") {
        style = EdlMarkerStyle::Loc;
        return true;
    }
    if (text == "resolve") {
        style = EdlMarkerStyle::Resolve;
        return true;
    }
    return false;
}

const std::vector<EdlMarkerColor>& EdlLocMarkerColors() { return LocColors(); }
const std::vector<EdlMarkerColor>& EdlResolveMarkerColors() {
    return ResolveColors();
}

bool EdlResolveMarkerColorWord(const std::string& hex, std::string& word,
                               bool& exact, std::string& error) {
    return WordOfColor(ResolveColors(), hex, word, exact, error);
}

bool BuildEdlMarkers(const Document& document, EdlMarkerStyle style,
                     const std::string& startTimecode,
                     EdlMarkerExport& exported, std::string& error) {
    EdlMarkerExport built;
    Writer writer;
    if (!EdlTimecodeFormatFor(document.sequence.frame_rate, writer.format,
                              error))
        return false;
    if (!ParseEdlTimecode(startTimecode, writer.format, writer.start, error)) {
        error = "start timecode: " + error;
        return false;
    }
    if (document.sequence.markers.empty()) {
        error = "the timeline has no marker to list";
        return false;
    }
    std::vector<const DocumentMarker*> markers;
    for (const DocumentMarker& marker : document.sequence.markers)
        markers.push_back(&marker);
    std::stable_sort(
        markers.begin(), markers.end(),
        [](const DocumentMarker* left, const DocumentMarker* right) {
            return left->time < right->time;
        });
    std::ostringstream out;
    writer.Header(document.sequence.name + " markers", out);
    int64_t number = 0;
    for (const DocumentMarker* marker : markers) {
        int64_t frame = 0;
        int64_t length = 0;
        if (!ExactFrameCount(marker->time, document.sequence.frame_rate,
                             frame) ||
            !ExactFrameCount(marker->duration, document.sequence.frame_rate,
                             length) ||
            frame < 0 || length < 0) {
            error = "marker '" + marker->id +
                    "' does not sit on whole sequence frames";
            return false;
        }
        if (marker->name.find_first_of("\r\n") != std::string::npos ||
            (style == EdlMarkerStyle::Resolve &&
             marker->name.find('|') != std::string::npos)) {
            error = "marker '" + marker->id + "' has a name holding " +
                    (style == EdlMarkerStyle::Resolve ? "a line break or a '|'"
                                                      : "a line break") +
                    ", which would end its line in the list";
            return false;
        }
        std::string word;
        bool exact = false;
        if (!WordOfColor(
                style == EdlMarkerStyle::Loc ? LocColors() : ResolveColors(),
                marker->color, word, exact, error)) {
            error = "marker '" + marker->id + "': " + error;
            return false;
        }
        if (!exact) built.colors_approximated.push_back(marker->id);
        const int64_t shown = std::max<int64_t>(length, 1);
        EdlEvent event;
        EdlLine line;
        line.reel = kMarkerReel;
        // Resolve's own list repeats the record timecode on the source side.
        line.source_in = writer.start + frame;
        line.source_out = writer.start + frame + shown;
        line.record_in = frame;
        line.record_out = frame + shown;
        event.lines = {line};
        if (!writer.Event(++number, event, out, error)) return false;
        std::string at;
        if (!writer.Tc(writer.start + frame, at, error)) return false;
        if (style == EdlMarkerStyle::Loc) {
            out << "* LOC: " << at << " " << Pad(word, 7) << " " << marker->name
                << "\n";
            if (length > 0) built.durations_not_sent.push_back(marker->id);
        } else {
            out << " |C:ResolveColor" << word << " |M:" << marker->name
                << " |D:" << shown << "\n";
        }
    }
    if (number > kMaxEdlEvents) {
        error = "the timeline holds " + std::to_string(number) +
                " markers; a CMX 3600 list numbers 999 events";
        return false;
    }
    built.markers = static_cast<int32_t>(number);
    built.text = out.str();
    exported = std::move(built);
    error.clear();
    return true;
}

bool ExportEdlMarkersFile(const Document& document, const std::string& path,
                          EdlMarkerStyle style,
                          const std::string& startTimecode,
                          std::string& reportJson, std::string& errorName,
                          std::string& error) {
    if (Upper(std::filesystem::path(path).extension().string()) != ".EDL") {
        errorName = "ValidationFailed";
        error = "'" + path + "' does not name a .edl file";
        return false;
    }
    EdlMarkerExport exported;
    if (!BuildEdlMarkers(document, style, startTimecode, exported, error)) {
        errorName = "InvalidExport";
        return false;
    }
    if (!PublishNewInterchangeFile(path, exported.text, errorName, error))
        return false;
    Value report = Value::MakeObject();
    report.Set("ok", Value::MakeBool(true));
    report.Set("schema", Value::MakeString("cutmachine.edl-markers.v1"));
    report.Set("path", Value::MakeString(path));
    report.Set("style", Value::MakeString(
                            style == EdlMarkerStyle::Loc ? "loc" : "resolve"));
    report.Set("start_timecode", Value::MakeString(startTimecode));
    report.Set("markers", Value::MakeInt(exported.markers));
    Value approximated = Value::MakeArray();
    for (const Ulid& id : exported.colors_approximated)
        approximated.Push(Value::MakeString(id));
    report.Set("colors_approximated", std::move(approximated));
    Value points = Value::MakeArray();
    for (const Ulid& id : exported.durations_not_sent)
        points.Push(Value::MakeString(id));
    report.Set("durations_not_sent", std::move(points));
    report.Set("verified_in_resolve", Value::MakeBool(false));
    reportJson = report.Dump() + "\n";
    errorName.clear();
    error.clear();
    return true;
}

bool ParseEdlMarkers(const std::string& text, const EdlTimecodeFormat& format,
                     int64_t startFrame, std::vector<EdlMarkerEntry>& markers,
                     std::string& error) {
    markers.clear();
    EdlTimecodeFormat current = format;
    // The timeline's start is a label: a list counted in the other FCM mode
    // writes the same label for it, at another frame count. It is read
    // again in the list's mode, or every marker shifts (108 frames an hour
    // at 29.97), and kept with colons only, since a `;` reads drop-frame
    // whatever the FCM line says.
    std::string startLabel;
    if (!FormatEdlTimecode(startFrame, format, startLabel, error)) return false;
    std::replace(startLabel.begin(), startLabel.end(), ';', ':');
    bool haveEvent = false;
    int64_t eventRecord = 0;
    std::istringstream input(text);
    std::string raw;
    size_t lineNumber = 0;
    const auto where = [&]() { return "line " + std::to_string(lineNumber); };
    const auto place = [&](int64_t record, EdlMarkerEntry& entry) {
        // A marker the list leaves unnamed is named as the window names
        // one it adds; the document refuses an empty name.
        if (entry.name.empty()) entry.name = kTimelineMarkerDefaultName;
        if (std::any_of(entry.name.begin(), entry.name.end(),
                        [](unsigned char byte) {
                            return byte < 0x20 || byte == 0x7f;
                        })) {
            error = where() + ": the marker's name holds a control character";
            return false;
        }
        if (record < startFrame) {
            error = where() +
                    ": the marker falls before the timeline's start timecode";
            return false;
        }
        entry.frame = record - startFrame;
        entry.line = lineNumber;
        return true;
    };
    while (std::getline(input, raw)) {
        ++lineNumber;
        if (lineNumber == 1 && StartsWith(raw, "\xEF\xBB\xBF")) raw.erase(0, 3);
        const std::string line = Trim(raw);
        if (line.empty()) continue;
        const std::string upper = Upper(line);
        if (StartsWith(upper, "FCM:")) {
            const bool drop = upper.find("DROP") != std::string::npos &&
                              upper.find("NON") == std::string::npos;
            if (drop && format.base % 30 != 0) {
                error = where() + ": the list is drop-frame, which a base of " +
                        std::to_string(format.base) + " does not have";
                return false;
            }
            current.drop_frame = drop;
            if (!ParseEdlTimecode(startLabel, current, startFrame, error)) {
                error = where() + ": the timeline's start timecode " +
                        startLabel + " does not exist in this count: " + error;
                return false;
            }
            continue;
        }
        if (StartsWith(upper, "*")) {
            const std::string comment = Trim(line.substr(1));
            if (!StartsWith(Upper(comment), "LOC:")) continue;
            std::istringstream fields(Trim(comment.substr(4)));
            std::string timecode;
            std::string word;
            fields >> timecode >> word;
            std::string name;
            std::getline(fields, name);
            EdlMarkerEntry entry;
            int64_t record = 0;
            if (!ParseEdlTimecode(timecode, current, record, error)) {
                error = where() + ": " + error;
                return false;
            }
            if (word.empty() || !ColorOfWord(word, entry.color)) {
                error = where() + ": unknown marker colour '" + word +
                        "' (known: " + KnownColorWords() + ")";
                return false;
            }
            entry.color_word = word;
            entry.name = Trim(name);
            if (!place(record, entry)) return false;
            markers.push_back(std::move(entry));
            continue;
        }
        if (StartsWith(line, "|")) {
            if (!haveEvent) {
                error = where() + ": a marker line under no event";
                return false;
            }
            EdlMarkerEntry entry;
            std::string word;
            int64_t length = 1;
            std::istringstream parts(line);
            std::string part;
            while (std::getline(parts, part, '|')) {
                part = Trim(part);
                if (StartsWith(part, "C:")) {
                    word = Trim(part.substr(2));
                    if (StartsWith(Upper(word), "RESOLVECOLOR"))
                        word = word.substr(12);
                } else if (StartsWith(part, "M:")) {
                    entry.name = Trim(part.substr(2));
                } else if (StartsWith(part, "D:")) {
                    const std::string digits = Trim(part.substr(2));
                    if (digits.empty() || digits.size() > 9 ||
                        !std::all_of(digits.begin(), digits.end(),
                                     [](unsigned char c) {
                                         return std::isdigit(c) != 0;
                                     }) ||
                        std::stoll(digits) <= 0) {
                        error = where() + ": marker length '" + digits +
                                "' is not a positive number of frames";
                        return false;
                    }
                    length = std::stoll(digits);
                }
            }
            if (word.empty() || !ColorOfWord(word, entry.color)) {
                error = where() + ": unknown marker colour '" + word +
                        "' (known: " + KnownColorWords() + ")";
                return false;
            }
            entry.color_word = word;
            // Resolve writes a point as one frame long.
            entry.duration_frames = length == 1 ? 0 : length;
            if (!place(eventRecord, entry)) return false;
            markers.push_back(std::move(entry));
            continue;
        }
        // An event line: a number, then four timecodes at its end.
        std::istringstream tokens(line);
        std::vector<std::string> words;
        for (std::string word; tokens >> word;) words.push_back(word);
        if (words.size() >= 8 &&
            std::all_of(words[0].begin(), words[0].end(),
                        [](unsigned char c) { return std::isdigit(c) != 0; })) {
            // A marker under this event takes its record in: one that does
            // not read is refused here, not left to the previous event.
            int64_t record = 0;
            if (!ParseEdlTimecode(words[words.size() - 2], current, record,
                                  error)) {
                error = where() + ": event record in: " + error;
                return false;
            }
            haveEvent = true;
            eventRecord = record;
        }
    }
    if (markers.empty()) {
        error = "the list holds no marker (`* LOC:` or Resolve `|C:` line)";
        return false;
    }
    error.clear();
    return true;
}

bool BuildEdlMarkerImport(const Document& document, const std::string& text,
                          const std::string& startTimecode,
                          EdlMarkerImport& imported, std::string& error) {
    EdlMarkerImport built;
    EdlTimecodeFormat format;
    if (!EdlTimecodeFormatFor(document.sequence.frame_rate, format, error))
        return false;
    int64_t start = 0;
    if (!ParseEdlTimecode(startTimecode, format, start, error)) {
        error = "start timecode: " + error;
        return false;
    }
    std::vector<EdlMarkerEntry> entries;
    if (!ParseEdlMarkers(text, format, start, entries, error)) return false;
    const MediaRate rate = document.sequence.frame_rate;
    std::vector<DocumentMarker> present = document.sequence.markers;
    for (const EdlMarkerEntry& entry : entries) {
        DocumentMarker marker;
        marker.name = entry.name;
        marker.time = FrameTime(entry.frame, rate);
        marker.duration = entry.duration_frames == 0
                              ? RationalTime{0, 1}
                              : FrameTime(entry.duration_frames, rate);
        marker.color = entry.color;
        marker.category = "note";
        const bool known = std::any_of(
            present.begin(), present.end(), [&](const DocumentMarker& other) {
                return other.time.compare(marker.time) == 0 &&
                       other.duration.compare(marker.duration) == 0 &&
                       other.name == marker.name &&
                       SameColor(other.color, marker.color);
            });
        if (known) {
            ++built.already_present;
            continue;
        }
        present.push_back(marker);
        AddMarkerOperation operation;
        operation.marker = marker;
        built.operations.push_back(operation);
        built.added.push_back(entry);
    }
    if (built.operations.size() > kMaxBatchOperations) {
        error = "the list adds " + std::to_string(built.operations.size()) +
                " markers; one import is one undo step of at most " +
                std::to_string(kMaxBatchOperations);
        return false;
    }
    imported = std::move(built);
    error.clear();
    return true;
}

Operation EdlMarkerImportOperation(EdlMarkerImport& imported) {
    if (imported.operations.size() == 1) return imported.operations.front();
    return BatchOperation{"marqueurs EDL", imported.operations};
}

std::string EdlMarkerImportReportJson(const EdlMarkerImport& imported,
                                      const std::string& path) {
    Value report = Value::MakeObject();
    report.Set("path", Value::MakeString(path));
    report.Set("added",
               Value::MakeInt(static_cast<int64_t>(imported.added.size())));
    report.Set("already_present",
               Value::MakeInt(static_cast<int64_t>(imported.already_present)));
    Value markers = Value::MakeArray();
    for (const EdlMarkerEntry& entry : imported.added) {
        Value item = Value::MakeObject();
        item.Set("name", Value::MakeString(entry.name));
        item.Set("frame", Value::MakeInt(entry.frame));
        item.Set("duration_frames", Value::MakeInt(entry.duration_frames));
        item.Set("color_word", Value::MakeString(entry.color_word));
        item.Set("color", Value::MakeString(entry.color));
        item.Set("line", Value::MakeInt(static_cast<int64_t>(entry.line)));
        markers.Push(std::move(item));
    }
    report.Set("markers", std::move(markers));
    report.Set("verified_in_resolve", Value::MakeBool(false));
    return report.Dump();
}

int ExportEdlCommand(const std::string& projectPath, const std::string& path,
                     std::string& output, const std::string& timelineId,
                     const std::string& startTimecode) {
    Document document;
    int status = 0;
    if (!LoadTimelineDocument(projectPath, timelineId, document, output,
                              status))
        return status;
    EdlExportOptions options;
    options.start_timecode = startTimecode;
    std::string errorName;
    std::string error;
    if (!ExportEdlFiles(document, path, options, output, errorName, error))
        return FailCliCommand(errorName, error, output);
    return 0;
}

int ExportEdlMarkersCommand(const std::string& projectPath,
                            const std::string& path, std::string& output,
                            const std::string& timelineId, EdlMarkerStyle style,
                            const std::string& startTimecode) {
    Document document;
    int status = 0;
    if (!LoadTimelineDocument(projectPath, timelineId, document, output,
                              status))
        return status;
    std::string errorName;
    std::string error;
    if (!ExportEdlMarkersFile(document, path, style, startTimecode, output,
                              errorName, error))
        return FailCliCommand(errorName, error, output);
    return 0;
}

int ImportEdlMarkersCommand(const std::string& projectPath,
                            const std::string& edlPath, std::string& output,
                            const std::string& timelineId,
                            const std::string& startTimecode) {
    Document document;
    int status = 0;
    if (!LoadTimelineDocument(projectPath, timelineId, document, output,
                              status))
        return status;
    std::string text;
    std::string error;
    if (!ReadTextFile(edlPath, text, error))
        return FailCliCommand("IoError", error, output);
    EdlMarkerImport imported;
    if (!BuildEdlMarkerImport(document, text, startTimecode, imported, error))
        return FailCliCommand("ValidationFailed", error, output);
    Value report;
    std::string parseError;
    Value::Parse(EdlMarkerImportReportJson(imported, edlPath), report,
                 parseError);
    if (imported.operations.empty()) {
        Value result = Value::MakeObject();
        result.Set("ok", Value::MakeBool(true));
        result.Set("applied", Value::MakeBool(false));
        result.Set("edl_markers", std::move(report));
        output = result.Dump() + "\n";
        return 0;
    }
    std::string applied;
    const int result = ApplyOperationCommand(
        projectPath, SerializeOperation(EdlMarkerImportOperation(imported)),
        applied, timelineId);
    if (result != 0) {
        output = applied;
        return result;
    }
    Value answer;
    if (!Value::Parse(applied, answer, parseError) || !answer.IsObject()) {
        output = applied;
        return result;
    }
    answer.Set("edl_markers", std::move(report));
    output = answer.Dump() + "\n";
    return 0;
}
