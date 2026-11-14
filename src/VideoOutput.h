#pragma once

// CF1 -- docs/roadmap/CLEAN-FEED-2026-10.md. A screen is a local output
// destination, never an object in the project. The host supplies stable
// display UUIDs; keeping the choice when disconnected allows reconnection
// without moving the feed onto the editor's screen.

#include <cstdint>
#include <string>
#include <vector>

struct VideoOutputScreen {
    std::string id;
    std::string name;
    int64_t width = 0;
    int64_t height = 0;
    bool editor_screen = false;
};

struct VideoOutputState {
    std::vector<VideoOutputScreen> screens;
    std::string selected_screen_id;
    std::string active_screen_id;
};

inline bool ValidateVideoOutputScreen(const std::string& id,
                                      const VideoOutputState& state,
                                      std::string& message) {
    if (id.empty()) {
        return true;
    }
    for (const VideoOutputScreen& screen : state.screens) {
        if (screen.id != id) {
            continue;
        }
        if (!screen.editor_screen) {
            return true;
        }
        message = "choose a screen other than the editor's screen";
        return false;
    }
    message =
        "the selected screen is not connected; read get_video_output "
        "for the available screen IDs";
    return false;
}
