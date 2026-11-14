/* AFTER-COMP -- Creates only an empty derived project; never closes user work. */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var target;
    if (!project) {
        project = app.newProject();
    }
    if (!project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a créé aucun projet actif."
        );
    }
    if (CUTMACHINE_AFTER.projectPath(project) !== "" ||
            project.numItems !== 0) {
        return CUTMACHINE_AFTER.failure(
            "ProjectAlreadyOpen",
            "Un autre projet After Effects est déjà ouvert."
        );
    }
    target = new File(request.arguments.project_path);
    if (target.exists) {
        return CUTMACHINE_AFTER.failure(
            "ProjectExists", "Le projet After Effects cible existe déjà."
        );
    }
    project.save(target);
    if (!target.exists) {
        return CUTMACHINE_AFTER.failure(
            "SaveFailed", "After Effects n'a pas créé le fichier demandé."
        );
    }
    return CUTMACHINE_AFTER.success(project);
};
