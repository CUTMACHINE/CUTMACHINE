/*
 * AFTER-COMP -- Brings one media file into the active project as footage.
 * Import is deliberately single-file: a numbered still stays one still rather
 * than becoming an image sequence the caller never asked for.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var media = new File(request.arguments.media_path);
    var folderName = request.arguments.folder_name;
    var options;
    var item = null;
    var folder = null;
    var createdFolder = null;
    var index;
    var candidate;
    var ended = false;
    if (!project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a aucun projet actif."
        );
    }
    if (!media.exists) {
        return CUTMACHINE_AFTER.failure(
            "MediaMissing", "Le fichier média est introuvable."
        );
    }
    options = new ImportOptions(media);
    if (!options.canImportAs(ImportAsType.FOOTAGE)) {
        return CUTMACHINE_AFTER.failure(
            "UnsupportedMedia",
            "After Effects ne sait pas importer ce fichier comme métrage."
        );
    }
    options.importAs = ImportAsType.FOOTAGE;
    try {
        options.sequence = false;
    } catch (ignoredSequence) {
    }
    app.beginUndoGroup("CUTMACHINE : importer un média");
    try {
        if (folderName !== undefined) {
            for (index = 1; index <= project.numItems; index += 1) {
                candidate = project.item(index);
                if (candidate instanceof FolderItem &&
                        String(candidate.name) === String(folderName) &&
                        candidate.parentFolder === project.rootFolder) {
                    folder = candidate;
                    break;
                }
            }
            if (!folder) {
                folder = project.items.addFolder(String(folderName));
                createdFolder = folder;
            }
        }
        item = project.importFile(options);
        if (folder) {
            item.parentFolder = folder;
        }
        CUTMACHINE_AFTER.saveIfRequested(request);
        app.endUndoGroup();
        ended = true;
    } catch (error) {
        try {
            if (item) {
                item.remove();
            }
            if (createdFolder) {
                createdFolder.remove();
            }
        } catch (ignored) {
        }
        if (!ended) {
            app.endUndoGroup();
        }
        throw error;
    }
    return CUTMACHINE_AFTER.success(project, {
        footage_id: String(item.id),
        name: String(item.name),
        media_path: media.fsName,
        width: item.width,
        height: item.height,
        is_still: item.duration === 0,
        duration_seconds: item.duration,
        folder_name: folder ? String(folder.name) : ""
    });
};
