// ACP1, ACP2 -- docs/roadmap/ACP-2026-09.md. The whole chain, with real
// processes: the panel's ACP client starts an agent (tests/fake_acp_agent.py,
// which does what Claude Code or Codex do with a session and nothing more),
// hands it the engine's MCP server exactly as the app does (this binary in
// `--mcp-serve <project> --stdio`), the agent asks before its call, and the
// marker it asks for lands in the project through the engine. Then a turn
// stopped from another thread, as the panel's Stop button does.
//
// The binary is run, and McpProjectBackend needs CommonCrypto
// (ProjectStorage), so like tests/mcp_endpoint_tests.cc this builds on macOS
// only.

#include "AcpClient.h"
#include "AcpProcess.h"
#include "ChatTranscript.h"
#include "McpProjectBackend.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

Document Fixture() {
    Document document;
    document.sequence.id = "01K30000000000000000000005";
    document.sequence.name = "MONTAGE";
    document.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000002",
         "video",
         0,
         {{"01K30000000000000000000003",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}}}},
    };
    return document;
}

std::string TranscriptText(const acp::AcpChatSession& session) {
    std::string text;
    for (const chat::ChatTranscriptEntry& entry : session.Transcript()) {
        text += std::to_string(static_cast<int>(entry.kind)) + " " +
                entry.tool_name + " " + entry.text + "\n";
    }
    return text;
}

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        (GenerateUlid() + "-acp-agent-tests");
    std::filesystem::create_directory(directory);
    std::string error;
    Project project = Project::FromDocument(Fixture(), "ACP fixture");
    std::string projectPath;
    Check(CreatePortableProject((directory / "Acp.cutmachine-project").string(),
                                project, projectPath, error),
          "fixture package saves: " + error);

    acp::AcpSessionConfig config;
    config.cwd = directory.string();
    config.mcp_servers.push_back({"cutmachine",
                                  CUTMACHINE_EXECUTABLE,
                                  {"--mcp-serve", projectPath, "--stdio"},
                                  {}});
    config.preamble = "Contexte de test";
    const std::filesystem::path log = directory / "agent.stderr.log";
    const std::string command = std::string("'") + CUTMACHINE_PYTHON + "' '" +
                                CUTMACHINE_FAKE_ACP_AGENT + "'";
    const acp::AcpTransportFactory factory =
        [&](std::string& startError) -> std::unique_ptr<acp::AcpTransport> {
        acp::AcpProcessOptions options;
        options.command = command;
        options.cwd = directory.string();
        options.environment = acp::CurrentEnvironment();
        acp::StripNestedAgentVariables(options.environment);
        options.stderr_path = log.string();
        return acp::AcpProcessTransport::Start(options, startError);
    };

    // ---- An engine call, granted, applied by the engine ------------------
    {
        acp::AcpChatSession session(factory, config);
        const bool ok = session.SubmitUserMessage(
            R"({"call":{"tool":"add_marker","arguments":{"timeline_id":"01K30000000000000000000005","name":"POSE PAR ACP","time":{"value":50,"rate":25}}}})",
            error);
        Check(ok, "the turn ends normally: " + error + "\n" +
                      TranscriptText(session) + Read(log));
        const auto& transcript = session.Transcript();
        Check(transcript.size() == 4,
              "user, text, call, result:\n" + TranscriptText(session));
        if (transcript.size() == 4) {
            Check(transcript[1].text == "blocs: 2",
                  "the preamble went with the instruction: " +
                      transcript[1].text);
            Check(transcript[2].kind == chat::ChatEntryKind::ToolCall &&
                      transcript[2].tool_name == "add_marker",
                  "the call, by the tool's name: " + transcript[2].tool_name);
            Check(transcript[3].kind == chat::ChatEntryKind::ToolResult &&
                      transcript[3].tool_ok,
                  "granted without asking and applied: " + transcript[3].text);
        }
        session.Shutdown();
    }
    {
        McpProjectBackend reader;
        std::string opened;
        std::string errorName;
        Check(reader.OpenProject(projectPath, opened, errorName, error),
              "the project opens once the agent is gone: " + error);
        std::string described;
        std::string message;
        Check(reader.Describe(described, message) &&
                  described.find("POSE PAR ACP") != std::string::npos,
              "the marker is in the project: " + message);
    }

    // ---- Stop from another thread, as the panel's button does -------------
    {
        std::atomic_bool working{false};
        acp::AcpChatSession session(factory, config);
        session.SetTrace([&working](bool outgoing, const std::string& line) {
            if (!outgoing && line.find("session/update") != std::string::npos)
                working = true;
        });
        bool ok = true;
        std::string turnError;
        std::thread turn([&] {
            ok = session.SubmitUserMessage(R"({"wait_for_cancel":true})",
                                           turnError);
        });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!working && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        Check(working, "the agent started its turn");
        session.RequestStop();
        turn.join();
        Check(!ok && turnError == "tour arrêté à la demande de l'utilisateur",
              "the turn stops as cancelled: " + turnError);
        Check(
            !session.Transcript().empty() &&
                session.Transcript().back().kind == chat::ChatEntryKind::Error,
            "and says so");
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All ACP agent tests passed\n";
    return 0;
}
