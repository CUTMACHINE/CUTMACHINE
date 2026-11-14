/*
 * AFTER-COMP -- Opens only the working copy prepared by the native bridge.
 * Existing user work is never closed or replaced implicitly.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var current = app.project;
    var working = new File(request.arguments.working_project_path);
    var opened;
    if (current && (CUTMACHINE_AFTER.projectPath(current) !== "" ||
            current.numItems !== 0)) {
        return CUTMACHINE_AFTER.failure(
            "ProjectAlreadyOpen",
            "Un autre projet After Effects est déjà ouvert."
        );
    }
    if (!working.exists) {
        return CUTMACHINE_AFTER.failure(
            "WorkingCopyMissing",
            "La copie de travail After Effects est introuvable."
        );
    }
    opened = app.open(working);
    if (!opened || CUTMACHINE_AFTER.projectPath(opened) !== working.fsName) {
        return CUTMACHINE_AFTER.failure(
            "OpenFailed",
            "After Effects n'a pas ouvert la copie de travail demandée."
        );
    }
    return CUTMACHINE_AFTER.success(opened, {
        source_project_path: request.arguments.source_project_path,
        working_project_path: working.fsName
    });
};
