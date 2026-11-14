/* AFTER-COMP -- Replaces one file-backed footage item addressed by host ID. */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var footage = CUTMACHINE_AFTER.itemById(request.arguments.footage_id);
    var media = new File(request.arguments.media_path);
    var previous;
    var ended = false;
    if (!project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a aucun projet actif."
        );
    }
    if (!footage || !(footage instanceof FootageItem) || !footage.file) {
        return CUTMACHINE_AFTER.failure(
            "UnknownFootage",
            "Le métrage demandé est introuvable ou n'est pas lié à un fichier."
        );
    }
    if (!media.exists) {
        return CUTMACHINE_AFTER.failure(
            "MediaMissing", "Le nouveau fichier média est introuvable."
        );
    }
    previous = footage.file;
    app.beginUndoGroup("CUTMACHINE : remplacer le métrage");
    try {
        footage.replace(media);
        CUTMACHINE_AFTER.saveIfRequested(request);
        app.endUndoGroup();
        ended = true;
    } catch (error) {
        try {
            footage.replace(previous);
        } catch (ignored) {
        }
        if (!ended) {
            app.endUndoGroup();
        }
        throw error;
    }
    return CUTMACHINE_AFTER.success(project, {
        footage_id: String(footage.id),
        before: previous.fsName,
        after: footage.file.fsName
    });
};
