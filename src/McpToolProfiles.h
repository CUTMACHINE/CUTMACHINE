#pragma once
// PROFIL-2026-11 PF1 -- docs/roadmap/PROFIL-2026-11.md. The tools a server
// offers. A client loads the description and schema of every tool a server
// lists into its model's context: the whole catalog weighed about 85,000
// tokens on 9 November 2026 (206 tools), and a client that cannot load tools
// on demand pays it on every turn. A profile names the domains of the
// catalog it serves whole -- one per McpTools*.cc file, as each tool records
// in McpTool::domain -- and the tools it takes from the others.
//
// A profile chooses what a client is shown and may call; it is not a
// permission. A relay to the app (McpEndpoint.h) answers tools/list and
// refuses a call outside the profile itself, but passes a batch on as it is.

#include <string>
#include <vector>

// The domains of the catalog: each McpTools*.cc file registers the tools of
// one, and McpTools.cc those of the session (undo, plans, tasks, motifs).
namespace mcp_domain {
inline constexpr char kEdit[] = "edit";
inline constexpr char kSequence[] = "sequence";
inline constexpr char kClip[] = "clip";
inline constexpr char kCaption[] = "caption";
inline constexpr char kProject[] = "project";
inline constexpr char kSpeech[] = "speech";
inline constexpr char kPicture[] = "picture";
inline constexpr char kColor[] = "color";
inline constexpr char kSound[] = "sound";
inline constexpr char kDelivery[] = "delivery";
inline constexpr char kAfter[] = "after";
inline constexpr char kSession[] = "session";
inline constexpr char kMotion[] = "motion";
inline constexpr char kMotionClip[] = "motion_clip";
inline constexpr char kGeneration[] = "generation";
inline constexpr char kVoice[] = "voice";
}  // namespace mcp_domain

// Every domain above, for a test to tell a misspelt one from a real one.
std::vector<std::string> McpToolDomains();

struct McpToolProfile {
    // What --profile names: "full", "motion", "itw", "reel".
    std::string name;
    // One line, ASCII, said in the server's instructions and in the refusal
    // of a tool outside the profile.
    std::string summary;
    // The whole catalog: what a server serves unless told otherwise.
    bool everything = false;
    // Served whole.
    std::vector<std::string> domains;
    // Served from the other domains, by name.
    std::vector<std::string> tools;
};

// The profiles, the full one first.
const std::vector<McpToolProfile>& McpToolProfiles();
// The full catalog, the default of every server and every command.
const McpToolProfile& FullMcpToolProfile();
// Null for a name no profile has.
const McpToolProfile* FindMcpToolProfile(const std::string& name);
// "full, motion, itw, reel": what a refusal of an unknown name offers
// instead.
std::string McpToolProfileNames();
// Whether `profile` serves the tool `tool` of `domain`.
bool McpToolProfileServes(const McpToolProfile& profile,
                          const std::string& domain, const std::string& tool);
