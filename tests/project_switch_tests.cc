// DEC39 -- docs/roadmap/PHASES-2026-08.md. What « Fichier › Ouvrir un
// projet… » and « Nouveau projet… » decide before the editor reopens on
// another project: which project file a choice names, whether it is the one
// open, whether it can be opened -- readable, not held elsewhere -- whether
// the window is busy, and the command line of the new editor.
//
// The relaunch itself (AppDelegate, applicationWillTerminate) is not run
// here; the detached start it uses is tried in process_launcher_tests.
// Portable: no AppKit, no FFmpeg, no network.

#include "Project.h"
#include "ProjectStorage.h"
#include "ProjectSwitch.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// A package made the way the window makes one, its project file returned.
std::string MakePackage(const std::filesystem::path& folder,
                        const std::string& name) {
    Project project(name);
    project.timelines.front().name = "Timeline 1";
    std::string path;
    std::string error;
    Check(CreatePortableProject(folder.string(), project, path, error),
          "the package '" + name + "' is created: " + error);
    return path;
}

}  // namespace

int main() {
    using project_switch::Outcome;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cutmachine-project-switch-" + GenerateUlid());
    std::filesystem::create_directories(root);
    const std::string current =
        MakePackage(root / "Courant.cutmachine-project", "Courant");
    const std::string other =
        MakePackage(root / "Autre.cutmachine-project", "Autre");

    Test("the project the window holds is already open", [&] {
        const project_switch::Plan byFile =
            project_switch::PlanProjectSwitch(current, current);
        const project_switch::Plan byFolder = project_switch::PlanProjectSwitch(
            current, (root / "Courant.cutmachine-project").string());
        Check(byFile.outcome == Outcome::kAlreadyOpen &&
                  byFolder.outcome == Outcome::kAlreadyOpen,
              "by its file or by its package folder, nothing to do");
    });

    Test("another readable project opens, its folder read as its file", [&] {
        const project_switch::Plan plan = project_switch::PlanProjectSwitch(
            current, (root / "Autre.cutmachine-project").string());
        Check(
            plan.outcome == Outcome::kOpen &&
                std::filesystem::equivalent(plan.project_path, other),
            "the package folder names its project file: " + plan.project_path);
        ProjectSessionLock lock;
        std::string owner;
        std::string error;
        Check(lock.Acquire(other, owner, error),
              "the plan gives the lock back for the new editor: " + error);
        lock.Release();
        Check(!std::filesystem::exists(other + ".lock"),
              "and leaves no lock behind");
        Check(project_switch::PlanProjectSwitch("", other).outcome ==
                  Outcome::kOpen,
              "from a window without a project path too");
    });

    Test("what cannot be opened is refused, in French, before closing", [&] {
        const project_switch::Plan missing = project_switch::PlanProjectSwitch(
            current, (root / "Nulle part.cutmachine-project").string());
        Check(missing.outcome == Outcome::kRefused &&
                  Contains(missing.refusal, "Aucun projet"),
              "nothing at the path: " + missing.refusal);

        std::filesystem::create_directories(root / "Pas un projet");
        const project_switch::Plan folder = project_switch::PlanProjectSwitch(
            current, (root / "Pas un projet").string());
        Check(
            folder.outcome == Outcome::kRefused &&
                Contains(folder.refusal, "project.cutmachine.json") &&
                !std::filesystem::exists(root / "Pas un projet" /
                                         "project.cutmachine.json.lock"),
            "a folder without a project file, no lock made: " + folder.refusal);

        const std::filesystem::path broken = root / "Abîmé.cutmachine-project";
        std::filesystem::create_directories(broken);
        std::ofstream(broken / "project.cutmachine.json") << "{ pas du JSON";
        const project_switch::Plan unreadable =
            project_switch::PlanProjectSwitch(current, broken.string());
        Check(unreadable.outcome == Outcome::kRefused &&
                  Contains(unreadable.refusal, "lisible") &&
                  !std::filesystem::exists(
                      (broken / "project.cutmachine.json").string() + ".lock"),
              "an unreadable project, its lock given back: " +
                  unreadable.refusal);

        ProjectSessionLock elsewhere;
        std::string owner;
        std::string error;
        Check(elsewhere.Acquire(other, owner, error),
              "another program holds the other project: " + error);
        const project_switch::Plan held =
            project_switch::PlanProjectSwitch(current, other);
        Check(held.outcome == Outcome::kRefused &&
                  Contains(held.refusal, "ouvert ailleurs"),
              "a project held elsewhere: " + held.refusal);
        Check(elsewhere.Held(), "and its holder keeps it");
        elsewhere.Release();
    });

    Test("an export or a running task holds the window on its project", [] {
        Check(project_switch::BusyRefusal(false, 0).empty(),
              "nothing running: free to change");
        Check(Contains(project_switch::BusyRefusal(true, 0), "export"),
              "an export is not thrown away");
        Check(Contains(project_switch::BusyRefusal(false, 1), "Une tâche") &&
                  Contains(project_switch::BusyRefusal(false, 3), "3 tâches"),
              "nor the tasks of the task bar, counted");
    });

    Test("the new editor is the executable given the project", [] {
        const std::vector<std::string> arguments =
            project_switch::EditorArgumentsFor(
                "/Applications/CUTMACHINE.app/"
                "Contents/MacOS/cutmachine",
                "/tmp/Autre/project.json");
        Check(
            arguments.size() == 2 && arguments[1] == "/tmp/Autre/project.json",
            "one argument, the project file, as main() reads it");
    });

    // 24 October 2026: `cutmachine <package folder>` said « Impossible
    // d'ouvrir le projet » and quit; main() now reads its argument here.
    Test("a package folder given to the editor names its project file", [&] {
        const std::string folder =
            (root / "Courant.cutmachine-project").string();
        const std::string file =
            std::filesystem::path(current).lexically_normal().string();
        Check(project_switch::ProjectFileOf(folder) == file,
              "the folder stands for the project.cutmachine.json it holds");
        Check(project_switch::ProjectFileOf(folder + "/") == file,
              "with a trailing slash too");
        Check(project_switch::ProjectFileOf(current) == file,
              "the project file is kept as it is");
        const std::string missing =
            (root / "Absent.cutmachine-project").string();
        Check(project_switch::ProjectFileOf(missing) == missing,
              "a path that names nothing is left for the editor to refuse");
    });

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all project switch tests passed\n";
    return 0;
}
