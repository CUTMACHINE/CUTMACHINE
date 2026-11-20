// IA3 -- docs/roadmap/IA-2026-09.md. Blocks of speech, the revision of the
// view a selection is read from, and the context a selection reports. No
// disk, no model: spans are given as the transcript view would build them.
//
// The fixture reproduces, in invented words, the two faults of a refused
// delivery (QC-2026-09b): a film opening in the middle of a sentence, and
// a word that refers back ("pareil" here) kept after the sentence it
// referred to was cut.

#include "SpeechSelection.h"

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

// This suite's own named test, which TestSupport.h's Test would otherwise
// take the place of for a lambda; kept, with its output.
void Case(const std::string& name, const std::function<void()>& body) {
    const int before = failures;
    body();
    std::cout << (failures == before ? "PASS: " : "FAIL: ") << name << '\n';
}

const Ulid kSourceA = "01K9J000000000000000000001";
const Ulid kSourceB = "01K9J000000000000000000002";

TimelineTranscriptSpan Span(const std::string& id, const Ulid& source,
                            int64_t sourceIn, int64_t duration,
                            int64_t timelineIn, const std::string& text) {
    TimelineTranscriptSpan span;
    span.span_id = id;
    span.source_id = source;
    span.source_in = {sourceIn, 25};
    span.duration = {duration, 25};
    span.timeline_in = {timelineIn, 25};
    span.text = text;
    return span;
}

// One answer of rush A laid out whole, then rush B. S1-S2 is one sentence
// cut in two cues; S3 is the sentence "pareil" refers to; S4-S5 the one
// that says it; S6 follows a one-second pause.
std::vector<TimelineTranscriptSpan> Interview() {
    return {
        Span("S1", kSourceA, 100, 30, 0, "Chaque matin, j'allume"),
        Span("S2", kSourceA, 130, 50, 30, "le four à bois\ndans la cour."),
        Span("S3", kSourceA, 185, 60, 85,
             "Mon grand-père pétrissait toujours la pâte à la main."),
        Span("S4", kSourceA, 250, 40, 150, "Alors nous, depuis cet hiver,"),
        Span("S5", kSourceA, 290, 45, 190,
             "on travaille pareil chaque semaine."),
        Span("S6", kSourceA, 360, 10, 260, "Voilà."),
        Span("S7", kSourceB, 0, 40, 270, "Une autre personne parle."),
    };
}

std::vector<SpeechSelectionSegment> Select(
    const std::vector<TimelineTranscriptSpan>& spans,
    const std::vector<SpeechSegmentRequest>& requests,
    SpeechSelectionError& kind, std::string& error) {
    std::vector<SpeechSelectionSegment> segments;
    ResolveSpeechSelection(spans, TranscriptViewRevision(spans), requests,
                           "segments", segments, kind, error);
    return segments;
}

SpeechSegmentRequest Blocks(const std::string& first,
                            const std::string& last = "") {
    SpeechSegmentRequest request;
    request.block_id = first;
    request.end_block_id = last;
    return request;
}

SpeechSegmentRequest Spans(const std::string& first,
                           const std::string& last = "") {
    SpeechSegmentRequest request;
    request.span_id = first;
    request.end_span_id = last;
    return request;
}

}  // namespace

int main() {
    Case("sentence ends and case read French punctuation", [] {
        Check(TextEndsSentence("dans la cour."), "a full stop ends");
        Check(TextEndsSentence("Vraiment\xC2\xA0?"), "a spaced ? ends");
        Check(TextEndsSentence("« C'est fini ! »"),
              "a closing guillemet does not hide the !");
        Check(TextEndsSentence("et puis\xE2\x80\xA6"), "an ellipsis ends");
        Check(!TextEndsSentence("depuis cet hiver,"), "a comma does not end");
        Check(!TextEndsSentence(""), "nothing does not end a sentence");
        Check(TextStartsLowercase("le four"), "lowercase start");
        Check(TextStartsLowercase("\xE2\x80\xA6 et après"),
              "an ellipsis before the word is not the word");
        Check(TextStartsLowercase("« été comme hiver »"),
              "an accented lowercase letter after a guillemet");
        Check(!TextStartsLowercase("\xC3\x89videmment"), "É is a capital");
        Check(!TextStartsLowercase("2026, c'est"), "a digit decides nothing");
        Check(!TextStartsLowercase("Et moi"), "a capital starts a sentence");
    });

    Case("blocks follow sentences, pauses and sources", [] {
        const std::vector<SpeechBlock> blocks = BuildSpeechBlocks(Interview());
        Check(blocks.size() == 5, "five blocks from seven cues");
        if (blocks.size() != 5) {
            return;
        }
        Check(blocks[0].first_span_id == "S1" && blocks[0].last_span_id == "S2",
              "a sentence cut in two cues is one block");
        Check(blocks[0].text ==
                  "Chaque matin, j'allume le four à bois dans la cour.",
              "the block reads as one line: " + blocks[0].text);
        Check(blocks[0].source_in == RationalTime{100, 25} &&
                  blocks[0].duration == RationalTime{80, 25} &&
                  blocks[0].timeline_in == RationalTime{0, 25},
              "a block's range runs from its first cue to its last");
        Check(blocks[1].first_span_id == "S3" && blocks[1].last_span_id == "S3",
              "a full stop closes a block even across a breath");
        Check(blocks[2].first_span_id == "S4" && blocks[2].last_span_id == "S5",
              "a comma does not");
        Check(blocks[3].first_span_id == "S6", "a one-second pause does");
        Check(blocks[4].source_id == kSourceB, "another rush does");
        Check(blocks[4].block_id == "B5", "blocks are numbered in view order");
        for (const SpeechBlock& block : blocks) {
            Check(!block.opens_mid_sentence && !block.closes_mid_sentence,
                  block.block_id + " is a whole sentence");
        }
    });

    Case("a view that starts mid-sentence says so", [] {
        // The rush trimmed so its first word is "le": the film would open
        // on "...le four".
        std::vector<TimelineTranscriptSpan> spans = Interview();
        spans.erase(spans.begin());
        const std::vector<SpeechBlock> blocks = BuildSpeechBlocks(spans);
        Check(!blocks.empty() && blocks[0].opens_mid_sentence,
              "the first block opens mid-sentence");
        Check(!blocks.empty() && !blocks[0].closes_mid_sentence,
              "and closes on its full stop");

        std::vector<TimelineTranscriptSpan> cut = Interview();
        cut[0].text = "Chaque matin, j'allume le four";
        cut.resize(1);
        Check(BuildSpeechBlocks(cut).front().closes_mid_sentence,
              "a block whose last cue has no final mark closes mid-sentence");
    });

    Case("a jump cut inside a sentence is not a mid-sentence start", [] {
        // A hesitation of 0.8 s taken out between "c'était" and "de": two
        // blocks, the sentence whole to the ear.
        std::vector<TimelineTranscriptSpan> cut = {
            Span("S1", kSourceA, 100, 30, 0, "Chaque matin, j'allume"),
            Span("S2", kSourceA, 150, 50, 30, "le four à bois."),
        };
        std::vector<SpeechBlock> blocks = BuildSpeechBlocks(cut);
        Check(blocks.size() == 2, "the removed pause splits the blocks");
        Check(blocks.size() == 2 && !blocks[0].closes_mid_sentence &&
                  !blocks[1].opens_mid_sentence,
              "the rest of the sentence follows: nothing is flagged");
        // The same two pieces in the other order: the film opens on "de".
        cut[1].timeline_in = {0, 25};
        cut[0].timeline_in = {50, 25};
        std::swap(cut[0], cut[1]);
        blocks = BuildSpeechBlocks(cut);
        Check(blocks.size() == 2 && blocks[0].opens_mid_sentence &&
                  blocks[1].closes_mid_sentence,
              "reordered, the film opens mid-sentence and ends on one");

        const std::vector<TimelineTranscriptSpan> spans = Interview();
        SpeechSelectionError kind = SpeechSelectionError::None;
        std::string error;
        std::vector<SpeechSelectionSegment> inOrder =
            Select(spans, {Spans("S1"), Spans("S2")}, kind, error);
        Check(inOrder.size() == 2 && !inOrder[0].closes_mid_sentence &&
                  !inOrder[1].opens_mid_sentence,
              "selected in order, the sentence is whole");
        std::vector<SpeechSelectionSegment> swapped =
            Select(spans, {Spans("S2"), Spans("S1")}, kind, error);
        Check(swapped.size() == 2 && swapped[0].opens_mid_sentence &&
                  swapped[1].closes_mid_sentence,
              "swapped, both halves are flagged");
    });

    Case("the revision names what the ids designate", [] {
        const std::vector<TimelineTranscriptSpan> spans = Interview();
        const std::string revision = TranscriptViewRevision(spans);
        Check(revision.size() == 16, "sixteen hex digits");
        Check(TranscriptViewRevision(Interview()) == revision,
              "the same view hashes the same");

        std::vector<TimelineTranscriptSpan> rescaled = spans;
        for (TimelineTranscriptSpan& span : rescaled) {
            span.source_in = {span.source_in.value * 2, 50};
            span.timeline_in = {span.timeline_in.value * 2, 50};
        }
        Check(TranscriptViewRevision(rescaled) == revision,
              "the same instants at another rate are the same view");

        std::vector<TimelineTranscriptSpan> trimmed = spans;
        trimmed[2].duration = {59, 25};
        Check(TranscriptViewRevision(trimmed) != revision,
              "an aligner moving one boundary by one frame changes it");

        std::vector<TimelineTranscriptSpan> retranscribed = spans;
        retranscribed[3].text = "Alors nous, depuis l'hiver,";
        Check(TranscriptViewRevision(retranscribed) != revision,
              "a new transcription changes it");

        std::vector<TimelineTranscriptSpan> flagged = spans;
        flagged[1].straddles_cut = true;
        Check(TranscriptViewRevision(flagged) != revision,
              "a reservation changes it");
    });

    Case("an old S2 is refused, never re-resolved", [] {
        const std::vector<TimelineTranscriptSpan> before = Interview();
        const std::string read = TranscriptViewRevision(before);
        // An insertion ahead of everything: the same numbers now name other
        // passages.
        std::vector<TimelineTranscriptSpan> after = before;
        after.insert(after.begin(),
                     Span("S0", kSourceB, 100, 20, 0, "Une question."));
        for (size_t index = 0; index < after.size(); ++index) {
            after[index].span_id = "S" + std::to_string(index + 1);
        }
        std::vector<SpeechSelectionSegment> segments;
        SpeechSelectionError kind = SpeechSelectionError::None;
        std::string error;
        Check(!ResolveSpeechSelection(after, read, {Spans("S2")}, "segments",
                                      segments, kind, error),
              "a selection read before the insertion is refused");
        Check(kind == SpeechSelectionError::Stale, "as stale");
        Check(
            error.find(read) != std::string::npos &&
                error.find(TranscriptViewRevision(after)) != std::string::npos,
            "the refusal names both revisions: " + error);
        Check(segments.empty(), "nothing is resolved");
        Check(!ResolveSpeechSelection(before, "", {Spans("S2")}, "segments",
                                      segments, kind, error) &&
                  kind == SpeechSelectionError::Stale,
              "no revision at all is not the current one");
    });

    Case("block runs resolve through the span path", [] {
        const std::vector<TimelineTranscriptSpan> spans = Interview();
        SpeechSelectionError kind = SpeechSelectionError::None;
        std::string error;
        const std::vector<SpeechSelectionSegment> blocks =
            Select(spans, {Blocks("B2", "B3")}, kind, error);
        const std::vector<SpeechSelectionSegment> cues =
            Select(spans, {Spans("S3", "S5")}, kind, error);
        Check(blocks.size() == 1 && cues.size() == 1, "both resolve: " + error);
        if (blocks.size() == 1 && cues.size() == 1) {
            Check(blocks[0].source_id == kSourceA &&
                      blocks[0].source_in == RationalTime{185, 25} &&
                      blocks[0].duration == RationalTime{150, 25},
                  "B2 to B3 is S3 to S5, source frames 185 to 335");
            Check(blocks[0].source_in == cues[0].source_in &&
                      blocks[0].duration == cues[0].duration,
                  "the same range by blocks and by spans");
            Check(blocks[0].first_span_id == "S3" &&
                      blocks[0].last_span_id == "S5",
                  "a segment names its edge spans");
        }

        Select(spans, {Blocks("B3", "B4")}, kind, error);
        Check(kind == SpeechSelectionError::Invalid &&
                  error.find("breath") != std::string::npos &&
                  error.find("B3") != std::string::npos,
              "a run over the one-second pause is refused and names its "
              "blocks: " +
                  error);
        Select(spans, {Blocks("B4", "B5")}, kind, error);
        Check(kind == SpeechSelectionError::Invalid,
              "a run across two rushes is refused");
        // AGENT-2026-09 A29 -- one refusal names every segment that does
        // not resolve, not only the first.
        Select(spans, {Blocks("B1"), Blocks("B3", "B4"), Blocks("B4", "B5")},
               kind, error);
        Check(kind == SpeechSelectionError::Invalid &&
                  error.find("segments[1]") != std::string::npos &&
                  error.find("segments[2]") != std::string::npos &&
                  error.find("2 segments to fix") != std::string::npos,
              "every unresolved segment is named in the one refusal: " + error);
        Select(spans, {Blocks("B3", "B2")}, kind, error);
        Check(kind == SpeechSelectionError::Invalid, "a reversed run too");
        Select(spans, {Blocks("B9")}, kind, error);
        Check(kind == SpeechSelectionError::Invalid &&
                  error.find("segments[0]") != std::string::npos,
              "an unknown block is refused by position: " + error);
        SpeechSegmentRequest both = Spans("S1");
        both.block_id = "B1";
        Select(spans, {both}, kind, error);
        Check(kind == SpeechSelectionError::Invalid,
              "a segment named both ways is refused");
        SpeechSegmentRequest mixed = Spans("S1");
        mixed.end_block_id = "B2";
        Select(spans, {mixed}, kind, error);
        Check(kind == SpeechSelectionError::Invalid,
              "an end id of the other kind is refused");
    });

    Case("a word that refers back, without its antecedent, is visible", [] {
        const std::vector<TimelineTranscriptSpan> spans = Interview();
        SpeechSelectionError kind = SpeechSelectionError::None;
        std::string error;
        std::vector<SpeechSelectionSegment> alone =
            Select(spans, {Blocks("B3")}, kind, error);
        Check(alone.size() == 1, "B3 resolves: " + error);
        if (alone.size() != 1) {
            return;
        }
        const SpeechNeighbour& before = alone[0].before;
        Check(
            before.present && before.block_id == "B2" && before.span_id == "S3",
            "what the rush says before B3 is S3, block B2");
        Check(before.text == spans[2].text, "its sentence is read whole");
        Check(before.gap == RationalTime{5, 25}, "five frames before B3");
        Check(!before.kept && !before.heard_next_to_it,
              "and the selection does not keep it");
        Check(!alone[0].opens_mid_sentence,
              "B3 is a sentence of its own: only its context tells");
        Check(alone[0].after.present && alone[0].after.block_id == "B4" &&
                  alone[0].after.gap == RationalTime{25, 25},
              "after it, one second later, B4");

        const std::vector<SpeechSelectionSegment> together =
            Select(spans, {Blocks("B2"), Blocks("B3")}, kind, error);
        Check(together.size() == 2 && together[1].before.kept &&
                  together[1].before.heard_next_to_it,
              "kept and heard just before when selected ahead of it");
        Check(together.size() == 2 && together[0].after.kept &&
                  together[0].after.heard_next_to_it,
              "and B2 is followed by what follows it in the rush");

        const std::vector<SpeechSelectionSegment> reordered =
            Select(spans, {Blocks("B3"), Blocks("B2")}, kind, error);
        Check(reordered.size() == 2 && reordered[0].before.kept &&
                  !reordered[0].before.heard_next_to_it,
              "kept elsewhere, but not heard before it, once reordered");
    });

    Case("a selection that starts mid-sentence shows the start", [] {
        const std::vector<TimelineTranscriptSpan> spans = Interview();
        SpeechSelectionError kind = SpeechSelectionError::None;
        std::string error;
        const std::vector<SpeechSelectionSegment> segments =
            Select(spans, {Spans("S2")}, kind, error);
        Check(segments.size() == 1, "S2 resolves: " + error);
        if (segments.size() != 1) {
            return;
        }
        Check(segments[0].opens_mid_sentence, "S2 opens mid-sentence");
        Check(segments[0].text == "le four à bois dans la cour.",
              "cue lines are joined: " + segments[0].text);
        Check(segments[0].before.present &&
                  segments[0].before.text == "Chaque matin, j'allume" &&
                  segments[0].before.block_id == "B1" &&
                  segments[0].before.gap == RationalTime{0, 25},
              "the start of its sentence, flush against it");

        const std::vector<SpeechSelectionSegment> first =
            Select(spans, {Spans("S1")}, kind, error);
        Check(first.size() == 1 && !first[0].before.present &&
                  first[0].closes_mid_sentence &&
                  first[0].after.text == "le four à bois dans la cour.",
              "nothing before the rush's first words, and the rest of the "
              "sentence after");
    });

    Case("a selection that replays material says which", [] {
        const std::vector<TimelineTranscriptSpan> spans = Interview();
        SpeechSelectionError kind = SpeechSelectionError::None;
        std::string error;
        const std::vector<SpeechSelectionSegment> segments = Select(
            spans, {Blocks("B1"), Blocks("B2"), Spans("S2")}, kind, error);
        Check(segments.size() == 3, "three segments: " + error);
        if (segments.size() != 3) {
            return;
        }
        Check(segments[0].overlaps == std::vector<size_t>{2} &&
                  segments[2].overlaps == std::vector<size_t>{0} &&
                  segments[1].overlaps.empty(),
              "B1 and S2 share material; B2 shares none");
    });

    Case("the selection report carries times and totals", [] {
        const std::vector<TimelineTranscriptSpan> spans = Interview();
        SpeechSelectionError kind = SpeechSelectionError::None;
        std::string error;
        const std::vector<SpeechSelectionSegment> segments =
            Select(spans, {Blocks("B3"), Blocks("B1")}, kind, error);
        const std::string json =
            SerializeSpeechSelection("01K9J00000000000000000000T",
                                     TranscriptViewRevision(spans), segments);
        Check(json.find("\"duration\":{\"value\":165,\"rate\":25}") !=
                  std::string::npos,
              "the total is exact: 85 + 80 frames: " + json);
        Check(json.find("\"opens_mid_sentence\"") == std::string::npos,
              "a flag that is false is left out");
        Check(json.find("\"before\":null") != std::string::npos,
              "B1 has nothing before it in the rush");
        const std::string blocks = SerializeSpeechBlocks(
            "01K9J00000000000000000000T", TranscriptViewRevision(spans),
            BuildSpeechBlocks(spans));
        Check(blocks.find("\"transcript_revision\":\"" +
                          TranscriptViewRevision(spans) + "\"") !=
                  std::string::npos,
              "the block view names its revision");
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
