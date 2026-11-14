/*
 * AFTER-COMP -- Lists file-backed footage and says which files are gone.
 * replace_footage can repair an offline item, but nothing could find one:
 * a render of missing media returns colour bars, not an error, so a caller
 * that only reads properties believes the composition is fine.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var footage = [];
    var index;
    var item;
    var folder;
    var folders;
    var missing;
    if (!project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a aucun projet actif."
        );
    }
    for (index = 1; index <= project.numItems; index += 1) {
        item = project.item(index);
        if (!(item instanceof FootageItem)) {
            continue;
        }
        // Solids and placeholders have no file and can never be offline;
        // reporting them as missing would bury the items that truly are.
        if (!item.file && !item.footageMissing) {
            continue;
        }
        folders = [];
        folder = item.parentFolder;
        while (folder && folder !== project.rootFolder) {
            folders.unshift(String(folder.name));
            folder = folder.parentFolder;
        }
        missing = item.footageMissing;
        if (!missing && item.file) {
            missing = !item.file.exists;
        }
        footage.push({
            id: String(item.id),
            name: String(item.name),
            path: item.file ? item.file.fsName : "",
            folder_path: folders,
            missing: missing,
            width: item.width,
            height: item.height,
            is_still: item.duration === 0,
            duration_seconds: item.duration
        });
    }
    return CUTMACHINE_AFTER.success(project, {footage: footage});
};
