#include "SpeechSelection.h"

#include "CanonicalHash.h"
#include "Json.h"
#include "Subtitles.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <optional>

namespace {

mcp_json::Value TimeValue(const RationalTime& time) {
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("value", mcp_json::Value::MakeInt(time.value));
    value.Set("rate", mcp_json::Value::MakeInt(time.rate));
    return value;
}

mcp_json::Value Seconds(const RationalTime& time) {
    return mcp_json::Value::MakeDouble(static_cast<double>(time.value) /
                                       time.rate);
}

// The same instant always hashes the same, whatever rate it was written in.
std::string ReducedTime(const RationalTime& time) {
    const int64_t divisor =
        std::gcd(std::llabs(time.value), static_cast<int64_t>(time.rate));
    const int64_t common = divisor == 0 ? 1 : divisor;
    return std::to_string(time.value / common) + "/" +
           std::to_string(time.rate / common);
}

RationalTime SpanEnd(const TimelineTranscriptSpan& span) {
    return span.source_in.add(span.duration);
}

// One code point of UTF-8 at `offset`, advancing it. A byte that does not
// start a valid sequence is returned as itself.
uint32_t NextCodePoint(const std::string& text, size_t& offset) {
    const auto byte = [&](size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const unsigned char lead = byte(offset);
    size_t length = 1;
    uint32_t codePoint = lead;
    if (lead >= 0xF0) {
        length = 4;
        codePoint = lead & 0x07U;
    } else if (lead >= 0xE0) {
        length = 3;
        codePoint = lead & 0x0FU;
    } else if (lead >= 0xC0) {
        length = 2;
        codePoint = lead & 0x1FU;
    }
    if (length == 1 || offset + length > text.size()) {
        ++offset;
        return lead;
    }
    for (size_t index = 1; index < length; ++index) {
        codePoint = (codePoint << 6U) | (byte(offset + index) & 0x3FU);
    }
    offset += length;
    return codePoint;
}

// The lowercase letters French writes: ASCII, Latin-1 and Latin
// Extended-A (odd code points there are the lowercase halves, œ included).
bool IsLowercaseLetter(uint32_t codePoint) {
    return (codePoint >= 'a' && codePoint <= 'z') ||
           (codePoint >= 0xDF && codePoint <= 0xFF && codePoint != 0xF7) ||
           (codePoint >= 0x100 && codePoint <= 0x17F && codePoint % 2 == 1);
}

// Anything that decides the question: a letter or a digit of any script.
// Spaces, quotes, dashes and ellipses before the first word do not.
bool DecidesCase(uint32_t codePoint) {
    return (codePoint >= '0' && codePoint <= '9') ||
           (codePoint >= 'A' && codePoint <= 'Z') ||
           (codePoint >= 'a' && codePoint <= 'z') ||
           (codePoint >= 0xC0 && codePoint != 0xD7 && codePoint != 0xF7 &&
            (codePoint < 0x2000 || codePoint > 0x206F));
}

std::string OneLine(std::string text) {
    std::replace(text.begin(), text.end(), '\n', ' ');
    return text;
}

// The spans of one source in view order, and where the view's blocks
// begin, so neighbours are found the same way for blocks and selections.
struct ViewIndex {
    // For each span, the index of its block in `blocks`.
    std::vector<size_t> block_of;
    // Each block as a range of span indices, [first, last].
    std::vector<std::pair<size_t, size_t>> blocks;
};

bool Continues(const TimelineTranscriptSpan& previous,
               const TimelineTranscriptSpan& next) {
    if (previous.source_id != next.source_id) {
        return false;
    }
    const RationalTime gap = next.source_in.sub(SpanEnd(previous));
    return gap.compare(RationalTime{0, 1}) >= 0 &&
           gap.compare(kSubtitleCueMaximumGap) < 0;
}

ViewIndex IndexView(const std::vector<TimelineTranscriptSpan>& spans) {
    ViewIndex index;
    index.block_of.resize(spans.size());
    for (size_t span = 0; span < spans.size(); ++span) {
        const bool joins = span > 0 &&
                           Continues(spans[span - 1], spans[span]) &&
                           !TextEndsSentence(spans[span - 1].text);
        if (joins) {
            index.blocks.back().second = span;
        } else {
            index.blocks.emplace_back(span, span);
        }
        index.block_of[span] = index.blocks.size() - 1;
    }
    return index;
}

// The span of `source` that ends last at or before `at`, the latest in view
// order on a tie (the same material placed twice).
std::optional<size_t> NearestBefore(
    const std::vector<TimelineTranscriptSpan>& spans, const Ulid& source,
    const RationalTime& at) {
    std::optional<size_t> found;
    for (size_t span = 0; span < spans.size(); ++span) {
        if (spans[span].source_id != source || SpanEnd(spans[span]) > at) {
            continue;
        }
        if (!found || SpanEnd(spans[*found]) <= SpanEnd(spans[span])) {
            found = span;
        }
    }
    return found;
}

// The span of `source` that starts first at or after `at`, the earliest in
// view order on a tie.
std::optional<size_t> NearestAfter(
    const std::vector<TimelineTranscriptSpan>& spans, const Ulid& source,
    const RationalTime& at) {
    std::optional<size_t> found;
    for (size_t span = 0; span < spans.size(); ++span) {
        if (spans[span].source_id != source || spans[span].source_in < at) {
            continue;
        }
        if (!found || spans[span].source_in < spans[*found].source_in) {
            found = span;
        }
    }
    return found;
}

std::string JoinText(const std::vector<TimelineTranscriptSpan>& spans,
                     size_t first, size_t last) {
    std::string text;
    for (size_t span = first; span <= last; ++span) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += OneLine(spans[span].text);
    }
    return text;
}

// Whether the words at `first` go on from a sentence the rush started
// earlier: a lowercase start, or a span of the same stretch right before
// that ends no sentence.
bool StartsMidSentence(const std::vector<TimelineTranscriptSpan>& spans,
                       size_t first) {
    if (TextStartsLowercase(spans[first].text)) {
        return true;
    }
    const std::optional<size_t> previous =
        NearestBefore(spans, spans[first].source_id, spans[first].source_in);
    return previous && Continues(spans[*previous], spans[first]) &&
           !TextEndsSentence(spans[*previous].text);
}

// Whether `heard`, played right before `first`, is the earlier part of the
// sentence `first` goes on with: same rush, earlier in it, and not ending a
// sentence. A jump cut that takes a hesitation out of a sentence leaves it
// whole to the ear; only a sentence entered from elsewhere is flagged. On a
// real 60 s montage of the bench, 16 of 18 blocks opened mid-sentence by the
// text alone, nearly all of them after such a cut.
bool HeardAsContinuation(const TimelineTranscriptSpan& heard,
                         const TimelineTranscriptSpan& first) {
    return heard.source_id == first.source_id &&
           SpanEnd(heard) <= first.source_in && !TextEndsSentence(heard.text);
}

bool OpensMidSentence(const std::vector<TimelineTranscriptSpan>& spans,
                      size_t first, std::optional<size_t> heardBefore) {
    return StartsMidSentence(spans, first) &&
           !(heardBefore &&
             HeardAsContinuation(spans[*heardBefore], spans[first]));
}

// The mirror: the last span ends no sentence, and what plays next is not
// the rest of it (same rush, later in it).
bool ClosesMidSentence(const std::vector<TimelineTranscriptSpan>& spans,
                       size_t last, std::optional<size_t> heardAfter) {
    if (TextEndsSentence(spans[last].text)) {
        return false;
    }
    return !(heardAfter &&
             spans[*heardAfter].source_id == spans[last].source_id &&
             SpanEnd(spans[last]) <= spans[*heardAfter].source_in);
}

struct Flags {
    bool straddles_cut = false;
    bool likely_hallucinated = false;
    bool likely_incomplete = false;
};

Flags SpanFlags(const std::vector<TimelineTranscriptSpan>& spans, size_t first,
                size_t last) {
    Flags flags;
    for (size_t span = first; span <= last; ++span) {
        flags.straddles_cut |= spans[span].straddles_cut;
        flags.likely_hallucinated |= spans[span].likely_hallucinated;
        flags.likely_incomplete |= spans[span].likely_incomplete;
    }
    return flags;
}

std::optional<size_t> FindSpan(const std::vector<TimelineTranscriptSpan>& spans,
                               const std::string& id) {
    for (size_t span = 0; span < spans.size(); ++span) {
        if (spans[span].span_id == id) {
            return span;
        }
    }
    return std::nullopt;
}

bool Covers(const SpeechSelectionSegment& segment,
            const TimelineTranscriptSpan& span) {
    return segment.source_id == span.source_id &&
           segment.source_in <= span.source_in &&
           SpanEnd(span) <= segment.source_in.add(segment.duration);
}

void SetFlags(mcp_json::Value& value, bool opensMid, bool closesMid,
              bool straddlesCut, bool hallucinated, bool incomplete) {
    // A flag that is false is left out: most blocks carry none, and the
    // view is read whole by a model.
    const auto flag = [&](const char* name, bool set) {
        if (set) {
            value.Set(name, mcp_json::Value::MakeBool(true));
        }
    };
    flag("opens_mid_sentence", opensMid);
    flag("closes_mid_sentence", closesMid);
    flag("straddles_cut", straddlesCut);
    flag("likely_hallucinated", hallucinated);
    flag("likely_incomplete", incomplete);
}

mcp_json::Value NeighbourValue(const SpeechNeighbour& neighbour) {
    if (!neighbour.present) {
        return mcp_json::Value::MakeNull();
    }
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("block_id", mcp_json::Value::MakeString(neighbour.block_id));
    value.Set("span_id", mcp_json::Value::MakeString(neighbour.span_id));
    value.Set("text", mcp_json::Value::MakeString(neighbour.text));
    value.Set("gap", TimeValue(neighbour.gap));
    value.Set("gap_seconds", Seconds(neighbour.gap));
    value.Set("kept", mcp_json::Value::MakeBool(neighbour.kept));
    value.Set("heard_next_to_it",
              mcp_json::Value::MakeBool(neighbour.heard_next_to_it));
    return value;
}

}  // namespace

std::string TranscriptViewRevision(
    const std::vector<TimelineTranscriptSpan>& spans) {
    std::string canonical = "transcript-view-1\n";
    for (const TimelineTranscriptSpan& span : spans) {
        canonical += span.span_id + '\t' + span.source_id + '\t' +
                     ReducedTime(span.source_in) + '\t' +
                     ReducedTime(span.duration) + '\t' +
                     ReducedTime(span.timeline_in) + '\t' +
                     (span.straddles_cut ? '1' : '0') +
                     (span.likely_hallucinated ? '1' : '0') +
                     (span.likely_incomplete ? '1' : '0') + '\t' +
                     std::to_string(span.text.size()) + ':' + span.text + '\n';
    }
    return CanonicalHash(canonical);
}

bool TextEndsSentence(const std::string& text) {
    static const std::vector<std::string> kClosing = {
        " ",
        "\t",
        "\n",
        "\r",
        "\"",
        "'",
        ")",
        "]",
        "\xC2\xBB",       // »
        "\xE2\x80\x9D",   // ”
        "\xE2\x80\x99",   // ’
        "\xC2\xA0",       // no-break space
        "\xE2\x80\xAF"};  // narrow nbsp
    std::string rest = text;
    bool stripped = true;
    while (stripped && !rest.empty()) {
        stripped = false;
        for (const std::string& closing : kClosing) {
            if (rest.size() >= closing.size() &&
                rest.compare(rest.size() - closing.size(), closing.size(),
                             closing) == 0) {
                rest.resize(rest.size() - closing.size());
                stripped = true;
            }
        }
    }
    if (rest.empty()) {
        return false;
    }
    const char last = rest.back();
    if (last == '.' || last == '?' || last == '!') {
        return true;
    }
    static const std::string kEllipsis = "\xE2\x80\xA6";
    return rest.size() >= kEllipsis.size() &&
           rest.compare(rest.size() - kEllipsis.size(), kEllipsis.size(),
                        kEllipsis) == 0;
}

bool TextStartsLowercase(const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const uint32_t codePoint = NextCodePoint(text, offset);
        if (IsLowercaseLetter(codePoint)) {
            return true;
        }
        if (DecidesCase(codePoint)) {
            return false;
        }
    }
    return false;
}

std::vector<SpeechBlock> BuildSpeechBlocks(
    const std::vector<TimelineTranscriptSpan>& spans) {
    const ViewIndex index = IndexView(spans);
    std::vector<SpeechBlock> blocks;
    for (const auto& range : index.blocks) {
        const TimelineTranscriptSpan& first = spans[range.first];
        const TimelineTranscriptSpan& last = spans[range.second];
        SpeechBlock block;
        block.block_id = "B" + std::to_string(blocks.size() + 1);
        block.first_span_id = first.span_id;
        block.last_span_id = last.span_id;
        block.source_id = first.source_id;
        block.source_in = first.source_in;
        block.duration = SpanEnd(last).sub(first.source_in);
        block.timeline_in = first.timeline_in;
        block.text = JoinText(spans, range.first, range.second);
        // In the view, what plays around a block is its neighbours.
        block.opens_mid_sentence = OpensMidSentence(
            spans, range.first,
            range.first > 0 ? std::optional<size_t>(range.first - 1)
                            : std::nullopt);
        block.closes_mid_sentence =
            ClosesMidSentence(spans, range.second,
                              range.second + 1 < spans.size()
                                  ? std::optional<size_t>(range.second + 1)
                                  : std::nullopt);
        const Flags flags = SpanFlags(spans, range.first, range.second);
        block.straddles_cut = flags.straddles_cut;
        block.likely_hallucinated = flags.likely_hallucinated;
        block.likely_incomplete = flags.likely_incomplete;
        blocks.push_back(std::move(block));
    }
    return blocks;
}

std::string StaleSelectionMessage(const std::string& given,
                                  const std::string& current) {
    return "transcript_revision '" + given +
           "' is not the current transcript view ('" + current +
           "'): an edit, a transcription or an alignment has moved or "
           "renumbered its spans since it was read, or it was read from "
           "another timeline. Span and block ids are positions in one "
           "view; read get_timeline_transcript or list_speech_blocks again "
           "and select from that";
}

bool ResolveSpeechSelection(const std::vector<TimelineTranscriptSpan>& spans,
                            const std::string& revision,
                            const std::vector<SpeechSegmentRequest>& requests,
                            const std::string& path,
                            std::vector<SpeechSelectionSegment>& segments,
                            SpeechSelectionError& kind, std::string& error) {
    segments.clear();
    kind = SpeechSelectionError::None;
    const std::string current = TranscriptViewRevision(spans);
    if (revision != current) {
        kind = SpeechSelectionError::Stale;
        error = StaleSelectionMessage(revision, current);
        return false;
    }
    const auto invalid = [&](size_t request, const std::string& detail) {
        kind = SpeechSelectionError::Invalid;
        error = path + "[" + std::to_string(request) + "]: " + detail;
        return false;
    };
    const ViewIndex index = IndexView(spans);
    // Each segment's edge spans, for what plays around it once cut together.
    std::vector<std::pair<size_t, size_t>> edges;
    std::vector<std::string> unresolved;
    const auto findBlock = [&](const std::string& id) {
        std::optional<size_t> found;
        if (id.size() > 1 && id[0] == 'B' &&
            std::all_of(id.begin() + 1, id.end(),
                        [](char c) { return c >= '0' && c <= '9'; })) {
            const size_t number = std::strtoull(id.c_str() + 1, nullptr, 10);
            if (number >= 1 && number <= index.blocks.size()) {
                found = number - 1;
            }
        }
        return found;
    };
    for (size_t request = 0; request < requests.size(); ++request) {
        const SpeechSegmentRequest& asked = requests[request];
        const bool bySpan = !asked.span_id.empty();
        const bool byBlock = !asked.block_id.empty();
        if (bySpan == byBlock) {
            return invalid(request,
                           "name the segment by span_id or by block_id, "
                           "exactly one of them");
        }
        if ((bySpan && !asked.end_block_id.empty()) ||
            (byBlock && !asked.end_span_id.empty())) {
            return invalid(request, bySpan
                                        ? "end_block_id belongs with block_id"
                                        : "end_span_id belongs with span_id");
        }
        std::string firstId = asked.span_id;
        std::string lastId = asked.end_span_id;
        if (byBlock) {
            const std::optional<size_t> first = findBlock(asked.block_id);
            if (!first) {
                return invalid(request,
                               "unknown block_id '" + asked.block_id + "'");
            }
            std::optional<size_t> last = first;
            if (!asked.end_block_id.empty()) {
                last = findBlock(asked.end_block_id);
                if (!last) {
                    return invalid(request, "unknown end_block_id '" +
                                                asked.end_block_id + "'");
                }
                if (*last < *first) {
                    return invalid(request, "end_block_id '" +
                                                asked.end_block_id +
                                                "' comes before block_id '" +
                                                asked.block_id + "'");
                }
            }
            firstId = spans[index.blocks[*first].first].span_id;
            lastId = spans[index.blocks[*last].second].span_id;
        }
        SpeechSelectionSegment segment;
        std::string detail;
        if (!ResolveTranscriptSpanRange(spans, firstId, lastId,
                                        segment.source_id, segment.source_in,
                                        segment.duration, detail)) {
            if (byBlock) {
                // The span ids in the message are the blocks' edges.
                detail += " (blocks '" + asked.block_id + "' to '" +
                          (asked.end_block_id.empty() ? asked.block_id
                                                      : asked.end_block_id) +
                          "')";
            }
            // AGENT-2026-09 A29 -- every segment that does not resolve is
            // named in the one refusal: on the bench of the 25 September
            // the first refusal named segment 1, the retry named segment
            // 15, a turn each.
            unresolved.push_back(path + "[" + std::to_string(request) +
                                 "]: " + detail);
            continue;
        }
        // ResolveTranscriptSpanRange found both ids; looked up again for
        // their positions.
        const std::optional<size_t> firstFound = FindSpan(spans, firstId);
        const std::optional<size_t> lastFound =
            lastId.empty() ? firstFound : FindSpan(spans, lastId);
        if (!firstFound || !lastFound) {
            return invalid(request, "unknown span id");
        }
        const size_t first = *firstFound;
        const size_t last = *lastFound;
        segment.first_span_id = spans[first].span_id;
        segment.last_span_id = spans[last].span_id;
        segment.text = JoinText(spans, first, last);
        edges.emplace_back(first, last);
        const Flags flags = SpanFlags(spans, first, last);
        segment.straddles_cut = flags.straddles_cut;
        segment.likely_hallucinated = flags.likely_hallucinated;
        segment.likely_incomplete = flags.likely_incomplete;
        segments.push_back(std::move(segment));
    }
    if (!unresolved.empty()) {
        kind = SpeechSelectionError::Invalid;
        error.clear();
        for (const std::string& item : unresolved)
            error += (error.empty() ? "" : " ; ") + item;
        if (unresolved.size() > 1)
            error += " (" + std::to_string(unresolved.size()) +
                     " segments to fix, all named here)";
        return false;
    }

    for (size_t position = 0; position < segments.size(); ++position) {
        SpeechSelectionSegment& segment = segments[position];
        const RationalTime end = segment.source_in.add(segment.duration);
        // In the film the selection builds, what plays around a segment is
        // the segments next to it.
        segment.opens_mid_sentence = OpensMidSentence(
            spans, edges[position].first,
            position > 0 ? std::optional<size_t>(edges[position - 1].second)
                         : std::nullopt);
        segment.closes_mid_sentence = ClosesMidSentence(
            spans, edges[position].second,
            position + 1 < segments.size()
                ? std::optional<size_t>(edges[position + 1].first)
                : std::nullopt);
        const auto kept = [&](const TimelineTranscriptSpan& span) {
            return std::any_of(segments.begin(), segments.end(),
                               [&](const SpeechSelectionSegment& other) {
                                   return Covers(other, span);
                               });
        };
        if (const std::optional<size_t> previous =
                NearestBefore(spans, segment.source_id, segment.source_in)) {
            const TimelineTranscriptSpan& span = spans[*previous];
            const size_t block = index.block_of[*previous];
            SpeechNeighbour& before = segment.before;
            before.present = true;
            before.block_id = "B" + std::to_string(block + 1);
            before.span_id = span.span_id;
            before.text = JoinText(spans, index.blocks[block].first, *previous);
            before.gap = segment.source_in.sub(SpanEnd(span));
            before.kept = kept(span);
            before.heard_next_to_it =
                position > 0 &&
                segments[position - 1].source_id == span.source_id &&
                segments[position - 1].source_in.add(
                    segments[position - 1].duration) == SpanEnd(span);
        }
        if (const std::optional<size_t> next =
                NearestAfter(spans, segment.source_id, end)) {
            const TimelineTranscriptSpan& span = spans[*next];
            const size_t block = index.block_of[*next];
            SpeechNeighbour& after = segment.after;
            after.present = true;
            after.block_id = "B" + std::to_string(block + 1);
            after.span_id = span.span_id;
            after.text = JoinText(spans, *next, index.blocks[block].second);
            after.gap = span.source_in.sub(end);
            after.kept = kept(span);
            after.heard_next_to_it =
                position + 1 < segments.size() &&
                segments[position + 1].source_id == span.source_id &&
                segments[position + 1].source_in == span.source_in;
        }
        for (size_t other = 0; other < segments.size(); ++other) {
            const SpeechSelectionSegment& candidate = segments[other];
            if (other == position || candidate.source_id != segment.source_id) {
                continue;
            }
            const RationalTime candidateEnd =
                candidate.source_in.add(candidate.duration);
            if (candidate.source_in < end && segment.source_in < candidateEnd) {
                segment.overlaps.push_back(other);
            }
        }
    }
    error.clear();
    return true;
}

std::string SerializeSpeechBlocks(const Ulid& timelineId,
                                  const std::string& revision,
                                  const std::vector<SpeechBlock>& blocks) {
    mcp_json::Value list = mcp_json::Value::MakeArray();
    for (const SpeechBlock& block : blocks) {
        mcp_json::Value value = mcp_json::Value::MakeObject();
        value.Set("block_id", mcp_json::Value::MakeString(block.block_id));
        value.Set("first_span_id",
                  mcp_json::Value::MakeString(block.first_span_id));
        value.Set("last_span_id",
                  mcp_json::Value::MakeString(block.last_span_id));
        value.Set("source_id", mcp_json::Value::MakeString(block.source_id));
        value.Set("source_in", TimeValue(block.source_in));
        value.Set("duration", TimeValue(block.duration));
        value.Set("timeline_in", TimeValue(block.timeline_in));
        value.Set("approx_seconds", Seconds(block.duration));
        value.Set("text", mcp_json::Value::MakeString(block.text));
        SetFlags(value, block.opens_mid_sentence, block.closes_mid_sentence,
                 block.straddles_cut, block.likely_hallucinated,
                 block.likely_incomplete);
        list.Push(std::move(value));
    }
    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("timeline_id", mcp_json::Value::MakeString(timelineId));
    root.Set("transcript_revision", mcp_json::Value::MakeString(revision));
    root.Set("blocks", std::move(list));
    return root.Dump();
}

std::string SerializeSpeechSelection(
    const Ulid& timelineId, const std::string& revision,
    const std::vector<SpeechSelectionSegment>& segments) {
    mcp_json::Value list = mcp_json::Value::MakeArray();
    RationalTime total{0, 1};
    for (const SpeechSelectionSegment& segment : segments) {
        total = total.add(segment.duration);
        mcp_json::Value value = mcp_json::Value::MakeObject();
        value.Set("first_span_id",
                  mcp_json::Value::MakeString(segment.first_span_id));
        value.Set("last_span_id",
                  mcp_json::Value::MakeString(segment.last_span_id));
        value.Set("source_id", mcp_json::Value::MakeString(segment.source_id));
        value.Set("source_in", TimeValue(segment.source_in));
        value.Set("duration", TimeValue(segment.duration));
        value.Set("approx_seconds", Seconds(segment.duration));
        value.Set("text", mcp_json::Value::MakeString(segment.text));
        SetFlags(value, segment.opens_mid_sentence, segment.closes_mid_sentence,
                 segment.straddles_cut, segment.likely_hallucinated,
                 segment.likely_incomplete);
        value.Set("before", NeighbourValue(segment.before));
        value.Set("after", NeighbourValue(segment.after));
        if (!segment.overlaps.empty()) {
            mcp_json::Value overlaps = mcp_json::Value::MakeArray();
            for (const size_t other : segment.overlaps) {
                overlaps.Push(
                    mcp_json::Value::MakeInt(static_cast<int64_t>(other)));
            }
            value.Set("overlaps_segments", std::move(overlaps));
        }
        list.Push(std::move(value));
    }
    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("timeline_id", mcp_json::Value::MakeString(timelineId));
    root.Set("transcript_revision", mcp_json::Value::MakeString(revision));
    root.Set("duration", TimeValue(total));
    root.Set("approx_seconds", Seconds(total));
    root.Set("segments", std::move(list));
    return root.Dump();
}
