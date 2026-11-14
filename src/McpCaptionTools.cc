// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// captions: styles, generation, the break model, legibility and SRT import.
// Moved out of McpTools.cc as they were; what they share with the other files
// of the catalog is in McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

bool DispatchImportSrt(McpBackend& backend, const IdResolver&,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"path", "index"};
    if (!CheckKnownKeys(args, kAllowed, "import_srt", message))
        return Fail(errorName, message, message);
    std::string path;
    int64_t index = -1;
    if (!ReadString(args, "path", "import_srt", true, "", path, message) ||
        !ReadInt64(args, "index", "import_srt", false, -1, index, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    if (index < 0) {
        index = 0;
        for (const DocumentTrack& track : document.sequence.tracks)
            index = std::max(index, static_cast<int64_t>(track.index) + 1);
    }
    if (index > std::numeric_limits<int32_t>::max())
        return Fail(errorName, message, "'import_srt.index' is out of range");
    std::vector<SubtitleCue> cues;
    if (!LoadSrt(path, cues, message)) return Fail(errorName, message, message);
    return backend.ApplyOperation(
        BuildSubtitleTrackEdit(cues, static_cast<int32_t>(index)), resultJson,
        errorName, message);
}

// ITW4 -- every setting of a caption style, over the values `style` already
// holds: a new style's defaults, or the stored style an update starts from.
bool ReadCaptionStyleSettings(const Value& args, const std::string& tool,
                              CaptionStyle& style, std::string& message) {
    if (!ReadString(args, "font_family", tool, false, style.font_family,
                    style.font_family, message) ||
        !ReadInt32(args, "font_size", tool, false, style.font_size,
                   style.font_size, message) ||
        !ReadInt32(args, "font_weight", tool, false, style.font_weight,
                   style.font_weight, message) ||
        !ReadString(args, "color", tool, false, style.color, style.color,
                    message) ||
        !ReadString(args, "position", tool, false, style.position,
                    style.position, message) ||
        !ReadInt32(args, "outline_width", tool, false, style.outline_width,
                   style.outline_width, message) ||
        !ReadString(args, "outline_color", tool, false, style.outline_color,
                    style.outline_color, message) ||
        !ReadInt32(args, "shadow_offset", tool, false, style.shadow_offset,
                   style.shadow_offset, message) ||
        !ReadString(args, "shadow_color", tool, false, style.shadow_color,
                    style.shadow_color, message) ||
        !ReadString(args, "background_color", tool, false,
                    style.background_color, style.background_color, message) ||
        !ReadInt32(args, "background_padding", tool, false,
                   style.background_padding, style.background_padding,
                   message) ||
        // PA4 -- docs/roadmap/PARITE-2026-11.md. "none" is how a tool says
        // the empty animation the document holds.
        !ReadString(args, "word_animation", tool, false, style.word_animation,
                    style.word_animation, message) ||
        !ReadString(args, "highlight_color", tool, false, style.highlight_color,
                    style.highlight_color, message))
        return false;
    if (style.word_animation == "none") style.word_animation.clear();
    if (const Value* vertical = args.Find("vertical_position")) {
        if (!ReadFractionValue(*vertical, tool + ".vertical_position",
                               style.vertical_position.num,
                               style.vertical_position.den, message))
            return false;
        // {0, n} is spelled {0, 1}: the one way the document says "use the
        // position preset".
        if (style.vertical_position.num == 0) style.vertical_position = {0, 1};
    }
    return true;
}

const std::vector<std::string> kCaptionStyleSettingKeys = {
    "font_family",        "font_size",
    "font_weight",        "color",
    "position",           "outline_width",
    "outline_color",      "shadow_offset",
    "shadow_color",       "background_color",
    "background_padding", "vertical_position",
    "word_animation",     "highlight_color"};

bool DispatchAddCaptionStyle(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    std::vector<std::string> allowed = kCaptionStyleSettingKeys;
    allowed.push_back("insertion_index");
    allowed.push_back("kit");
    if (!CheckKnownKeys(args, allowed, "add_caption_style", message))
        return Fail(errorName, message, message);
    AddCaptionStyleOperation op;
    int64_t insertionIndex = -1;
    std::string kitName;
    if (!ReadString(args, "kit", "add_caption_style", false, "", kitName,
                    message))
        return Fail(errorName, message, message);
    // DEC6 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The brand's
    // charter first, at the timeline's size; the settings given beside it
    // win, and are said, since a value the charter does not allow will
    // fail check_timeline's `charter`. The style keeps values, not the
    // charter: the project renders the same without the library.
    MotionKitInfo kit;
    std::string kitFormat;
    std::vector<std::string> fromKit;
    if (!kitName.empty()) {
        if (!FindMotionLibraryKit(CurrentMotionLibraryLocation(), kitName, kit,
                                  message)) {
            errorName = "UnknownKit";
            return false;
        }
        Document document;
        if (!backend.SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        if (!MotionKitCaptionStyle(kit, document.sequence.width,
                                   document.sequence.height, op.style,
                                   kitFormat, fromKit, message)) {
            errorName =
                kit.captions.IsObject() ? "UnknownFormat" : "KitMismatch";
            return false;
        }
    }
    if (!ReadCaptionStyleSettings(args, "add_caption_style", op.style,
                                  message) ||
        !ReadInt64(args, "insertion_index", "add_caption_style", false, -1,
                   insertionIndex, message))
        return Fail(errorName, message, message);
    op.insertion_index = insertionIndex;
    op.style.id = GenerateUlid();
    const Ulid styleId = op.style.id;
    if (!backend.ApplyOperation(op, resultJson, errorName, message))
        return false;
    // ITW4 -- the id the next call needs, without reading the project.
    Value result;
    std::string parseError;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject())
        result = Value::MakeObject();
    result.Set("style_id", Value::MakeString(styleId));
    if (!kitName.empty()) {
        Value given = Value::MakeObject();
        given.Set("name", Value::MakeString(kit.name));
        if (kit.version > 0) given.Set("version", Value::MakeInt(kit.version));
        if (!kitFormat.empty())
            given.Set("format", Value::MakeString(kitFormat));
        Value set = Value::MakeArray();
        Value overridden = Value::MakeArray();
        for (const std::string& key : fromKit) {
            (args.Find(key) != nullptr ? overridden : set)
                .Push(Value::MakeString(key));
        }
        given.Set("set", std::move(set));
        if (!overridden.AsArray().empty())
            given.Set("overridden", std::move(overridden));
        result.Set("kit", std::move(given));
    }
    resultJson = result.Dump();
    return true;
}

bool DispatchUpdateCaptionStyle(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    std::vector<std::string> allowed = kCaptionStyleSettingKeys;
    allowed.push_back("style_id");
    if (!CheckKnownKeys(args, allowed, "update_caption_style", message))
        return Fail(errorName, message, message);
    Ulid styleId;
    if (!ReadId(args, "style_id", "update_caption_style", resolver, true,
                styleId, message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const CaptionStyle* stored = document.FindCaptionStyle(styleId);
    if (stored == nullptr) {
        errorName = EditErrorName(EditError::UnknownCaptionStyle);
        message = "unknown caption style_id '" + styleId + "'";
        return false;
    }
    UpdateCaptionStyleOperation op{*stored};
    if (!ReadCaptionStyleSettings(args, "update_caption_style", op.style,
                                  message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

// ITW4 -- ROADMAP.md. Cartons from the montage's words, measured by the
// renderer and set onto the picture cuts. See CaptionGeneration.h.
bool DispatchGenerateCaptions(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "style_id",    "max_characters", "max_line_width", "snap_frames",
        "track_index", "preview",        "break_model",    "cartons"};
    const std::string tool = "generate_captions";
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    Ulid styleId;
    // DEC4 -- docs/roadmap/REGLES-2026-10.md. One line of about 20
    // characters by default, 32 on a landscape timeline, which is only known
    // once the document is read below; --export-srt keeps 42. Zero stands
    // for "not given".
    int64_t maximumCharacters = 0;
    int64_t maximumLineWidth = 0;
    int64_t snapFrames = 12;
    int64_t trackIndex = -1;
    bool preview = false;
    std::string breakModel;
    if (!ReadId(args, "style_id", tool, resolver, false, styleId, message) ||
        !ReadInt64(args, "max_characters", tool, false, 0, maximumCharacters,
                   message) ||
        !ReadInt64(args, "max_line_width", tool, false, 0, maximumLineWidth,
                   message) ||
        !ReadInt64(args, "snap_frames", tool, false, 12, snapFrames, message) ||
        !ReadInt64(args, "track_index", tool, false, -1, trackIndex, message) ||
        !ReadBool(args, "preview", tool, false, preview, message) ||
        !ReadString(args, "break_model", tool, false, "installed", breakModel,
                    message))
        return Fail(errorName, message, message);
    if (breakModel != "installed" && breakModel != "rule")
        return Fail(errorName, message,
                    "'generate_captions.break_model' must be \"installed\" "
                    "or \"rule\"");
    // RM3 -- cartons grouped by the caller, as texts in the montage's order.
    std::vector<std::string> givenCartons;
    if (const Value* list = args.Find("cartons")) {
        if (!list->IsArray() || list->AsArray().empty() ||
            list->AsArray().size() > 10000)
            return Fail(errorName, message,
                        "'generate_captions.cartons' must list 1 to 10000 "
                        "texts");
        for (const Value& text : list->AsArray()) {
            if (!text.IsString())
                return Fail(errorName, message,
                            "'generate_captions.cartons' holds texts");
            givenCartons.push_back(text.AsString());
        }
    }
    const bool charactersGiven = args.Find("max_characters") != nullptr;
    if ((charactersGiven &&
         (maximumCharacters < 1 || maximumCharacters > 200)) ||
        maximumLineWidth < 0 || snapFrames < 0 || snapFrames > 250 ||
        trackIndex > std::numeric_limits<int32_t>::max())
        return Fail(errorName, message,
                    "'generate_captions' limits are out of range");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    if (!charactersGiven) {
        maximumCharacters =
            static_cast<int64_t>(DefaultCaptionCartonMaximumCharacters(
                document.sequence.width, document.sequence.height));
    }
    CaptionStyle style;
    if (!styleId.empty()) {
        const CaptionStyle* stored = document.FindCaptionStyle(styleId);
        if (stored == nullptr) {
            errorName = EditErrorName(EditError::UnknownCaptionStyle);
            message = "unknown caption style_id '" + styleId + "'";
            return false;
        }
        style = *stored;
    }
    std::vector<TimelineTranscriptSpan> words;
    TimelineWordCheck check;
    if (!backend.ReadTimelineWordSpans(words, check, message)) {
        errorName = "IoError";
        return false;
    }
    CaptionGenerationSettings settings;
    settings.maximum_characters = static_cast<size_t>(maximumCharacters);
    settings.maximum_line_width = maximumLineWidth;
    settings.snap_frames = snapFrames;
    // RM3 -- docs/roadmap/REGLES-2026-10.md. The installed model ranks where
    // a carton ends, inside the rule's bounds; without one the rule decides.
    // An installed file that does not read is said, never silently skipped.
    CaptionBreakModel learned;
    std::string modelFingerprint;
    std::string modelPath;
    std::string modelError;
    settings.cartons = givenCartons;
    if (givenCartons.empty() && breakModel == "installed" &&
        LoadInstalledCaptionBreakModel(learned, modelFingerprint, modelPath,
                                       modelError))
        settings.break_model = &learned;
    std::vector<GeneratedCarton> cartons;
    if (!GenerateCaptionCartons(document, style, words, settings, cartons,
                                message))
        return Fail(errorName, message, message);
    const int64_t usedWidth = maximumLineWidth > 0
                                  ? maximumLineWidth
                                  : (document.sequence.width * 60) / 100;

    Value root = Value::MakeObject();
    root.Set("preview", Value::MakeBool(preview));
    root.Set("style_id", Value::MakeString(styleId));
    root.Set("max_line_width", Value::MakeInt(usedWidth));
    // RM3 -- what cut the cartons, so that a measurement names its model.
    Value cutBy = Value::MakeObject();
    cutBy.Set("source",
              Value::MakeString(!givenCartons.empty()             ? "cartons"
                                : settings.break_model != nullptr ? "model"
                                                                  : "rule"));
    if (settings.break_model != nullptr) {
        cutBy.Set("fingerprint", Value::MakeString(modelFingerprint));
        cutBy.Set("path", Value::MakeString(modelPath));
    }
    if (!modelError.empty())
        cutBy.Set("model_error", Value::MakeString(modelError));
    root.Set("break_model", std::move(cutBy));
    int64_t overWidth = 0;
    for (const GeneratedCarton& carton : cartons)
        if (carton.rendered_width > usedWidth) ++overWidth;
    // A single word wider than the limit cannot be split; it is reported,
    // never hidden.
    root.Set("over_width", Value::MakeInt(overWidth));
    Value cartonList;
    std::string parseError;
    if (Value::Parse(SerializeGeneratedCartons(cartons), cartonList,
                     parseError))
        root.Set("cartons", std::move(cartonList));
    Value view;
    if (Value::Parse(SerializeTimelineTranscriptSpans(document, {}, &check),
                     view, parseError)) {
        if (const Value* timing = view.Find("word_timing"))
            root.Set("word_timing", *timing);
        if (const Value* heard = view.Find("word_check")) {
            // BENCH-2026-09 -- a word heard although aligned into a cut is
            // never added to a carton; the carton shown at its clip edge is
            // named (-1 when the edge falls between two cartons).
            Value named = *heard;
            const Value* cut = heard->Find("cut_but_heard");
            if (cut != nullptr && cut->IsArray() &&
                cut->AsArray().size() == check.cut_but_heard.size()) {
                Value list = Value::MakeArray();
                for (size_t index = 0; index < check.cut_but_heard.size();
                     ++index) {
                    const RationalTime edge =
                        check.cut_but_heard[index].timeline_in;
                    int64_t carton = -1;
                    for (size_t shown = 0; shown < cartons.size(); ++shown) {
                        if (cartons[shown].timeline_in <= edge &&
                            edge < cartons[shown].timeline_in.add(
                                       cartons[shown].duration)) {
                            carton = static_cast<int64_t>(shown);
                        }
                    }
                    Value item = cut->AsArray()[index];
                    item.Set("carton", Value::MakeInt(carton));
                    list.Push(std::move(item));
                }
                named.Set("cut_but_heard", std::move(list));
            }
            root.Set("word_check", std::move(named));
        }
    }
    // BENCH-2026-09 -- stretches between two cartons while a word is said.
    std::vector<std::pair<RationalTime, RationalTime>> shownCartons;
    shownCartons.reserve(cartons.size());
    for (const GeneratedCarton& carton : cartons) {
        shownCartons.emplace_back(carton.timeline_in, carton.duration);
    }
    Value gaps;
    if (Value::Parse(SerializeCaptionSpeechGaps(
                         FindCaptionSpeechGaps(shownCartons, check)),
                     gaps, parseError)) {
        root.Set("caption_gaps", std::move(gaps));
    }
    if (preview) {
        resultJson = root.Dump();
        return true;
    }
    if (trackIndex < 0) {
        trackIndex = 0;
        for (const DocumentTrack& track : document.sequence.tracks)
            trackIndex =
                std::max(trackIndex, static_cast<int64_t>(track.index) + 1);
    }
    AddTrackOperation operation = BuildCaptionTrackEdit(
        cartons, styleId, static_cast<int32_t>(trackIndex));
    const Ulid trackId = operation.track_id;
    // MOTIF3 -- the cartons come from the rule, with these settings; a
    // caller's own cartons are its choice and are not repeated here.
    Value ruleParams = Value::MakeObject();
    ruleParams.Set("max_characters", Value::MakeInt(maximumCharacters));
    ruleParams.Set("max_line_width", Value::MakeInt(maximumLineWidth));
    ruleParams.Set("snap_frames", Value::MakeInt(snapFrames));
    ruleParams.Set("break_model", Value::MakeString(breakModel));
    ruleParams.Set("given_cartons", Value::MakeBool(!givenCartons.empty()));
    RuleMotifScope ruleMotif(backend);
    if (!ruleMotif.Set(MotifReason::Captions, "generate_captions",
                       std::move(ruleParams), errorName, message))
        return false;
    std::string applied;
    if (!backend.ApplyOperation(operation, applied, errorName, message))
        return false;
    root.Set("track_id", Value::MakeString(trackId));
    Value appliedView;
    if (Value::Parse(applied, appliedView, parseError) &&
        appliedView.IsObject()) {
        if (const Value* hash = appliedView.Find("doc_hash"))
            root.Set("doc_hash", *hash);
        // PRD40 -- the delta every edit returns: the track and its cartons.
        if (const Value* delta = appliedView.Find("delta"))
            root.Set("delta", *delta);
    }
    resultJson = root.Dump();
    return true;
}

// PA5 -- docs/roadmap/PARITE-2026-11.md. A caption track in another
// language (CaptionGeneration.h): the caller's texts, the source track's
// times and styles, one AddTrack. The language is the caller's word; it is
// kept in the journal with the edit (the rule's parameters), not in the
// document, which has no field for it.
bool DispatchTranslateCaptions(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    const std::string tool = "translate_captions";
    if (!CheckKnownKeys(args,
                        {"track_id", "language", "cartons", "track_index",
                         "visible", "preview"},
                        tool, message))
        return Fail(errorName, message, message);
    Ulid trackId;
    std::string language;
    int64_t trackIndex = -1;
    bool visible = false;
    bool preview = false;
    if (!ReadId(args, "track_id", tool, resolver, true, trackId, message) ||
        !ReadString(args, "language", tool, true, "", language, message) ||
        !ReadInt64(args, "track_index", tool, false, -1, trackIndex, message) ||
        !ReadBool(args, "visible", tool, false, visible, message) ||
        !ReadBool(args, "preview", tool, false, preview, message))
        return Fail(errorName, message, message);
    // A language tag as subtitles name one: "en", "pt-BR", "zh-Hans".
    const bool tagged =
        language.size() >= 2 && language.size() <= 35 &&
        std::isalpha(static_cast<unsigned char>(language.front())) &&
        std::isalnum(static_cast<unsigned char>(language.back())) &&
        std::all_of(language.begin(), language.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
        });
    if (!tagged)
        return Fail(errorName, message,
                    "'translate_captions.language' is a language tag, as "
                    "\"en\" or \"pt-BR\"");
    const Value* list = args.Find("cartons");
    if (list == nullptr || !list->IsArray() || list->AsArray().empty())
        return Fail(errorName, message,
                    "'translate_captions.cartons' is a non-empty array of "
                    "{clip_id, text}");
    std::vector<CaptionTranslationText> texts;
    for (size_t index = 0; index < list->AsArray().size(); ++index) {
        const Value& entry = list->AsArray()[index];
        const std::string path =
            tool + ".cartons[" + std::to_string(index) + "]";
        CaptionTranslationText text;
        if (!entry.IsObject() ||
            !CheckKnownKeys(entry, {"clip_id", "text"}, path, message) ||
            !ReadId(entry, "clip_id", path, resolver, true, text.clip_id,
                    message) ||
            !ReadString(entry, "text", path, true, "", text.text, message))
            return Fail(errorName, message,
                        message.empty() ? "'" + path + "' is {clip_id, text}"
                                        : message);
        texts.push_back(std::move(text));
    }
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Fail(errorName, message, message);
    if (trackIndex < 0) {
        trackIndex = 0;
        for (const DocumentTrack& track : document.sequence.tracks)
            trackIndex =
                std::max(trackIndex, static_cast<int64_t>(track.index) + 1);
    }
    if (trackIndex > std::numeric_limits<int32_t>::max())
        return Fail(errorName, message,
                    "'translate_captions.track_index' is out of range");
    CaptionTranslation translation;
    if (!BuildCaptionTranslation(document, trackId, texts,
                                 static_cast<int32_t>(trackIndex), visible,
                                 translation, message))
        return Fail(errorName, message, message);

    Value root = Value::MakeObject();
    root.Set("applied", Value::MakeBool(!preview));
    root.Set("source_track_id", Value::MakeString(trackId));
    root.Set("language", Value::MakeString(language));
    root.Set("visible", Value::MakeBool(visible));
    root.Set("count",
             Value::MakeInt(static_cast<int64_t>(translation.cartons.size())));
    root.Set("max_line_width", Value::MakeInt(translation.maximum_line_width));
    root.Set("widths_measured",
             Value::MakeBool(translation.width_error.empty()));
    if (!translation.width_error.empty())
        root.Set("width_error", Value::MakeString(translation.width_error));
    int64_t wrapped = 0;
    for (const TranslatedCarton& carton : translation.cartons)
        if (carton.wraps) ++wrapped;
    root.Set("wrapped", Value::MakeInt(wrapped));
    Value cartonList;
    std::string parseError;
    if (Value::Parse(SerializeTranslatedCartons(translation.cartons),
                     cartonList, parseError))
        root.Set("cartons", std::move(cartonList));
    if (preview) {
        resultJson = root.Dump();
        return true;
    }
    Value ruleParams = Value::MakeObject();
    ruleParams.Set("language", Value::MakeString(language));
    ruleParams.Set("source_track_id", Value::MakeString(trackId));
    RuleMotifScope ruleMotif(backend);
    if (!ruleMotif.Set(MotifReason::Captions, tool, std::move(ruleParams),
                       errorName, message))
        return false;
    std::string applied;
    if (!backend.ApplyOperation(translation.operation, applied, errorName,
                                message))
        return false;
    root.Set("track_id", Value::MakeString(translation.operation.track_id));
    Value appliedView;
    if (Value::Parse(applied, appliedView, parseError) &&
        appliedView.IsObject()) {
        if (const Value* hash = appliedView.Find("doc_hash"))
            root.Set("doc_hash", *hash);
        if (const Value* delta = appliedView.Find("delta"))
            root.Set("delta", *delta);
    }
    resultJson = root.Dump();
    return true;
}

// RM3 -- docs/roadmap/REGLES-2026-10.md. The forge: a caption break model
// trained on delivered tracks, measured against the rule on groups held
// out of its training, and installed only when asked and when it wins.
bool DispatchTrainCaptionBreaks(McpBackend&, const IdResolver&,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    const std::string tool = "train_caption_breaks";
    if (!CheckKnownKeys(args,
                        {"corpus", "evaluation", "epochs", "step", "folds",
                         "vocabulary_minimum_groups", "install"},
                        tool, message))
        return Fail(errorName, message, message);
    CaptionBreakTrainingOptions options;
    int64_t epochs = options.epochs;
    int64_t folds = options.folds;
    int64_t minimumGroups =
        static_cast<int64_t>(options.vocabulary_minimum_groups);
    bool install = false;
    if (!ReadInt64(args, "epochs", tool, false, epochs, epochs, message) ||
        !ReadInt64(args, "step", tool, false, options.step, options.step,
                   message) ||
        !ReadInt64(args, "folds", tool, false, folds, folds, message) ||
        !ReadInt64(args, "vocabulary_minimum_groups", tool, false,
                   minimumGroups, minimumGroups, message) ||
        !ReadBool(args, "install", tool, false, install, message))
        return Fail(errorName, message, message);
    if (epochs < 1 || epochs > 100 || folds < 2 || folds > 50 ||
        minimumGroups < 1 || minimumGroups > 1000)
        return Fail(errorName, message,
                    "'train_caption_breaks' limits are out of range");
    options.epochs = static_cast<int32_t>(epochs);
    options.folds = static_cast<int32_t>(folds);
    options.vocabulary_minimum_groups = static_cast<size_t>(minimumGroups);
    // Absolute paths, 1 to 50 for the corpus and 0 to 50 for the evaluation.
    const auto readPaths = [&](const char* key, bool required,
                               std::vector<std::string>& paths) {
        const Value* list = args.Find(key);
        if (list == nullptr && !required) return true;
        const std::string name =
            std::string("'train_caption_breaks.") + key + "'";
        if (list == nullptr || !list->IsArray() ||
            (required && list->AsArray().empty()) ||
            list->AsArray().size() > 50) {
            message =
                name + " must list " + (required ? "1" : "0") + " to 50 files";
            return false;
        }
        for (const Value& path : list->AsArray()) {
            if (!path.IsString() ||
                !std::filesystem::path(path.AsString()).is_absolute()) {
                message = name + " paths must be absolute";
                return false;
            }
            paths.push_back(path.AsString());
        }
        return true;
    };
    std::vector<std::string> corpus;
    std::vector<std::string> evaluation;
    if (!readPaths("corpus", true, corpus) ||
        !readPaths("evaluation", false, evaluation))
        return Fail(errorName, message, message);
    return RunCaptionBreakForge(corpus, evaluation, options, install,
                                resultJson, errorName, message);
}

bool DispatchCheckCaptionLegibility(McpBackend& backend, const IdResolver&,
                                    const Value& args, std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    const std::string tool = "check_caption_legibility";
    if (!CheckKnownKeys(args, {"timeline_id", "path", "minimum_contrast"}, tool,
                        message))
        return Fail(errorName, message, message);
    std::string timelineId;
    std::string path;
    if (!ReadString(args, "timeline_id", tool, true, "", timelineId, message) ||
        !ReadString(args, "path", tool, true, "", path, message))
        return Fail(errorName, message, message);
    // An exact fraction, like every other ratio an agent passes: 3/1 by
    // default, 9/2 for the WCAG level of body text.
    int32_t minimumNum = 3;
    int32_t minimumDen = 1;
    if (const Value* minimum = args.Find("minimum_contrast")) {
        if (!ReadFractionValue(*minimum, tool + ".minimum_contrast", minimumNum,
                               minimumDen, message))
            return Fail(errorName, message, message);
        if (minimumDen <= 0 || minimumNum < minimumDen ||
            minimumNum > 21 * minimumDen)
            return Fail(errorName, message,
                        "'check_caption_legibility.minimum_contrast' must lie "
                        "between 1 and 21");
    }
    const double minimumContrast = static_cast<double>(minimumNum) / minimumDen;
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    std::vector<CaptionLegibility> results;
    // PRD1 -- no path of its own: the binary FfmpegBinary.h resolves.
    if (!CheckCaptionLegibility(document, path, std::string(), results,
                                message)) {
        errorName = "IoError";
        return false;
    }
    resultJson = SerializeCaptionLegibility(results, minimumContrast);
    // AX-2026-09 AX6 -- a carton whose family the caption renderer does not
    // have was drawn in Helvetica Neue: say so beside the contrast, since no
    // ratio shows it (FontCatalog.h). DEC19 -- since LEGENDES-2026-10 that
    // renderer is Core Text, no longer FFmpeg's fontconfig.
    Value result;
    std::string parseError;
    if (Value::Parse(resultJson, result, parseError) && result.IsObject()) {
        result.Set("fonts", CaptionFontsJson(CaptionFontFamilies(document),
                                             ReadCaptionFontFinder()));
        resultJson = result.Dump();
    }
    return true;
}

bool DispatchRemoveCaptionStyle(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"style_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_caption_style", message))
        return Fail(errorName, message, message);
    RemoveCaptionStyleOperation op;
    if (!ReadId(args, "style_id", "remove_caption_style", resolver, true,
                op.style_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetClipCaption(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "clip_id", "caption_group_id", "caption_text"};
    if (!CheckKnownKeys(args, kAllowed, "set_clip_caption", message))
        return Fail(errorName, message, message);
    SetClipCaptionOperation op;
    if (!ReadId(args, "clip_id", "set_clip_caption", resolver, true, op.clip_id,
                message) ||
        !ReadOptionalIdOrEmpty(args, "caption_group_id", "set_clip_caption",
                               resolver, op.caption_group_id, message) ||
        !ReadString(args, "caption_text", "set_clip_caption", false,
                    op.caption_text, op.caption_text, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

}  // namespace

void RegisterCaptionTools(const McpToolAdder& add) {
    add("import_srt", "Import a SubRip file as one exact caption track.",
        SchemaBuilder()
            .Field("path", StringSchema("Path to the UTF-8 .srt file."), true)
            .Field(
                "index",
                IntSchema("Optional unique track index; appends by default."),
                false)
            .Build("import_srt arguments"),
        DispatchImportSrt);

    const auto captionStyleSchema = [&](SchemaBuilder builder) {
        return builder
            .Field("font_family", StringSchema("Font family name."), false)
            .Field("font_size",
                   IntSchema("Font size in pixels at the sequence "
                             "resolution."),
                   false)
            .Field("font_weight",
                   IntSchema("Weight of the face, 1 to 1000 as in CSS: 400 "
                             "the regular (default), 500 the medium, 700 "
                             "the bold. The family's nearest face is drawn, "
                             "as a motion text's is."),
                   false)
            .Field("color", StringSchema("Text color, e.g. '#ffffff'."), false)
            .Field("position",
                   EnumSchema({"bottom", "top", "center"},
                              "Preset caption position, used unless "
                              "vertical_position is set."),
                   false)
            .Field("outline_width",
                   IntSchema("Outline around every letter, in pixels at the "
                             "sequence resolution; 0 for none. What keeps a "
                             "white carton legible over a white shirt."),
                   false)
            .Field("outline_color",
                   StringSchema("Outline color, e.g. '#000000'."), false)
            .Field("shadow_offset",
                   IntSchema("Drop shadow offset down and right, in pixels; "
                             "0 for none."),
                   false)
            .Field("shadow_color",
                   StringSchema("Shadow color; '@' adds an alpha, e.g. "
                                "'#000000@0.6'."),
                   false)
            .Field("background_color",
                   StringSchema("Box behind the text, e.g. '#000000@0.5'; "
                                "empty for none."),
                   false)
            .Field("background_padding",
                   IntSchema("Box padding around the text, in pixels."), false)
            .Field("vertical_position",
                   FractionSchema("Centre of the text block, as a fraction of "
                                  "the frame height from the top; 0/1: the "
                                  "preset position."),
                   false)
            .Field("word_animation",
                   EnumSchema({"none", "highlight", "reveal"},
                              "How a carton's words follow the voice: "
                              "'highlight' draws the word being said in "
                              "highlight_color, 'reveal' draws each word "
                              "from the moment it is said, the line placed "
                              "as when full. Only a carton whose words are "
                              "timed is animated -- one generate_captions "
                              "made (timed_words in describe); any other is "
                              "drawn whole. Default 'none'."),
                   false)
            .Field("highlight_color",
                   StringSchema("Colour of the word being said under "
                                "'highlight', e.g. '#ffe600' (default)."),
                   false);
    };

    add("add_caption_style",
        "Add a new caption style to the sequence and return its style_id. "
        "Lines of a carton are centred. `kit` starts from a brand's charter of "
        "the library (list_motion_templates kits, brand-kit.v1 with captions): "
        "its caption settings, and those of its caption format of the "
        "timeline's size; settings given beside it win and are listed in "
        "kit.overridden. The style keeps the values, not the charter.",
        captionStyleSchema(SchemaBuilder())
            .Field("insertion_index",
                   IntSchema("Position in the style list; omit to append."),
                   false)
            .Field("kit",
                   StringSchema("A brand's charter of the library whose "
                                "captions the style starts from."),
                   false)
            .Build("add_caption_style arguments"),
        DispatchAddCaptionStyle);

    add("generate_captions",
        "Build subtitle cartons from the words of the timeline's montage and "
        "place them on a new sync-locked caption track. Words come from the "
        "aligned transcript (MFA boundaries when cached) and skip any word no "
        "transcribe_timeline reading of the montage contains "
        "(word_check.unheard_words). Inflected matches (fait/faits) count as "
        "heard and are listed in word_check.inflected_matches. "
        "word_check.cut_but_heard names each word the aligner placed in "
        "removed material next to a clip edge while every reading hears it "
        "there, with the carton shown at that edge; it is reported, never "
        "added to a carton. caption_gaps lists every stretch between two "
        "cartons during which a word is said, with each word and its reason. "
        "Run again after cutting words: the text does not follow a cut by "
        "itself. Each carton keeps the time of each of its words "
        "(timed_words), which a style with a word_animation follows. The "
        "answer's break_model names what cut the cartons: the "
        "rule, the model train_caption_breaks installed (by fingerprint), or "
        "your own cartons.",
        SchemaBuilder()
            .Field("style_id",
                   IdSchema("Caption style of the cartons; the default style "
                            "when omitted."),
                   false)
            .Field("max_characters",
                   IntSchema("Characters per carton at most; default 32 on a "
                             "landscape timeline (wider than tall), 20 "
                             "otherwise."),
                   false)
            .Field("max_line_width",
                   IntSchema("Rendered width in pixels at the sequence "
                             "resolution (default 60 % of its width)."),
                   false)
            .Field("snap_frames",
                   IntSchema("Distance to a cut under which a carton edge "
                             "moves onto it (default 12)."),
                   false)
            .Field("track_index",
                   IntSchema("Index of the new caption track; above every "
                             "track by default."),
                   false)
            .Field("preview",
                   BoolSchema("Report the cartons without adding the track; "
                              "default false."),
                   false)
            .Field("break_model",
                   EnumSchema({"installed", "rule"},
                              "Who ranks where a carton ends: the installed "
                              "caption break model when there is one "
                              "(default), or the rule alone."),
                   false)
            .Field("cartons",
                   "{\"type\":\"array\",\"minItems\":1,"
                   "\"maxItems\":10000,\"items\":{\"type\":\"string\"},"
                   "\"description\":\"Your grouping instead of the rule's: "
                   "texts whose words are the montage's, in order "
                   "(get_timeline_transcript's spans; case and typographic "
                   "apostrophes aside). The engine sets times, capitals, "
                   "widths and snaps, never joins a short carton to a "
                   "neighbour, and refuses a word that differs or is missing, "
                   "a sentence end or a 0.7 s pause inside a carton, and a "
                   "carton over max_characters.\"}",
                   false)
            .Build("generate_captions arguments"),
        DispatchGenerateCaptions);

    add("train_caption_breaks",
        "Train a caption break model -- where a carton ends -- on delivered "
        "caption tracks, and measure it against the rule on groups held out of "
        "its training. Each corpus file holds one delivered track per line: "
        "{\"group\": project or film, \"frame_rate\": {\"num\", \"den\"}, "
        "\"cards\": [{\"text\", \"start\", \"duration\"}]}, times in frames. "
        "Folds by group give held_out.rule and held_out.model (boundaries "
        "matched, cartons reproduced). A line may give \"words\": [{\"text\", "
        "\"start\", \"end\"}] in frames, as said: each goes to the card its "
        "middle falls in, and the pauses are known. decisions checks the "
        "editor's caption choices; the two firm ones, article and conjunction, "
        "must hold. beats_rule is true when the model reproduces more cartons "
        "with a better boundary F1 held out and on the evaluation, and holds "
        "the firm choices. The model file is written as a candidate under the "
        "engine's models folder; install=true also installs it when "
        "beats_rule, and generate_captions then cuts with it. Answered with no "
        "project open.",
        SchemaBuilder()
            .Field("corpus",
                   "{\"type\":\"array\",\"minItems\":1,"
                   "\"maxItems\":50,\"items\":{\"type\":\"string\"}}",
                   true)
            .Field("evaluation",
                   "{\"type\":\"array\",\"maxItems\":50,"
                   "\"items\":{\"type\":\"string\"},\"description\":"
                   "\"Corpus files never trained on (a series' corrected "
                   "cartons), scored as evaluation.rule and "
                   "evaluation.model.\"}",
                   false)
            .Field("epochs", IntSchema("Passes over the corpus (default 5)."),
                   false)
            .Field("step",
                   IntSchema("How far one mistake moves a weight, in cost "
                             "units (default 10)."),
                   false)
            .Field("folds", IntSchema("Groups held out in turn (default 5)."),
                   false)
            .Field("vocabulary_minimum_groups",
                   IntSchema("Groups a word must appear in to get weights "
                             "of its own (default 10)."),
                   false)
            .Field("install",
                   BoolSchema("Install the model when it beats the rule; "
                              "needs an evaluation; default false."),
                   false)
            .Build("train_caption_breaks arguments"),
        DispatchTrainCaptionBreaks);

    add("check_caption_legibility",
        "Measure how legible every caption of a timeline is in the exported "
        "file. For each carton, on the frame at its middle, the letters are "
        "located with the export's own drawing and the text colour is compared "
        "with the ring of pixels just around them -- the outline when the "
        "style has one, the picture otherwise -- as a WCAG contrast ratio: its "
        "median, and its tenth percentile for the worst stretch. "
        "`fonts.missing` names families the file was drawn without (in "
        "Helvetica Neue instead, which no ratio shows): pick one list_fonts "
        "accepts and export again; `fonts.checked` false gives the reason it "
        "could not tell. Read-only; run on the final file.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full timeline ID the file was exported from."),
                   true)
            .Field("path",
                   StringSchema("Exported .mp4 or .mov file; a server takes a "
                                "relative path from the project's folder."),
                   true)
            .Field("minimum_contrast", std::string(kFractionSchemaText), false)
            .Build("check_caption_legibility arguments"),
        DispatchCheckCaptionLegibility);

    add("update_caption_style",
        "Change settings of an existing caption style. Every carton of the run "
        "follows: this is how a whole set of subtitles is made legible. "
        "Settings not given keep their value.",
        captionStyleSchema(SchemaBuilder())
            .Field("style_id", IdSchema("Caption style to change."), true)
            .Build("update_caption_style arguments"),
        DispatchUpdateCaptionStyle);

    // PA5 -- docs/roadmap/PARITE-2026-11.md.
    add("translate_captions",
        "Lay a caption track in another language beside an existing one: "
        "you give one text per carton of `track_id`, and the engine puts "
        "them on a new caption track at the same times and in the same "
        "styles, in one undo step. You translate; the engine keeps the "
        "timing and measures what the new texts cost to read. Each carton "
        "comes back with its characters, its characters per second, its "
        "width as the delivery draws it and `wraps` when it is wider than "
        "the delivery's line (`max_line_width`): a carton that runs long "
        "is yours to shorten, then call again. Every carton of the track "
        "needs a text; one left out is refused by name. The new track is "
        "HIDDEN unless `visible` is true: an export burns every visible "
        "caption track into the picture, so deliver one language at a time "
        "by showing its track and hiding the others (set_track_output). "
        "`preview:true` measures without editing. The language is recorded "
        "with the edit in the journal (list_motifs), not in the document.",
        SchemaBuilder()
            .Field("track_id", IdSchema("The caption track to translate."),
                   true)
            .Field("language",
                   StringSchema("Language tag of the translation, as "
                                "\"en\" or \"pt-BR\"."),
                   true)
            .Field(
                "cartons",
                ArraySchema(
                    SchemaBuilder()
                        .Field("clip_id",
                               IdSchema("A carton of the source track."), true)
                        .Field("text",
                               StringSchema("Its text in the new "
                                            "language; a line break "
                                            "is kept."),
                               true)
                        .Build("Translated carton"),
                    "One text per carton of the source track."),
                true)
            .Field("track_index",
                   IntSchema("Index of the new track; default: above every "
                             "track."),
                   false)
            .Field("visible",
                   BoolSchema("Show the new track at once; default false."),
                   false)
            .Field("preview",
                   BoolSchema("Measure without editing; default false."), false)
            .Build("translate_captions arguments"),
        DispatchTranslateCaptions);

    add("remove_caption_style", "Remove a caption style.",
        SchemaBuilder()
            .Field("style_id", IdSchema("Caption style to remove."), true)
            .Build("remove_caption_style arguments"),
        DispatchRemoveCaptionStyle);

    add("set_clip_caption",
        "Join a clip to a caption run (or clear its caption) and set its "
        "slice of the caption text. A carton whose words are timed "
        "(timed_words in describe) keeps their times when the new text "
        "counts as many words -- a spelling corrected, a style changed -- "
        "and loses them otherwise: it is then drawn whole under an animated "
        "style, until generate_captions is run again.",
        SchemaBuilder()
            .Field("clip_id", IdSchema("Clip to caption."), true)
            .Field("caption_group_id",
                   IdSchema("Caption style/run to join; omit or empty to "
                            "clear the clip's caption."),
                   false)
            .Field("caption_text",
                   StringSchema("This clip's slice of the caption text."),
                   false)
            .Build("set_clip_caption arguments"),
        DispatchSetClipCaption);
}
