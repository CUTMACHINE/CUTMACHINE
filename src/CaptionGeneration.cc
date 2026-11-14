#include "CaptionGeneration.h"
#include "CaptionBreakModel.h"
#include "CaptionLayout.h"
#include "CaptionRenderer.h"
#include "ClipEnabled.h"
#include "ClipSpeed.h"

#include "FfmpegPipe.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/parseutils.h>
}
#include "Json.h"
#include "Subtitles.h"
#include "Timeline.h"
#include "Ulid.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace {

using mcp_json::Value;

int64_t FloorFrames(const RationalTime& time, MediaRate rate) {
    return time.to_frames(rate.num, rate.den);
}

RationalTime FrameTime(int64_t frame, MediaRate rate) {
    return {frame * rate.den, rate.num};
}

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

}  // namespace

std::string CaptionFontName(const CaptionStyle& style) {
    return style.font_family == "system" ? "Helvetica Neue" : style.font_family;
}

bool ParseCaptionColor(const std::string& text, std::array<uint8_t, 4>& rgba) {
    if (text.empty() || text.find('\0') != std::string::npos) return false;
    return av_parse_color(rgba.data(), text.c_str(), -1, nullptr) >= 0;
}

bool MeasureCaptionTextWidths(const CaptionStyle& style,
                              const DocumentSequence& sequence,
                              const std::vector<std::string>& texts,
                              std::vector<int64_t>& widths,
                              std::string& error) {
    widths.assign(texts.size(), 0);
    error.clear();
    if (texts.empty()) return true;
    if (style.font_family.empty() || style.font_size <= 0 ||
        sequence.width <= 0) {
        error = "caption style has no measurable font";
        return false;
    }
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    if (renderer == nullptr) {
        error = kNoCaptionRenderer;
        return false;
    }
    // LG2 -- docs/roadmap/LEGENDES-2026-10.md. Measured by the renderer
    // that draws the carton, every line of every text in one call: a text
    // is as wide as its widest line.
    std::vector<std::string> lines;
    std::vector<size_t> owners;
    for (size_t index = 0; index < texts.size(); ++index) {
        if (texts[index].find('\0') != std::string::npos) {
            error = "caption text contains NUL";
            return false;
        }
        for (std::string& line : caption_layout::SplitLines(texts[index])) {
            lines.push_back(std::move(line));
            owners.push_back(index);
        }
    }
    std::vector<double> advances;
    if (!renderer->measure(style, lines, advances, error)) {
        error = "unable to measure caption widths: " + error;
        return false;
    }
    if (advances.size() != lines.size()) {
        error = "caption measurement answered too few lines";
        return false;
    }
    for (size_t index = 0; index < lines.size(); ++index) {
        widths[owners[index]] =
            std::max(widths[owners[index]],
                     caption_layout::MeasuredWidth(advances[index], style));
    }
    return true;
}

int64_t CaptionMaximumLineWidth(int32_t sequenceWidth) {
    return (static_cast<int64_t>(std::max<int32_t>(0, sequenceWidth)) * 9) / 10;
}

std::string WrapCaptionLines(
    const std::string& text, int64_t maximumWidth,
    const std::function<int64_t(const std::string&)>& widthOf) {
    std::string wrapped;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const size_t lineEnd =
            std::min(text.find('\n', lineStart), text.size());
        const std::string line = text.substr(lineStart, lineEnd - lineStart);
        if (!wrapped.empty() || lineStart > 0) wrapped += '\n';
        if (widthOf(line) <= maximumWidth) {
            wrapped += line;
        } else {
            std::vector<std::string> words;
            std::istringstream stream(line);
            for (std::string word; stream >> word;) words.push_back(word);
            std::string current;
            bool first = true;
            for (const std::string& word : words) {
                const std::string candidate =
                    current.empty() ? word : current + ' ' + word;
                if (!current.empty() && widthOf(candidate) > maximumWidth) {
                    wrapped += (first ? "" : "\n") + current;
                    first = false;
                    current = word;
                } else {
                    current = candidate;
                }
            }
            wrapped += (first ? "" : "\n") + current;
        }
        if (lineEnd >= text.size()) break;
        lineStart = lineEnd + 1;
    }
    return wrapped;
}

namespace {

size_t CodePoints(const std::string& text) {
    return static_cast<size_t>(std::count_if(
        text.begin(), text.end(),
        [](unsigned char byte) { return (byte & 0xC0) != 0x80; }));
}

}  // namespace

bool WrapCaptionTextsForDelivery(const std::vector<CaptionStyle>& styles,
                                 const DocumentSequence& sequence,
                                 std::vector<std::string>& texts,
                                 std::string& error) {
    error.clear();
    const int64_t limit = CaptionMaximumLineWidth(sequence.width);
    if (styles.size() != texts.size() || limit <= 0) return true;
    // Texts that measure alike -- same font, size and decoration -- are
    // measured together.
    std::map<size_t, std::vector<size_t>> byStyle;
    for (size_t index = 0; index < texts.size(); ++index) {
        bool assigned = false;
        for (auto& [first, members] : byStyle) {
            const CaptionStyle& a = styles[first];
            const CaptionStyle& b = styles[index];
            if (a.font_family == b.font_family && a.font_size == b.font_size &&
                a.font_weight == b.font_weight &&
                a.outline_width == b.outline_width &&
                a.shadow_offset == b.shadow_offset) {
                members.push_back(index);
                assigned = true;
                break;
            }
        }
        if (!assigned) byStyle[index] = {index};
    }
    for (const auto& [first, members] : byStyle) {
        const CaptionStyle& style = styles[first];
        // LG2 -- every line is measured; only a line too wide has its word
        // ranges measured, to find where it breaks.
        std::vector<std::string> lines;
        std::set<std::string> seen;
        for (const size_t index : members) {
            for (const std::string& line :
                 caption_layout::SplitLines(texts[index])) {
                if (seen.insert(line).second) lines.push_back(line);
            }
        }
        std::vector<int64_t> widths;
        if (!MeasureCaptionTextWidths(style, sequence, lines, widths, error)) {
            return false;
        }
        std::map<std::string, int64_t> measured;
        std::vector<std::string> ranges;
        for (size_t index = 0; index < lines.size(); ++index) {
            measured[lines[index]] = widths[index];
            if (widths[index] <= limit) continue;
            std::vector<std::string> words;
            std::istringstream stream(lines[index]);
            for (std::string word; stream >> word;) words.push_back(word);
            for (size_t from = 0; from < words.size(); ++from) {
                std::string range;
                for (size_t to = from; to < words.size(); ++to) {
                    range += (to == from ? "" : " ") + words[to];
                    if (seen.insert(range).second) ranges.push_back(range);
                }
            }
        }
        if (!ranges.empty()) {
            if (!MeasureCaptionTextWidths(style, sequence, ranges, widths,
                                          error)) {
                return false;
            }
            for (size_t index = 0; index < ranges.size(); ++index) {
                measured[ranges[index]] = widths[index];
            }
        }
        for (const size_t index : members) {
            texts[index] = WrapCaptionLines(
                texts[index], limit, [&](const std::string& line) {
                    const auto found = measured.find(line);
                    return found == measured.end() ? int64_t{0} : found->second;
                });
        }
    }
    return true;
}

std::vector<int64_t> VisibleCutFrames(const Document& document) {
    std::vector<int64_t> cuts;
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) return cuts;
    std::vector<const DocumentTrack*> tracks;
    std::set<int64_t> candidates;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video" || !track.visible) continue;
        tracks.push_back(&track);
        for (const DocumentClip& clip : track.clips) {
            candidates.insert(FloorFrames(clip.timeline_in, rate));
            candidates.insert(
                FloorFrames(clip.timeline_in.add(clip.duration), rate));
        }
    }
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index > right->index;
                     });
    const Timeline timeline(document);
    const auto top = [&](int64_t frame) -> std::optional<ResolvedFrame> {
        for (const DocumentTrack* track : tracks) {
            const auto resolved =
                timeline.ResolveTrack(track->id, FrameTime(frame, rate));
            if (resolved) return resolved;
        }
        return std::nullopt;
    };
    for (const int64_t frame : candidates) {
        if (frame <= 0) continue;
        const auto before = top(frame - 1);
        const auto after = top(frame);
        if (!before || !after) {
            if (before || after) cuts.push_back(frame);
            continue;
        }
        // A clip split without removal plays on: not a cut.
        if (before->source_id != after->source_id ||
            after->source_frame != before->source_frame + 1)
            cuts.push_back(frame);
    }
    return cuts;
}

bool GenerateCaptionCartons(const Document& document, const CaptionStyle& style,
                            const std::vector<TimelineTranscriptSpan>& words,
                            const CaptionGenerationSettings& settings,
                            std::vector<GeneratedCarton>& cartons,
                            std::string& error) {
    cartons.clear();
    error.clear();
    // R3 -- docs/roadmap/REEL-2026-09.md. A carton is placed from a word's
    // timeline time, which TimelineTranscription derives from a source
    // time. On a clip that is not at 1/1 that derivation is another
    // calculation of the same mapping, and a wrong carton under
    // `applied: true` is exactly what guard rail 5 forbids.
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!ClipHasSpeed(clip)) continue;
            error = ClipSpeedRefusal(clip, "generate_captions");
            return false;
        }
    }
    const MediaRate rate = document.sequence.frame_rate;
    if (words.empty()) {
        error = "the montage has no words to caption";
        return false;
    }
    std::vector<SubtitleWord> timed;
    std::vector<std::string> unique;
    std::map<std::string, size_t> uniqueIndex;
    // RM2 -- docs/roadmap/REGLES-2026-10.md. The word that opens the film or
    // a sentence takes its capital before anything is measured, so the
    // widths are those of the text delivered.
    std::string previous;
    for (const TimelineTranscriptSpan& span : words) {
        const std::string text = SubtitleOpensSentenceAfter(previous)
                                     ? SubtitleWithCapital(span.text)
                                     : span.text;
        previous = span.text;
        timed.push_back(
            {text, span.timeline_in, span.timeline_in.add(span.duration)});
        if (uniqueIndex.emplace(text, unique.size()).second)
            unique.push_back(text);
    }
    // Two probes give the width of a space between words.
    const size_t pairIndex = unique.size();
    unique.push_back("x x");
    unique.push_back("xx");
    std::vector<int64_t> measured;
    if (!MeasureCaptionTextWidths(style, document.sequence, unique, measured,
                                  error))
        return false;
    const int64_t space =
        std::max<int64_t>(0, measured[pairIndex] - measured[pairIndex + 1]);
    // Each word was measured with its own outline and shadow; a line carries
    // them once.
    const int64_t decoration = (2 * std::max(0, style.outline_width)) +
                               std::max(0, style.shadow_offset);
    std::vector<int64_t> bare(timed.size());
    for (size_t index = 0; index < timed.size(); ++index) {
        bare[index] = std::max<int64_t>(
            0, measured[uniqueIndex[timed[index].text]] - decoration);
    }

    const int64_t maximumWidth = settings.maximum_line_width > 0
                                     ? settings.maximum_line_width
                                     : (document.sequence.width * 60) / 100;
    SubtitleSegmentationLimits limits;
    limits.maximum_characters = settings.maximum_characters;
    limits.maximum_width = maximumWidth;
    limits.width = [&](size_t begin, size_t end) {
        int64_t width = decoration;
        for (size_t index = begin; index < end; ++index) width += bare[index];
        return width + (space * static_cast<int64_t>(end - begin - 1));
    };
    std::vector<int64_t> learnedCosts;
    if (settings.break_model != nullptr)
        limits = WithCaptionBreakModel(*settings.break_model, timed, false,
                                       limits, learnedCosts);
    std::vector<SubtitleCue> cues;
    std::vector<std::pair<size_t, size_t>> ranges;
    std::vector<int64_t> exact;
    // Words summed miss kerning and side bearings: measured on the interview,
    // whole lines came out up to 24 px wider than their words. The estimate's
    // limit is tightened by the worst overshoot until the lines themselves
    // fit, or a carton is a single word that cannot be split.
    const bool given = !settings.cartons.empty();
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (given) {
            if (!GroupSubtitleWords(timed, settings.cartons, limits, cues,
                                    &ranges, error))
                return false;
        } else if (!SegmentSubtitleWords(timed, limits, cues, &ranges, error)) {
            return false;
        }
        std::vector<std::string> texts;
        for (const SubtitleCue& cue : cues) texts.push_back(cue.text);
        if (!MeasureCaptionTextWidths(style, document.sequence, texts, exact,
                                      error))
            return false;
        int64_t overshoot = 0;
        for (size_t index = 0; index < cues.size(); ++index) {
            if (ranges[index].second - ranges[index].first > 1)
                overshoot = std::max(overshoot, exact[index] - maximumWidth);
        }
        // Given cartons are the caller's: a line too wide is reported
        // (over_width), never cut again.
        if (overshoot <= 0 || given) break;
        limits.maximum_width -= overshoot;
    }

    // AGENT-2026-09 A32 -- a carton too short to read (under 0.8 s) first
    // joins its neighbour when the text still fits and the two are not
    // apart by more than the segmenter's own gap; failing that it grows into
    // the silence after it, then into the silence before it. On the third
    // run the first carton ("là,") lasted four frames, fixed by hand.
    //
    // REMOTION-PARITY-2026-09 REM1, scene C20 -- it used to borrow the
    // missing frames from a neighbour instead of the silence before it. A
    // neighbour's edges are its words, so the borrowed frames were always
    // spoken ones: "Puis" alone held the screen nine frames into "on cadre
    // le visage", which appeared after "on" was said. A carton that finds no
    // silence stays short; that is the house setting, not a defect.
    const int64_t minimumFrames = (static_cast<int64_t>(rate.num) * 4) /
                                  (5 * static_cast<int64_t>(rate.den));
    const auto frameCount = [&](const RationalTime& duration) {
        return FloorFrames(duration, rate);
    };
    // The last carton never runs past the sequence: on the native run of
    // the 26th it ended one frame after the picture and lengthened the
    // timeline.
    int64_t sequenceEndFrame = 0;
    for (const DocumentTrack& track : document.sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            sequenceEndFrame = std::max(
                sequenceEndFrame,
                FloorFrames(clip.timeline_in.add(clip.duration), rate));
    // RM2 -- docs/roadmap/REGLES-2026-10.md. A carton too short to read joins
    // the neighbour after it, else the one before; never across a sentence
    // end, which the segmentation itself never crosses. On an interview
    // series a two-word carton of 16 frames ending a sentence joined the
    // carton after its full stop.
    bool merged = false;
    for (size_t index = 0; index < cues.size() && !given;) {
        if (frameCount(cues[index].duration) >= minimumFrames ||
            cues.size() < 2) {
            ++index;
            continue;
        }
        std::vector<size_t> partners;
        if (index + 1 < cues.size()) partners.push_back(index + 1);
        if (index > 0) partners.push_back(index - 1);
        bool joined = false;
        for (const size_t partner : partners) {
            const size_t first = std::min(index, partner);
            const size_t second = std::max(index, partner);
            if (SubtitleEndsSentence(cues[first].text)) continue;
            const std::string text = cues[first].text + " " + cues[second].text;
            const RationalTime gap = cues[second].timeline_in.sub(
                cues[first].timeline_in.add(cues[first].duration));
            const bool fits =
                SubtitleCharacterCount(text) <= limits.maximum_characters &&
                limits.width(ranges[first].first, ranges[second].second) <=
                    limits.maximum_width &&
                gap.compare(kSubtitleCueMaximumGap) <= 0;
            if (!fits) continue;
            SubtitleCue combined;
            combined.text = text;
            combined.timeline_in = cues[first].timeline_in;
            combined.duration = cues[second]
                                    .timeline_in.add(cues[second].duration)
                                    .sub(cues[first].timeline_in);
            cues[first] = combined;
            ranges[first] = {ranges[first].first, ranges[second].second};
            cues.erase(cues.begin() + static_cast<std::ptrdiff_t>(second));
            ranges.erase(ranges.begin() + static_cast<std::ptrdiff_t>(second));
            merged = true;
            joined = true;
            index = first + 1;
            break;
        }
        if (!joined) ++index;
    }
    if (merged) {
        std::vector<std::string> texts;
        for (const SubtitleCue& cue : cues) texts.push_back(cue.text);
        if (!MeasureCaptionTextWidths(style, document.sequence, texts, exact,
                                      error))
            return false;
    }
    const std::vector<int64_t> cuts = VisibleCutFrames(document);
    for (size_t index = 0; index < cues.size(); ++index) {
        const int64_t frames = frameCount(cues[index].duration);
        if (frames >= minimumFrames) continue;
        int64_t needed = minimumFrames - frames;
        // Into the silence after it.
        if (index + 1 < cues.size()) {
            const int64_t room = FloorFrames(
                cues[index + 1].timeline_in.sub(
                    cues[index].timeline_in.add(cues[index].duration)),
                rate);
            const int64_t taken = std::max<int64_t>(0, std::min(room, needed));
            cues[index].duration =
                cues[index].duration.add(FrameTime(taken, rate));
            needed -= taken;
        } else {
            const int64_t room = std::max<int64_t>(
                0, sequenceEndFrame - FloorFrames(cues[index].timeline_in.add(
                                                      cues[index].duration),
                                                  rate));
            const int64_t taken =
                sequenceEndFrame > 0 ? std::min(room, needed) : needed;
            cues[index].duration =
                cues[index].duration.add(FrameTime(taken, rate));
            needed = 0;
        }
        if (needed <= 0) continue;
        // Into the silence before it, back to the previous carton's end but
        // never across a visible cut: a carton does not open on the tail of
        // the shot before its own.
        const int64_t startFrame = FloorFrames(cues[index].timeline_in, rate);
        int64_t earliest =
            index > 0
                ? FloorFrames(
                      cues[index - 1].timeline_in.add(cues[index - 1].duration),
                      rate)
                : 0;
        for (const int64_t cut : cuts)
            if (cut > earliest && cut <= startFrame) earliest = cut;
        const int64_t taken =
            std::max<int64_t>(0, std::min(startFrame - earliest, needed));
        cues[index].timeline_in =
            cues[index].timeline_in.sub(FrameTime(taken, rate));
        cues[index].duration = cues[index].duration.add(FrameTime(taken, rate));
    }

    const auto nearestCut = [&](int64_t frame, int64_t low, int64_t high) {
        int64_t best = -1;
        for (const int64_t cut : cuts) {
            if (cut < low || cut > high) continue;
            if (std::llabs(cut - frame) > settings.snap_frames) continue;
            if (best < 0 || std::llabs(cut - frame) < std::llabs(best - frame))
                best = cut;
        }
        return best;
    };
    const auto distance = [&](int64_t frame) -> int64_t {
        int64_t best = -1;
        for (const int64_t cut : cuts) {
            const int64_t gap = std::llabs(cut - frame);
            if (best < 0 || gap < best) best = gap;
        }
        return best;
    };

    // A snap never costs a carton more than a fifth of its time, nor leaves
    // it under 0.8 s: two cuts around a short carton once squeezed it to
    // one frame.
    const auto keepsReadable = [&](int64_t original, int64_t snapped) {
        const int64_t minimum = std::max<int64_t>(
            (original * 4) / 5,
            (static_cast<int64_t>(rate.num) * 4) / (5 * rate.den));
        return snapped >= minimum;
    };
    int64_t previousEnd = 0;
    for (size_t index = 0; index < cues.size(); ++index) {
        GeneratedCarton carton;
        carton.text = cues[index].text;
        carton.characters = SubtitleCharacterCount(cues[index].text);
        carton.rendered_width = exact[index];
        RationalTime start = cues[index].timeline_in;
        RationalTime end = start.add(cues[index].duration);
        const int64_t startFrame = FloorFrames(start, rate);
        const int64_t endFrame = FloorFrames(end, rate);
        const int64_t nextStart =
            index + 1 < cues.size()
                ? FloorFrames(cues[index + 1].timeline_in, rate)
                : std::numeric_limits<int64_t>::max() / 4;
        // DEC18 -- docs/roadmap/BENCH-2026-09.md. A snap never uncovers a
        // word: the carton opens at the latest when its first word starts
        // and closes at the earliest when its last word ends. On the bench
        // project of 16 September, two edges set onto cuts left 0.12 and
        // 0.24 s of speech without a carton, and a word said without its
        // carton is a defect of the "correct", not a matter of taste. Only a
        // cut that keeps every word covered is a candidate, so a nearer cut
        // inside a word gives way to a farther one outside it.
        const RationalTime firstWordStart = timed[ranges[index].first].start;
        const RationalTime lastWordEnd = timed[ranges[index].second - 1].end;
        const int64_t latestStart = FloorFrames(firstWordStart, rate);
        int64_t earliestEnd = FloorFrames(lastWordEnd, rate);
        if (FrameTime(earliestEnd, rate) < lastWordEnd) ++earliestEnd;
        // A snapped start never runs past the carton's own end; a snapped
        // end never overlaps the next carton.
        const int64_t snappedStart = nearestCut(
            startFrame, previousEnd, std::min(endFrame - 1, latestStart));
        if (snappedStart >= 0 && snappedStart != startFrame &&
            keepsReadable(endFrame - startFrame, endFrame - snappedStart)) {
            start = FrameTime(snappedStart, rate);
            carton.start_snapped = true;
        }
        const int64_t firstFrame = FloorFrames(start, rate);
        const int64_t snappedEnd = nearestCut(
            endFrame, std::max(firstFrame + 1, earliestEnd), nextStart);
        if (snappedEnd >= 0 && snappedEnd != endFrame &&
            keepsReadable(endFrame - firstFrame, snappedEnd - firstFrame)) {
            end = FrameTime(snappedEnd, rate);
            carton.end_snapped = true;
        }
        carton.timeline_in = start;
        carton.duration = end.sub(start);
        // PA4 -- the words' times on the carton's clock, which starts with
        // it. A start snapped back onto a cut leaves the first word a few
        // frames in; nothing is lit before it is said.
        if (CaptionWordCount(carton.text) ==
            ranges[index].second - ranges[index].first) {
            for (size_t word = ranges[index].first; word < ranges[index].second;
                 ++word) {
                carton.word_times.push_back(
                    ReducedCaptionWordTime(timed[word].start.sub(start)));
            }
        }
        carton.start_cut_distance = distance(FloorFrames(start, rate));
        carton.end_cut_distance = distance(FloorFrames(end, rate));
        previousEnd = FloorFrames(end, rate);
        cartons.push_back(std::move(carton));
    }
    // AGENT-2026-09 A32 -- a hole shorter than a breath between two cartons
    // of one phrase is a flicker: the earlier carton runs on to the next.
    // On the third run a four-frame hole opened on "que".
    const int64_t holeFrames = (static_cast<int64_t>(rate.num) * 32) /
                               (100 * static_cast<int64_t>(rate.den));
    for (size_t index = 0; index + 1 < cartons.size(); ++index) {
        const RationalTime end =
            cartons[index].timeline_in.add(cartons[index].duration);
        const int64_t hole =
            FloorFrames(cartons[index + 1].timeline_in.sub(end), rate);
        if (hole > 0 && hole <= holeFrames) {
            cartons[index].duration =
                cartons[index + 1].timeline_in.sub(cartons[index].timeline_in);
            cartons[index].end_cut_distance =
                distance(FloorFrames(cartons[index + 1].timeline_in, rate));
        }
    }
    return true;
}

AddTrackOperation BuildCaptionTrackEdit(
    const std::vector<GeneratedCarton>& cartons, const Ulid& styleId,
    int32_t trackIndex) {
    std::vector<SubtitleCue> cues;
    cues.reserve(cartons.size());
    for (const GeneratedCarton& carton : cartons)
        cues.push_back({carton.timeline_in, carton.duration, carton.text});
    AddTrackOperation operation = BuildSubtitleTrackEdit(cues, trackIndex);
    operation.sync_lock = true;
    for (size_t index = 0; index < operation.clips.size(); ++index) {
        DocumentClip& clip = operation.clips[index];
        clip.caption_group_id = styleId;
        // PA4 -- one clip per carton, in order (BuildSubtitleTrackEdit).
        clip.caption_word_times = cartons[index].word_times;
    }
    return operation;
}

bool BuildCaptionTranslation(const Document& document, const Ulid& trackId,
                             const std::vector<CaptionTranslationText>& texts,
                             int32_t trackIndex, bool visible,
                             CaptionTranslation& translation,
                             std::string& error) {
    translation = {};
    error.clear();
    const DocumentTrack* track = document.FindTrack(trackId);
    if (track == nullptr || track->kind != "caption") {
        error = "track '" + trackId + "' is not a caption track";
        return false;
    }
    std::map<Ulid, const std::string*> given;
    for (const CaptionTranslationText& text : texts) {
        const bool held = std::any_of(
            track->clips.begin(), track->clips.end(),
            [&](const DocumentClip& clip) { return clip.id == text.clip_id; });
        if (!held) {
            error = "clip '" + text.clip_id + "' is not a carton of track '" +
                    trackId + "'";
            return false;
        }
        if (text.text.empty()) {
            error = "the translation of carton '" + text.clip_id + "' is empty";
            return false;
        }
        if (!given.emplace(text.clip_id, &text.text).second) {
            error = "carton '" + text.clip_id + "' is given two translations";
            return false;
        }
    }
    std::vector<const DocumentClip*> source;
    for (const DocumentClip& clip : track->clips) source.push_back(&clip);
    std::stable_sort(source.begin(), source.end(),
                     [](const DocumentClip* left, const DocumentClip* right) {
                         return left->timeline_in < right->timeline_in;
                     });
    if (source.empty()) {
        error = "track '" + trackId + "' holds no carton to translate";
        return false;
    }
    std::vector<Ulid> missing;
    for (const DocumentClip* clip : source)
        if (given.count(clip->id) == 0) missing.push_back(clip->id);
    if (!missing.empty()) {
        error = std::to_string(missing.size()) + " of " +
                std::to_string(source.size()) + " cartons have no translation:";
        for (size_t index = 0; index < missing.size() && index < 5; ++index)
            error += " '" + missing[index] + "'";
        if (missing.size() > 5)
            error += " and " + std::to_string(missing.size() - 5) + " more";
        return false;
    }

    AddTrackOperation& operation = translation.operation;
    operation.track_id = GenerateUlid();
    operation.kind = "caption";
    operation.index = trackIndex;
    operation.sync_lock = track->sync_lock;
    operation.visible = visible;
    translation.maximum_line_width =
        CaptionMaximumLineWidth(document.sequence.width);
    for (const DocumentClip* clip : source) {
        DocumentClip translated = *clip;
        RenewClipIdentity(translated);
        translated.caption_text = *given.at(clip->id);
        // PA4 -- the times are those of the source's words; a translation
        // counts others, and is drawn whole.
        translated.caption_word_times.clear();
        TranslatedCarton carton;
        carton.source_clip_id = clip->id;
        carton.clip_id = translated.id;
        carton.text = translated.caption_text;
        carton.timeline_in = translated.timeline_in;
        carton.duration = translated.duration;
        carton.characters = SubtitleCharacterCount(carton.text);
        translation.cartons.push_back(std::move(carton));
        operation.clips.push_back(std::move(translated));
    }

    // Each run's texts measured in its own style, as the delivery draws
    // them. One failure is the renderer's absence: nothing is measured.
    std::map<Ulid, std::vector<size_t>> runs;
    for (size_t index = 0; index < operation.clips.size(); ++index)
        runs[operation.clips[index].caption_group_id].push_back(index);
    for (const auto& [styleId, members] : runs) {
        const CaptionStyle* found = document.FindCaptionStyle(styleId);
        const CaptionStyle style = found != nullptr ? *found : CaptionStyle{};
        std::vector<std::string> runTexts;
        for (const size_t index : members)
            runTexts.push_back(translation.cartons[index].text);
        std::vector<int64_t> widths;
        std::string measureError;
        if (!MeasureCaptionTextWidths(style, document.sequence, runTexts,
                                      widths, measureError) ||
            widths.size() != members.size()) {
            translation.width_error = measureError.empty()
                                          ? "the cartons could not be measured"
                                          : measureError;
            for (TranslatedCarton& carton : translation.cartons) {
                carton.rendered_width = -1;
                carton.wraps = false;
            }
            break;
        }
        for (size_t index = 0; index < members.size(); ++index) {
            TranslatedCarton& carton = translation.cartons[members[index]];
            carton.rendered_width = widths[index];
            carton.wraps = widths[index] > translation.maximum_line_width;
        }
    }
    return true;
}

std::string SerializeTranslatedCartons(
    const std::vector<TranslatedCarton>& cartons) {
    Value list = Value::MakeArray();
    for (const TranslatedCarton& carton : cartons) {
        Value item = Value::MakeObject();
        item.Set("source_clip_id", Value::MakeString(carton.source_clip_id));
        item.Set("clip_id", Value::MakeString(carton.clip_id));
        item.Set("text", Value::MakeString(carton.text));
        item.Set("timeline_in", TimeValue(carton.timeline_in));
        item.Set("duration", TimeValue(carton.duration));
        item.Set("characters",
                 Value::MakeInt(static_cast<int64_t>(carton.characters)));
        // Tenths of a character per second, by integers: the same carton
        // reads the same rate on every machine.
        const __int128 tenths =
            carton.duration.value > 0
                ? (static_cast<__int128>(carton.characters) * 10 *
                       carton.duration.rate +
                   carton.duration.value / 2) /
                      carton.duration.value
                : 0;
        item.Set("characters_per_second",
                 Value::MakeRawNumber(
                     std::to_string(static_cast<int64_t>(tenths / 10)) + "." +
                     std::to_string(static_cast<int64_t>(tenths % 10))));
        if (carton.rendered_width >= 0) {
            item.Set("rendered_width", Value::MakeInt(carton.rendered_width));
            item.Set("wraps", Value::MakeBool(carton.wraps));
        }
        list.Push(std::move(item));
    }
    return list.Dump();
}

std::string SerializeGeneratedCartons(
    const std::vector<GeneratedCarton>& cartons) {
    Value list = Value::MakeArray();
    for (const GeneratedCarton& carton : cartons) {
        Value item = Value::MakeObject();
        item.Set("text", Value::MakeString(carton.text));
        item.Set("timeline_in", TimeValue(carton.timeline_in));
        item.Set("duration", TimeValue(carton.duration));
        item.Set("characters",
                 Value::MakeInt(static_cast<int64_t>(carton.characters)));
        const long double seconds =
            static_cast<long double>(carton.duration.value) /
            carton.duration.rate;
        item.Set("approx_cps",
                 Value::MakeDouble(
                     seconds > 0.0L
                         ? static_cast<double>(carton.characters / seconds)
                         : 0.0));
        item.Set("rendered_width", Value::MakeInt(carton.rendered_width));
        item.Set("start_cut_distance_frames",
                 Value::MakeInt(carton.start_cut_distance));
        item.Set("end_cut_distance_frames",
                 Value::MakeInt(carton.end_cut_distance));
        item.Set("start_snapped", Value::MakeBool(carton.start_snapped));
        item.Set("end_snapped", Value::MakeBool(carton.end_snapped));
        // PA4 -- how many of its words are timed, not when: an animated
        // style reads the times, an author reads the text.
        item.Set(
            "timed_words",
            Value::MakeInt(static_cast<int64_t>(carton.word_times.size())));
        list.Push(std::move(item));
    }
    return list.Dump();
}

namespace {

double RelativeLuminance(uint8_t red, uint8_t green, uint8_t blue) {
    const auto linear = [](uint8_t channel) {
        const double value = channel / 255.0;
        return value <= 0.04045 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * linear(red)) + (0.7152 * linear(green)) +
           (0.0722 * linear(blue));
}

// Box dilation of a binary mask, separable, by `radius` pixels.
std::vector<uint8_t> Dilate(const std::vector<uint8_t>& mask, int64_t width,
                            int64_t height, int64_t radius) {
    std::vector<uint8_t> rows(mask.size(), 0);
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            if (!mask[(y * width) + x]) continue;
            for (int64_t dx = std::max<int64_t>(0, x - radius);
                 dx <= std::min(width - 1, x + radius); ++dx)
                rows[(y * width) + dx] = 1;
        }
    }
    std::vector<uint8_t> result(mask.size(), 0);
    for (int64_t y = 0; y < height; ++y) {
        for (int64_t x = 0; x < width; ++x) {
            if (!rows[(y * width) + x]) continue;
            for (int64_t dy = std::max<int64_t>(0, y - radius);
                 dy <= std::min(height - 1, y + radius); ++dy)
                result[(dy * width) + x] = 1;
        }
    }
    return result;
}

struct CartonMask {
    size_t result = 0;
    int64_t left = 0;
    int64_t top = 0;
    int64_t width = 0;
    int64_t height = 0;
    std::vector<uint8_t> ring;
};

}  // namespace

bool CheckCaptionLegibility(const Document& document,
                            const std::string& exportPath,
                            const std::string& ffmpegPath,
                            std::vector<CaptionLegibility>& results,
                            std::string& error) {
    results.clear();
    error.clear();
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0 || document.sequence.width <= 0) {
        error = "invalid sequence format";
        return false;
    }
    int32_t width = 0;
    int32_t height = 0;
    {
        AVFormatContext* format = nullptr;
        if (avformat_open_input(&format, exportPath.c_str(), nullptr, nullptr) <
            0) {
            error = "exported file cannot be opened: " + exportPath;
            return false;
        }
        if (avformat_find_stream_info(format, nullptr) >= 0) {
            const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO,
                                                   -1, -1, nullptr, 0);
            if (stream >= 0) {
                width = format->streams[stream]->codecpar->width;
                height = format->streams[stream]->codecpar->height;
            }
        }
        avformat_close_input(&format);
    }
    if (width <= 0 || height <= 0) {
        error = "exported file has no video stream";
        return false;
    }

    struct Carton {
        const DocumentClip* clip = nullptr;
        CaptionStyle style;
        int64_t frame = 0;
        std::array<uint8_t, 3> color{};
    };
    std::vector<Carton> cartons;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "caption" || !track.visible) continue;
        for (const DocumentClip& clip : track.clips) {
            // A disabled carton is not burned (ClipEnabled.h).
            if (clip.caption_text.empty() ||
                !ClipIsSeen(document.sequence, clip))
                continue;
            Carton carton;
            carton.clip = &clip;
            if (const CaptionStyle* stored =
                    document.FindCaptionStyle(clip.caption_group_id))
                carton.style = *stored;
            const RationalTime middle = clip.timeline_in.add(
                RationalTime{clip.duration.value, clip.duration.rate * 2});
            carton.frame = middle.to_frames(rate.num, rate.den);
            uint8_t rgba[4];
            if (av_parse_color(rgba, carton.style.color.c_str(), -1, nullptr) <
                0) {
                error = "caption colour cannot be read: " + carton.style.color;
                return false;
            }
            carton.color = {rgba[0], rgba[1], rgba[2]};
            cartons.push_back(std::move(carton));
        }
    }
    if (cartons.empty()) return true;
    std::stable_sort(cartons.begin(), cartons.end(),
                     [](const Carton& left, const Carton& right) {
                         return left.frame < right.frame;
                     });

    // The letters of every carton, one mask frame each, drawn from the text
    // the export delivers: long lines broken as it breaks them.
    std::vector<CaptionStyle> styles;
    std::vector<std::string> delivered;
    for (const Carton& carton : cartons) {
        styles.push_back(carton.style);
        delivered.push_back(carton.clip->caption_text);
    }
    if (!WrapCaptionTextsForDelivery(styles, document.sequence, delivered,
                                     error)) {
        return false;
    }
    const CaptionRenderer* renderer = CurrentCaptionRenderer();
    if (renderer == nullptr) {
        error = kNoCaptionRenderer;
        return false;
    }
    // LG4 -- docs/roadmap/LEGENDES-2026-10.md. The letters are located by
    // the renderer that drew them into the file: the same carton at the
    // file's size, its fill alone.
    CaptionDrawing drawing;
    drawing.width = width;
    drawing.height = height;
    drawing.sequence_width = document.sequence.width;
    drawing.glyphs_only = true;
    std::vector<std::vector<CaptionCarton>> pictures;
    for (size_t index = 0; index < cartons.size(); ++index) {
        pictures.push_back({{delivered[index], cartons[index].style, {1, 1}}});
    }
    std::vector<CartonMask> masks(cartons.size());
    size_t maskFrame = 0;
    const bool masked = renderer->draw(
        drawing, pictures,
        [&](size_t picture, CaptionImage& image) {
            const auto letter = [&](int64_t x, int64_t y) {
                return image.pixels[(4 * ((y * width) + x)) + 3] > 128;
            };
            CartonMask& mask = masks[picture];
            mask.result = picture;
            ++maskFrame;
            int64_t left = width;
            int64_t right = -1;
            int64_t top = height;
            int64_t bottom = -1;
            for (int64_t y = 0; y < height; ++y) {
                for (int64_t x = 0; x < width; ++x) {
                    if (!letter(x, y)) continue;
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
            if (right < left) return true;
            const int64_t fontPixels =
                (static_cast<int64_t>(cartons[mask.result].style.font_size) *
                 width) /
                document.sequence.width;
            const int64_t radius = std::max<int64_t>(2, fontPixels / 20);
            mask.left = std::max<int64_t>(0, left - radius - 2);
            mask.top = std::max<int64_t>(0, top - radius - 2);
            mask.width = std::min<int64_t>(width - 1, right + radius + 2) -
                         mask.left + 1;
            mask.height = std::min<int64_t>(height - 1, bottom + radius + 2) -
                          mask.top + 1;
            std::vector<uint8_t> glyphs(
                static_cast<size_t>(mask.width * mask.height), 0);
            for (int64_t y = 0; y < mask.height; ++y) {
                for (int64_t x = 0; x < mask.width; ++x) {
                    glyphs[(y * mask.width) + x] =
                        letter(mask.left + x, mask.top + y) ? 1 : 0;
                }
            }
            // The first pixel around a letter is antialiasing, drawn half in
            // the text colour: the ring starts one pixel out.
            const std::vector<uint8_t> inner =
                Dilate(glyphs, mask.width, mask.height, 1);
            std::vector<uint8_t> outer =
                Dilate(glyphs, mask.width, mask.height, radius + 1);
            for (size_t index = 0; index < outer.size(); ++index)
                outer[index] = outer[index] && !inner[index];
            mask.ring = std::move(outer);
            return true;
        },
        error);
    if (!masked || maskFrame != cartons.size()) {
        error = "unable to locate caption letters: " + error;
        return false;
    }
    const size_t grayBytes =
        static_cast<size_t>(width) * static_cast<size_t>(height);

    std::vector<int64_t> frames;
    for (const Carton& carton : cartons) {
        if (frames.empty() || frames.back() != carton.frame)
            frames.push_back(carton.frame);
    }
    std::string select = "select=";
    for (size_t index = 0; index < frames.size(); ++index) {
        if (index) select += '+';
        select += "eq(n\\," + std::to_string(frames[index]) + ')';
    }
    select += ",format=rgb24";
    results.resize(cartons.size());
    for (size_t index = 0; index < cartons.size(); ++index) {
        results[index].clip_id = cartons[index].clip->id;
        results[index].text = cartons[index].clip->caption_text;
        results[index].frame = cartons[index].frame;
    }
    size_t decoded = 0;
    const bool read = ReadFfmpegRawFrames(
        {ffmpegPath, "-hide_banner", "-loglevel", "error",     "-nostdin",
         "-i",       exportPath,     "-map",      "0:v:0",     "-an",
         "-sn",      "-vf",          select,      "-fps_mode", "passthrough",
         "-f",       "rawvideo",     "-pix_fmt",  "rgb24",     "pipe:1"},
        grayBytes * 3,
        [&](const uint8_t* pixels) {
            if (decoded >= frames.size()) return;
            const int64_t frame = frames[decoded];
            ++decoded;
            for (size_t index = 0; index < cartons.size(); ++index) {
                if (cartons[index].frame != frame) continue;
                const CartonMask& mask = masks[index];
                const double text = RelativeLuminance(cartons[index].color[0],
                                                      cartons[index].color[1],
                                                      cartons[index].color[2]);
                std::vector<double> ratios;
                for (int64_t y = 0; y < mask.height; ++y) {
                    for (int64_t x = 0; x < mask.width; ++x) {
                        if (!mask.ring[(y * mask.width) + x]) continue;
                        const size_t offset =
                            3 * static_cast<size_t>(((mask.top + y) * width) +
                                                    mask.left + x);
                        const double around = RelativeLuminance(
                            pixels[offset], pixels[offset + 1],
                            pixels[offset + 2]);
                        ratios.push_back((std::max(text, around) + 0.05) /
                                         (std::min(text, around) + 0.05));
                    }
                }
                CaptionLegibility& result = results[index];
                result.ring_pixels = static_cast<int64_t>(ratios.size());
                if (ratios.empty()) continue;
                std::sort(ratios.begin(), ratios.end());
                result.contrast_median = ratios[ratios.size() / 2];
                result.contrast_low = ratios[ratios.size() / 10];
            }
        },
        error);
    if (!read) {
        error = "unable to decode the exported file: " + error;
        return false;
    }
    if (decoded != frames.size()) {
        error = "the exported file is shorter than its captions";
        return false;
    }
    return true;
}

bool CaptionLegibilityBelowMinimum(const CaptionLegibility& result,
                                   double minimumContrast) {
    return result.ring_pixels == 0 || result.contrast_median < minimumContrast;
}

std::string SerializeCaptionLegibility(
    const std::vector<CaptionLegibility>& results, double minimumContrast) {
    Value root = Value::MakeObject();
    Value list = Value::MakeArray();
    int64_t illegible = 0;
    for (const CaptionLegibility& result : results) {
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(result.clip_id));
        item.Set("text", Value::MakeString(result.text));
        item.Set("frame", Value::MakeInt(result.frame));
        // Contrast ratios are measurements, not document time: two decimals.
        item.Set("contrast_median",
                 Value::MakeDouble(std::round(result.contrast_median * 100.0) /
                                   100.0));
        item.Set(
            "contrast_low",
            Value::MakeDouble(std::round(result.contrast_low * 100.0) / 100.0));
        item.Set("ring_pixels", Value::MakeInt(result.ring_pixels));
        const bool low = CaptionLegibilityBelowMinimum(result, minimumContrast);
        if (low) ++illegible;
        item.Set("below_minimum", Value::MakeBool(low));
        list.Push(std::move(item));
    }
    root.Set("minimum_contrast", Value::MakeDouble(minimumContrast));
    root.Set("cartons", Value::MakeInt(static_cast<int64_t>(results.size())));
    root.Set("below_minimum", Value::MakeInt(illegible));
    root.Set("results", std::move(list));
    return root.Dump();
}
