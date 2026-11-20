#include "McpToolProfiles.h"

#include <algorithm>

std::vector<std::string> McpToolDomains() {
    using namespace mcp_domain;
    return {kEdit,    kSequence,   kClip,       kCaption,  kProject, kSpeech,
            kPicture, kColor,      kSound,      kDelivery, kAfter,   kSession,
            kMotion,  kMotionClip, kGeneration, kVoice};
}

const std::vector<McpToolProfile>& McpToolProfiles() {
    using namespace mcp_domain;
    static const std::vector<McpToolProfile> profiles = {
        {"full", "every tool of the catalog", true, {}, {}},
        // PF1 -- for an agent that makes motion design and hands it to After
        // Effects: the three domains whole, and from the others what the
        // motion-design procedure names (tests/mcp_tools_tests.cc checks it)
        // and what a composition needs around it -- a project and its
        // timelines, a clip under it, the music it is timed on, a look at the
        // result, its export, undo and the tasks a render runs as.
        {"motion",
         "motion design: compositions, motion clips and After Effects, with "
         "the project, timeline, music and export tools they need",
         false,
         {kMotion, kMotionClip, kAfter},
         {// project
          "get_procedure", "create_project", "open_project", "ingest_media",
          "describe", "list_timelines", "add_timeline", "clone_timeline",
          "remove_timeline", "rename_timeline", "set_active_timeline",
          // edit, sequence
          "insert_clip", "move_clip", "trim_clip", "remove_clip", "add_track",
          // session
          "undo", "redo", "get_task", "list_tasks", "cancel_task",
          // picture
          "read_frame", "contact_sheet",
          // sound: the music a composition is timed on
          "analyze_audio_beats", "list_audio_beats", "list_tempo_grid",
          "list_music_structure",
          // delivery
          "check_timeline", "export_timeline", "export_timelines",
          "verify_export"}},
        // PA10 -- docs/roadmap/PARITE-2026-11.md. For an agent that edits
        // an interview: speech, captions and the session whole, and from
        // the others what the monter-une-itw procedure names
        // (tests/mcp_tools_tests.cc checks it) with the plain edits, the
        // timelines and the delivery a film needs around them.
        {"itw",
         "interview editing: speech, word cuts and captions, with the "
         "project, edit, picture, colour, sound and delivery tools they need",
         false,
         {kSpeech, kCaption, kSession},
         {// project
          "get_procedure", "create_project", "open_project", "ingest_media",
          "describe", "list_timelines", "add_timeline", "clone_timeline",
          "remove_timeline", "rename_timeline", "set_active_timeline",
          "choose_timeline_variant", "create_timeline_variants",
          "query_project_archive",
          // the window: what the person looks at, and their markers
          "get_editor_view",
          // edit
          "insert_clip", "move_clip", "trim_clip", "remove_clip", "split_clip",
          "delete_timeline_range", "paste_clips", "remove_linked_clips",
          "ripple_trim", "roll_edit", "edit_source_range", "delete_gap",
          // sequence
          "update_sequence", "add_track", "add_marker", "update_marker",
          "remove_marker", "add_transition",
          // clip
          "set_clip_framing", "set_clip_mask", "analyze_person_matte",
          "track_subject_framing", "stabilize_clip", "set_clip_audio",
          "set_clip_enabled",
          // picture: the cutaways and the look at a cut
          "analyze_shot_quality", "describe_shots", "inspect_cut",
          "list_stable_windows", "media_sheet", "read_frame", "review_cuts",
          "search_shots", "contact_sheet", "cut_sheet",
          // colour
          "apply_cdl", "conform_color_management", "export_cdl",
          "measure_color", "propose_grade", "set_media_color_input",
          // sound
          "analyze_timeline_audio", "measure_audio_offset", "propose_ducking",
          "sync_audio",
          // delivery
          "check_timeline", "export_timeline", "export_timelines",
          "verify_export", "timeline_stats", "export_resolve_xml",
          "send_to_resolve",
          // motion: a title card laid on the film
          "add_motion_template", "list_motion_templates", "place_motion_clip"}},
        // PA10 -- for an agent that cuts a reel or an ad: a clip's
        // settings, the pictures, the sound, generation, motion clips and
        // the session whole, and from the others what the monter-un-reel
        // procedure names with the edits and the delivery around them.
        {"reel",
         "reels and ads: clip speed, framing and animation, pictures, music "
         "and generation, with the project, edit, colour, caption and "
         "delivery tools they need",
         false,
         {kClip, kPicture, kSound, kGeneration, kMotionClip, kSession},
         {// project
          "get_procedure", "create_project", "open_project", "ingest_media",
          "describe", "list_timelines", "add_timeline", "clone_timeline",
          "remove_timeline", "rename_timeline", "set_active_timeline",
          "get_editor_view",
          // edit
          "insert_clip", "move_clip", "trim_clip", "remove_clip", "split_clip",
          "roll_edit", "ripple_trim", "split_at_interval", "slip_edit",
          "delete_gap", "edit_source_range", "paste_clips",
          // sequence
          "add_track", "add_transition", "update_transition",
          "remove_transition", "conform_sequence", "update_sequence",
          "add_marker",
          // colour
          "conform_color_management", "propose_exposure", "propose_grade",
          "set_sequence_lut", "match_color",
          // delivery
          "check_timeline", "export_timeline", "export_timelines",
          "verify_export", "timeline_stats", "export_resolve_xml",
          "send_to_resolve",
          // motion: a title card laid on the film
          "add_motion_template", "list_motion_templates", "render_motion_frame",
          // speech and captions, for a face to camera
          "get_timeline_transcript", "generate_captions", "add_caption_style",
          "check_caption_legibility"}},
    };
    return profiles;
}

const McpToolProfile& FullMcpToolProfile() { return McpToolProfiles().front(); }

const McpToolProfile* FindMcpToolProfile(const std::string& name) {
    for (const McpToolProfile& profile : McpToolProfiles()) {
        if (profile.name == name) return &profile;
    }
    return nullptr;
}

std::string McpToolProfileNames() {
    std::string names;
    for (const McpToolProfile& profile : McpToolProfiles()) {
        names += (names.empty() ? "" : ", ") + profile.name;
    }
    return names;
}

bool McpToolProfileServes(const McpToolProfile& profile,
                          const std::string& domain, const std::string& tool) {
    if (profile.everything) return true;
    const auto has = [](const std::vector<std::string>& names,
                        const std::string& name) {
        return std::find(names.begin(), names.end(), name) != names.end();
    };
    return has(profile.domains, domain) || has(profile.tools, tool);
}
