#include "ProjectSwitch.h"

#include "Project.h"
#include "ProjectStorage.h"

#include <filesystem>
#include <system_error>

namespace project_switch {

std::string ProjectFileOf(const std::string& chosen) {
    std::error_code error;
    const std::filesystem::path path(chosen);
    if (std::filesystem::is_directory(path, error))
        return (path / "project.cutmachine.json").lexically_normal().string();
    return path.lexically_normal().string();
}

Plan PlanProjectSwitch(const std::string& currentProjectPath,
                       const std::string& chosen) {
    Plan plan;
    plan.project_path = ProjectFileOf(chosen);
    std::error_code sameError;
    if (!currentProjectPath.empty() &&
        std::filesystem::equivalent(plan.project_path, currentProjectPath,
                                    sameError)) {
        plan.outcome = Outcome::kAlreadyOpen;
        return plan;
    }
    if (IsMissingProjectPath(plan.project_path)) {
        plan.refusal = "Aucun projet à cet endroit : « " + chosen + " ».";
        return plan;
    }
    // A folder that holds no project file takes no lock beside it.
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(plan.project_path, fileError)) {
        plan.refusal = "Ce n’est pas un projet CUTMACHINE : « " + chosen +
                       " » n’a pas de fichier project.cutmachine.json.";
        return plan;
    }
    // Held by a server, an agent's command or another window, the project
    // would be refused by the new editor, after this one has closed.
    ProjectSessionLock lock;
    std::string owner;
    std::string error;
    if (!lock.Acquire(plan.project_path, owner, error)) {
        plan.refusal = "Ce projet est ouvert ailleurs" +
                       (owner.empty() ? std::string() : " (" + owner + ")") +
                       " : fermez-le là avant de l’ouvrir ici (" + error + ").";
        return plan;
    }
    // Read as the editor reads it, under its lock: a package interrupted
    // in a commit is recovered here as it would be there.
    Project project;
    const bool readable =
        LoadStoredProject(plan.project_path, project, error, false);
    lock.Release();
    if (!readable) {
        plan.refusal =
            "Ce n’est pas un projet CUTMACHINE lisible (" + error + ").";
        return plan;
    }
    plan.outcome = Outcome::kOpen;
    return plan;
}

std::string BusyRefusal(bool exportRunning, size_t runningTasks) {
    if (exportRunning)
        return "Un export est en cours : attendez sa fin, ou annulez-le, "
               "avant de changer de projet.";
    if (runningTasks == 1)
        return "Une tâche tourne encore dans la barre des tâches : attendez "
               "sa fin, ou annulez-la, avant de changer de projet.";
    if (runningTasks > 1)
        return std::to_string(runningTasks) +
               " tâches tournent encore dans la barre des tâches : attendez "
               "leur fin, ou annulez-les, avant de changer de projet.";
    return {};
}

std::vector<std::string> EditorArgumentsFor(const std::string& executable,
                                            const std::string& projectPath) {
    return {executable, projectPath};
}

}  // namespace project_switch
