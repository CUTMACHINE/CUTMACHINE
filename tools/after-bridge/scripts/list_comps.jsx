/* AFTER-COMP -- Lists compositions by host ID, never by project index. */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comps = [];
    var index;
    var item;
    var folder;
    var folders;
    if (!project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a aucun projet actif."
        );
    }
    for (index = 1; index <= project.numItems; index += 1) {
        item = project.item(index);
        if (item instanceof CompItem) {
            folders = [];
            folder = item.parentFolder;
            while (folder && folder !== project.rootFolder) {
                folders.unshift(String(folder.name));
                folder = folder.parentFolder;
            }
            comps.push({
                id: String(item.id),
                name: String(item.name),
                folder_path: folders,
                width: item.width,
                height: item.height,
                frame_rate: item.frameRate,
                duration_frames: CUTMACHINE_AFTER.timeToFrame(
                    item, item.duration
                ),
                layer_count: item.numLayers
            });
        }
    }
    return CUTMACHINE_AFTER.success(project, {comps: comps});
};
