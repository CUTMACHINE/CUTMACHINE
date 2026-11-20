#include "AgentJournal.h"

#include <algorithm>
#include <iterator>
#include <utility>

// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). See AgentJournal.h for
// what this decides and why.

namespace {

using Event = McpEndpoint::Event;

// What each tool that can change something does, in the words of a person
// reading the journal: an action, in the infinitive, as an Undo menu names
// one. The panel draws these with AppKit, so any character is allowed; they
// never reach the Metal atlas.
struct ToolTitle {
    const char* tool;
    const char* title;
};

constexpr ToolTitle kToolTitles[] = {
    // The timeline's journal.
    {"insert_clip", "insérer un plan"},
    {"edit_source_range", "monter une plage de rush"},
    {"delete_timeline_range", "supprimer une plage de la timeline"},
    {"remove_clip", "supprimer un plan"},
    {"clear_clip", "effacer un plan"},
    {"clear_clips", "effacer des plans"},
    {"trim_clip", "ajuster un plan"},
    {"move_clip", "déplacer un plan"},
    {"move_clips", "déplacer des plans"},
    {"split_clip", "couper un plan"},
    {"split_at_interval", "couper à intervalle régulier"},
    {"delete_gap", "supprimer un vide"},
    {"detach_audio", "détacher le son"},
    {"move_linked_clips", "déplacer des plans liés"},
    {"trim_linked_clips", "ajuster des plans liés"},
    {"shorten_linked_clip", "raccourcir un plan lié"},
    {"ripple_trim", "ajuster en décalant la suite"},
    {"roll_edit", "déplacer un raccord"},
    {"slip_edit", "faire glisser le contenu d'un plan"},
    {"slide_edit", "faire glisser un plan entre ses voisins"},
    {"rate_stretch", "étirer un plan en changeant sa vitesse"},
    {"remove_linked_clips", "supprimer des plans liés"},
    {"clear_linked_clips", "effacer des plans liés"},
    {"split_linked_clips", "couper des plans liés"},
    {"paste_clips", "coller des plans"},
    {"add_track", "ajouter une piste"},
    {"import_srt", "importer des sous-titres"},
    {"remove_track", "supprimer une piste"},
    {"set_track_lock", "verrouiller une piste"},
    {"set_track_sync_lock", "régler la synchro d'une piste"},
    {"set_track_output", "régler la sortie d'une piste"},
    {"update_sequence", "modifier la séquence"},
    {"set_color_management", "régler la gestion des couleurs"},
    {"add_bin", "ajouter un chutier"},
    {"remove_bin", "supprimer un chutier"},
    {"rename_bin", "renommer un chutier"},
    {"move_bin", "déplacer un chutier"},
    {"set_media_bin", "ranger un rush"},
    {"set_media_proxy", "attacher ou retirer un proxy"},
    {"add_marker", "poser un marqueur"},
    {"commit_run_step", "enregistrer une étape du montage"},
    {"remove_marker", "retirer un marqueur"},
    {"update_marker", "modifier un marqueur"},
    {"add_transition", "ajouter une transition"},
    {"remove_transition", "retirer une transition"},
    {"update_transition", "modifier une transition"},
    {"set_clip_link", "lier des plans"},
    {"split_link_groups", "séparer les liens fusionnés"},
    {"sync_audio", "poser le son séparé"},
    {"place_sound_stem", "poser un stem son à son timecode"},
    {"render_sound", "rendre un stem son sur les coupes"},
    {"set_clip_effects", "étalonner un plan"},
    {"paste_clip_grade", "coller un étalonnage"},
    {"set_clip_opacity", "régler l'opacité"},
    {"set_clip_blur", "flouter un plan"},
    {"set_clip_mask", "masquer un plan"},
    {"set_clip_speed", "changer la vitesse d'un plan"},
    {"set_clip_framing", "recadrer un plan"},
    {"set_clip_animation", "animer un plan"},
    {"set_clip_enabled", "activer ou désactiver des plans"},
    {"track_subject_framing", "faire suivre le sujet au cadrage"},
    {"stabilize_clip", "stabiliser un plan"},
    {"set_clip_audio", "régler le son d'un plan"},
    {"add_caption_style", "ajouter un style de sous-titres"},
    {"generate_captions", "générer les sous-titres"},
    {"translate_captions", "poser des sous-titres traduits"},
    {"train_caption_breaks", "entraîner la découpe des sous-titres"},
    {"update_caption_style", "modifier un style de sous-titres"},
    {"remove_caption_style", "retirer un style de sous-titres"},
    {"set_clip_caption", "modifier un sous-titre"},
    {"add_multicam_group", "grouper des angles"},
    {"set_multicam_active_angle", "changer d'angle"},
    {"split_multicam_group", "couper le multicam"},
    {"remove_multicam_group", "dissoudre un groupe d'angles"},
    {"clean_disfluencies", "retirer les hésitations"},
    {"tighten_pauses", "resserrer les pauses"},
    {"trim_boundary_air", "rogner l'air au bord des plans"},
    {"close_junction_air", "resserrer les raccords"},
    {"remove_words", "couper des mots"},
    {"remove_source_ranges", "couper des passages"},
    {"set_media_white_balance", "régler la balance des blancs"},
    {"apply_cdl", "appliquer un CDL"},
    {"set_media_color_input", "régler l'entrée couleur d'un rush"},
    {"describe_shots", "décrire des plans d'un rush"},
    {"propose_grade", "appliquer un étalonnage proposé"},
    {"conform_sequence", "conformer la séquence"},
    {"conform_color_management", "conformer la gestion des couleurs"},
    {"undo", "annuler"},
    {"redo", "rétablir"},
    {"end_intervention", "regrouper une intervention"},
    {"apply_edit_plan", "appliquer un plan de montage"},
    {"add_motion_composition", "ajouter une composition motion"},
    {"add_motion_template", "ajouter un modèle motion"},
    {"remove_motion_composition", "retirer une composition motion"},
    {"edit_motion_composition", "modifier une composition motion"},
    {"humanize_motion_composition", "humaniser une composition motion"},
    {"insert_motion_clip", "insérer un plan motion"},
    {"place_motion_clip", "poser un plan motion"},
    {"set_clip_motion_props", "régler un plan motion"},
    {"import_resolve_timeline", "importer une timeline de Resolve"},
    {"import_edl_markers", "importer des marqueurs d'une EDL"},
    {"set_sequence_lut", "poser un LUT sur la séquence"},
    // The project's journal.
    {"add_timeline", "ajouter une timeline"},
    {"remove_timeline", "supprimer une timeline"},
    {"clone_timeline", "dupliquer une timeline"},
    {"rename_timeline", "renommer une timeline"},
    {"set_timeline_bin", "ranger une timeline"},
    {"set_active_timeline", "changer de timeline active"},
    {"create_timeline_variants", "créer des variantes de timeline"},
    {"create_interview_short", "monter un short d'interview"},
    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md.
    {"choose_timeline_variant", "noter le choix d'une variante"},
    // Files at the caller's path.
    {"archive_project", "archiver le projet en SQL"},
    {"sync_project_archive", "reprendre l’archivage SQL du projet"},
    {"import_project_archive", "importer des projets dans l’archive SQL"},
    {"backup_project_archive", "copier l’archive SQL hors du disque"},
    {"export_timeline", "exporter la timeline"},
    {"export_timelines", "exporter des timelines"},
    {"render_motion_video", "rendre une vidéo motion"},
    {"export_resolve_xml", "exporter un XML pour Resolve"},
    {"send_to_resolve", "renvoyer le montage dans Resolve"},
    {"export_edl", "exporter des EDL"},
    {"export_edl_markers", "exporter les marqueurs en EDL"},
    {"export_otio", "exporter un fichier OTIO"},
    {"export_fcpxml", "exporter un FCPXML"},
    {"export_cdl", "exporter un CDL"},
    // Outside any journal.
    {"create_project", "créer un projet"},
    {"ingest_media", "importer des rushes"},
    {"save_motion_template", "enregistrer un modèle motion"},
    // CH4 -- docs/roadmap/CHARTE-2026-11.md. A charter of the library.
    {"save_motion_kit", "enregistrer une charte"},
    {"edit_motion_kit", "modifier une charte"},
    {"restore_motion_kit", "restaurer une version de charte"},
    // CF1 -- a viewing preference, outside the edit journal.
    {"set_video_output", "choisir la sortie vidéo"},
    // Analyses, which write a cache beside the project.
    {"transcribe_media", "transcrire des rushes"},
    {"transcribe_timeline", "transcrire la timeline"},
    {"analyze_speech_onset", "repérer les débuts de parole"},
    {"analyze_shot_quality", "analyser la qualité des plans"},
    {"analyze_audio_beats", "repérer les temps de la musique"},
    {"analyze_hesitations", "repérer les hésitations"},
    {"analyze_isolated_frames", "repérer les images isolées"},
    {"align_transcript", "aligner les transcriptions"},
    {"align_words", "aligner les mots"},
    {"analyze_speakers", "distinguer les voix d'un rush"},
    {"analyze_person_matte", "détourer les personnes d'un rush"},
    // Another program, or a server.
    {"after_create_project", "créer un projet After Effects"},
    {"after_open_project_copy", "ouvrir une copie de projet After Effects"},
    {"after_render_frame", "rendre une image dans After Effects"},
    {"after_set_text", "changer un texte dans After Effects"},
    {"after_set_property", "régler une propriété dans After Effects"},
    {"after_set_layer_state", "régler un calque dans After Effects"},
    {"after_replace_footage", "remplacer un métrage dans After Effects"},
    {"after_import_footage", "importer un métrage dans After Effects"},
    {"after_add_layer", "ajouter un calque dans After Effects"},
    {"after_duplicate_layer", "dupliquer un calque dans After Effects"},
    {"after_save_project", "enregistrer le projet After Effects"},
    {"export_motion_after", "exporter une composition vers After Effects"},
    {"verify_after_export", "comparer un projet After Effects au moteur"},
    {"import_after_comp", "importer une composition d'After Effects"},
    // GENERATIF-2026-10 GN2 -- what the journal says of a paid generation.
    {"propose_generation", "proposer une génération payante"},
    {"generate_media", "générer un média payant avec fal.ai"},
    {"design_voice", "fabriquer une voix"},
    {"generate_voice_over", "faire dire une voix off"},
};

// `text` with its first letter in upper case: the titles above all start
// with a letter of ASCII or with é.
std::string Capitalized(std::string text) {
    if (!text.empty() && text[0] >= 'a' && text[0] <= 'z') {
        text[0] = static_cast<char>(text[0] - 'a' + 'A');
    } else if (text.size() >= 2 &&
               static_cast<unsigned char>(text[0]) == 0xC3) {
        // é è ê à, two bytes each in UTF-8: the capital is 0x20 lower.
        const unsigned char second = static_cast<unsigned char>(text[1]);
        if (second == 0xA9 || second == 0xA8 || second == 0xAA ||
            second == 0xA0) {
            text[1] = static_cast<char>(second - 0x20);
        }
    }
    return text;
}

std::string Quoted(const std::string& text) { return "« " + text + " »"; }

// " — timeline « Nom »" when the call acted on a timeline other than the one
// the window showed, or on one named while the window's is not known.
std::string TimelineSegment(const std::string& timelineId,
                            const std::string& shownTimelineId,
                            const AgentJournalContext& context) {
    if (timelineId.empty() || timelineId == shownTimelineId ||
        !context.timeline_name) {
        return {};
    }
    const std::string name = context.timeline_name(timelineId);
    return name.empty() ? std::string() : " — timeline " + Quoted(name);
}

std::string Stamped(const AgentJournalContext& context,
                    const std::string& line) {
    return context.time.empty() ? line : context.time + " " + line;
}

void Insert(std::set<std::string>& into, const std::vector<std::string>& ids) {
    into.insert(ids.begin(), ids.end());
}

}  // namespace

std::string AgentToolTitle(const std::string& tool) {
    for (const ToolTitle& entry : kToolTitles) {
        if (tool == entry.tool) {
            return entry.title;
        }
    }
    return tool;
}

std::string AgentClipCountText(int64_t changed, int64_t created,
                               int64_t removed, int64_t shifted) {
    const std::pair<int64_t, const char*> figures[] = {{changed, "modifié"},
                                                       {created, "créé"},
                                                       {removed, "retiré"},
                                                       {shifted, "décalé"}};
    std::string text;
    for (const auto& [count, participle] : figures) {
        if (count <= 0) {
            continue;
        }
        const bool plural = count > 1;
        text += text.empty()
                    ? std::to_string(count) + (plural ? " plans " : " plan ")
                    : ", " + std::to_string(count) + " ";
        text += participle;
        if (plural) {
            text += 's';
        }
    }
    return text;
}

bool AgentJournal::Record(const Event& event,
                          const AgentJournalContext& context) {
    switch (event.kind) {
        case Event::Kind::Connected:
            return false;
        case Event::Kind::Disconnected:
            // Its interventions never ended: their steps stay apart, as the
            // engine leaves them apart in the timeline's history.
            open_.erase(std::remove_if(open_.begin(), open_.end(),
                                       [&](const Intervention& open) {
                                           return open.connection ==
                                                  event.connection;
                                       }),
                        open_.end());
            return false;
        case Event::Kind::ToolCall:
            break;
    }
    if (event.replayed) {
        return false;
    }
    if (!McpToolClassActs(event.tool_class)) {
        // A token handed out begins an intervention (begin_intervention, a
        // read); a plan prepared is kept for its short apply (a session
        // call). Neither is an entry.
        if (event.ok && !event.intervention.empty()) {
            Intervention open;
            open.token = event.intervention;
            open.connection = event.connection;
            open.timeline_id = event.timeline_id;
            open_.push_back(std::move(open));
        }
        if (event.ok && event.has_delta && !event.plan_id.empty()) {
            RememberPlan(event.plan_id, event.delta);
        }
        return false;
    }
    // A token given back ends one (end_intervention).
    if (!event.intervention.empty()) {
        return Close(event, context);
    }

    const std::string timeline =
        TimelineSegment(event.timeline_id, event.shown_timeline_id, context);
    AgentJournalEntry entry;
    if (!event.ok) {
        const std::string what = event.label.empty()
                                     ? AgentToolTitle(event.tool)
                                     : Quoted(event.label);
        entry.refused = true;
        entry.text = Stamped(
            context, "Refusé : " + what + timeline +
                         (event.error.empty() ? std::string()
                                              : " (" + event.error + ")"));
        Append(std::move(entry));
        return true;
    }
    // An edit that left the project at the revision the endpoint's previous
    // answer had changed nothing.
    if ((event.tool_class == McpToolClass::JournaledEdit ||
         event.tool_class == McpToolClass::ProjectEdit) &&
        !event.revision.empty() && event.revision == event.previous_revision) {
        return false;
    }

    McpDeltaSummary delta = event.delta;
    bool counted = event.has_delta;
    if (event.delta_as_prepared && !event.plan_id.empty()) {
        // apply_edit_plan's short answer: the plan applied is the plan
        // previewed, the same operations and the same created IDs.
        const auto plan = std::find_if(
            plans_.begin(), plans_.end(),
            [&](const auto& kept) { return kept.first == event.plan_id; });
        if (plan != plans_.end()) {
            delta = plan->second;
            counted = true;
            plans_.erase(plan);
        }
    }
    const std::string what = event.label.empty()
                                 ? Capitalized(AgentToolTitle(event.tool))
                                 : Quoted(event.label);
    std::string line = what + timeline;
    if (counted) {
        const std::string counts = AgentClipCountText(
            delta.changed, delta.created, delta.removed, delta.shifted);
        if (!counts.empty()) {
            line += " — " + counts;
        }
    }
    if (event.background) {
        line += " — en tâche de fond";
    }
    entry.text = Stamped(context, line);
    entry.touched_clip_ids = delta.created_clip_ids;
    entry.touched_clip_ids.insert(entry.touched_clip_ids.end(),
                                  delta.changed_clip_ids.begin(),
                                  delta.changed_clip_ids.end());
    if (!entry.touched_clip_ids.empty()) {
        marked_ = std::set<std::string>(entry.touched_clip_ids.begin(),
                                        entry.touched_clip_ids.end());
    }
    const uint64_t id = Append(std::move(entry));
    if (Intervention* open = InterventionOf(event)) {
        if (!open->stepped) {
            open->stepped = true;
            open->shown_timeline_id = event.shown_timeline_id;
        }
        Insert(open->created, delta.created_clip_ids);
        Insert(open->changed, delta.changed_clip_ids);
        Insert(open->removed, delta.removed_clip_ids);
        open->steps.push_back(id);
    }
    return true;
}

bool AgentJournal::Close(const Event& event,
                         const AgentJournalContext& context) {
    const auto open = std::find_if(
        open_.begin(), open_.end(), [&](const Intervention& candidate) {
            return candidate.token == event.intervention;
        });
    AgentJournalEntry entry;
    if (!event.ok) {
        // Refused, the history is left as it was: the steps stay apart, and
        // the intervention can still be ended.
        const std::string what = event.label.empty()
                                     ? AgentToolTitle(event.tool)
                                     : "regrouper " + Quoted(event.label);
        const std::string timeline =
            open == open_.end()
                ? std::string()
                : TimelineSegment(open->timeline_id, open->shown_timeline_id,
                                  context);
        entry.refused = true;
        entry.text = Stamped(
            context, "Refusé : " + what + timeline +
                         (event.error.empty() ? std::string()
                                              : " (" + event.error + ")"));
        Append(std::move(entry));
        return true;
    }
    std::string line = event.label.empty()
                           ? Capitalized(AgentToolTitle(event.tool))
                           : Quoted(event.label);
    if (open != open_.end()) {
        line += TimelineSegment(
            open->timeline_id,
            open->stepped ? open->shown_timeline_id : event.shown_timeline_id,
            context);
        // What the intervention did as a whole: a clip created and removed
        // within it was never there, one changed then removed is removed,
        // one created then changed is created. A ripple is left out: its
        // rules name no clip, and a clip shifted by two steps cannot be told
        // from two clips shifted once.
        std::vector<std::string> created;
        std::set_difference(open->created.begin(), open->created.end(),
                            open->removed.begin(), open->removed.end(),
                            std::back_inserter(created));
        std::vector<std::string> removed;
        std::set_difference(open->removed.begin(), open->removed.end(),
                            open->created.begin(), open->created.end(),
                            std::back_inserter(removed));
        std::vector<std::string> changed;
        for (const std::string& id : open->changed) {
            if (open->created.count(id) == 0 && open->removed.count(id) == 0) {
                changed.push_back(id);
            }
        }
        const std::string counts =
            AgentClipCountText(static_cast<int64_t>(changed.size()),
                               static_cast<int64_t>(created.size()),
                               static_cast<int64_t>(removed.size()), 0);
        if (!counts.empty()) {
            line += " — " + counts;
        }
        // The steps' entries make way for the one that says them all.
        const std::vector<uint64_t> steps = open->steps;
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const AgentJournalEntry& kept) {
                                          return std::find(
                                                     steps.begin(), steps.end(),
                                                     kept.id) != steps.end();
                                      }),
                       entries_.end());
        entry.touched_clip_ids = created;
        entry.touched_clip_ids.insert(entry.touched_clip_ids.end(),
                                      changed.begin(), changed.end());
        open_.erase(open);
    }
    entry.text = Stamped(context, line);
    if (!entry.touched_clip_ids.empty()) {
        marked_ = std::set<std::string>(entry.touched_clip_ids.begin(),
                                        entry.touched_clip_ids.end());
    }
    Append(std::move(entry));
    return true;
}

AgentJournal::Intervention* AgentJournal::InterventionOf(const Event& event) {
    // Only the timeline's own journal folds: a project edit, an export, an
    // analysis stay entries of their own.
    if (event.tool_class != McpToolClass::JournaledEdit) {
        return nullptr;
    }
    for (auto open = open_.rbegin(); open != open_.rend(); ++open) {
        if (open->connection == event.connection &&
            (event.timeline_id.empty() ||
             event.timeline_id == open->timeline_id)) {
            return &*open;
        }
    }
    return nullptr;
}

uint64_t AgentJournal::Append(AgentJournalEntry entry) {
    entry.id = ++last_id_;
    entries_.push_back(std::move(entry));
    while (entries_.size() > kCapacity) {
        entries_.pop_front();
    }
    return last_id_;
}

void AgentJournal::RememberPlan(const std::string& planId,
                                const McpDeltaSummary& delta) {
    plans_.erase(
        std::remove_if(plans_.begin(), plans_.end(),
                       [&](const auto& kept) { return kept.first == planId; }),
        plans_.end());
    plans_.emplace_back(planId, delta);
    while (plans_.size() > kRememberedPlans) {
        plans_.pop_front();
    }
}

std::vector<chat::ChatTranscriptEntry> AgentJournal::TranscriptEntries() const {
    std::vector<chat::ChatTranscriptEntry> transcript;
    transcript.reserve(entries_.size());
    for (const AgentJournalEntry& entry : entries_) {
        chat::ChatTranscriptEntry line;
        line.kind = chat::ChatEntryKind::AgentJournal;
        line.text = entry.text;
        line.tool_ok = !entry.refused;
        line.journal_id = entry.id;
        transcript.push_back(std::move(line));
    }
    return transcript;
}

bool AgentJournal::DismissMarks() {
    if (marked_.empty()) {
        return false;
    }
    marked_.clear();
    return true;
}
