/* AFTER-COMP -- Saves the active project without an implicit Save As. */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var currentPath;
    var target;
    if (!project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a aucun projet actif."
        );
    }
    currentPath = CUTMACHINE_AFTER.projectPath(project);
    if (request.arguments.project_path) {
        target = new File(request.arguments.project_path);
        if (currentPath !== "" && currentPath !== target.fsName) {
            return CUTMACHINE_AFTER.failure(
                "ProjectPathMismatch",
                "Le projet actif est lié à un autre fichier After Effects."
            );
        }
        if (currentPath === "" && target.exists) {
            return CUTMACHINE_AFTER.failure(
                "ProjectExists", "Le projet After Effects cible existe déjà."
            );
        }
        project.save(target);
    } else {
        if (currentPath === "") {
            return CUTMACHINE_AFTER.failure(
                "ProjectPathRequired",
                "Le projet actif n'a pas encore de chemin d'enregistrement."
            );
        }
        project.save();
        target = project.file;
    }
    if (!target || !target.exists) {
        return CUTMACHINE_AFTER.failure(
            "SaveFailed", "After Effects n'a pas enregistré le projet."
        );
    }
    return CUTMACHINE_AFTER.success(project);
};
