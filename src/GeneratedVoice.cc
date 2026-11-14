#include "GeneratedVoice.h"

#include <utility>

namespace {

// One phrase per language the models say (VoiceOver.cc's kLanguages). The
// French one is the bench's (tools/voice-over-benchmark/vo_design.py),
// heard by the author on 7 November 2026; the others say the same thing and
// were not heard.
const std::pair<const char*, const char*> kReferenceTexts[] = {
    {"fr",
     "Bonjour. Je vais vous raconter une histoire. Prenez le temps "
     "d'écouter, et laissez-vous porter par les images."},
    {"en",
     "Hello. I am going to tell you a story. Take the time to listen, and "
     "let the images carry you."},
    {"de",
     "Guten Tag. Ich erzähle Ihnen eine Geschichte. Nehmen Sie sich Zeit "
     "zum Zuhören, und lassen Sie sich von den Bildern tragen."},
    {"es",
     "Hola. Voy a contarles una historia. Tómense el tiempo de escuchar, y "
     "déjense llevar por las imágenes."},
    {"it",
     "Buongiorno. Vi racconterò una storia. Prendetevi il tempo di "
     "ascoltare, e lasciatevi trasportare dalle immagini."},
    {"pt",
     "Olá. Vou contar uma história. Reserve um tempo para ouvir, e deixe-se "
     "levar pelas imagens."},
    {"ru",
     "Здравствуйте. Я расскажу вам одну историю. Не спешите, послушайте, и "
     "позвольте изображениям увлечь вас."},
    {"zh", "你好。我要给你们讲一个故事。请慢慢地听，让画面带着你走。"},
    {"ja",
     "こんにちは。これからある物語をお話しします。ゆっくりと耳を傾けて、"
     "映像に身をゆだねてください。"},
    {"ko",
     "안녕하세요. 지금부터 이야기 하나를 들려드리겠습니다. 천천히 "
     "들으시면서 영상에 몸을 맡겨 보세요."},
};

}  // namespace

std::string VoiceReferenceText(const std::string& language) {
    if (VoiceOverLanguageName(language).empty()) {
        return {};
    }
    for (const auto& [code, text] : kReferenceTexts) {
        if (language == code) {
            return text;
        }
    }
    return {};
}

std::string VoiceModelName(const VoiceOverResult& result) {
    std::string name = "Qwen/" + result.model;
    if (!result.revision.empty()) {
        name += "@" + result.revision;
    }
    return name;
}

MediaGeneration VoiceProvenance(const std::string& kind,
                                const VoiceOverResult& result,
                                const std::string& prompt,
                                const std::string& referenceText,
                                const Ulid& sourceMediaId,
                                const std::string& createdAt) {
    MediaGeneration generation;
    generation.service = kLocalVoiceService;
    generation.model = VoiceModelName(result);
    generation.kind = kind;
    generation.prompt = prompt;
    generation.seed = std::to_string(result.seed);
    generation.created_at = createdAt;
    generation.source_media_id = sourceMediaId;
    generation.reference_text = referenceText;
    return generation;
}

std::string CloneRefusal(const LibraryMedia& media) {
    const MediaGeneration& generation = media.generation;
    const std::string named =
        "media '" + media.id + "' (" + media.filename + ")";
    const std::string rule =
        ": generate_voice_over clones only a voice design_voice made, never "
        "a recording, a rush, an interview or a voice-over (DEC49). Make one "
        "with design_voice";
    if (!IsGenerated(generation)) {
        return named + " is not a voice the engine made" + rule;
    }
    if (generation.service != kLocalVoiceService ||
        generation.kind != kVoiceDesignKind) {
        return named + " was generated (" + generation.service + ", " +
               generation.kind + ") but is not a voice design_voice made" +
               rule;
    }
    if (generation.reference_text.empty()) {
        return named +
               " is a voice the engine made, but its provenance lacks the "
               "words it says, which cloning needs. Make a new one with "
               "design_voice";
    }
    return {};
}
