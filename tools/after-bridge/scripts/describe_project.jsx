/* AFTER-COMP -- Read-only host diagnostics, never imported into CUTMACHINE. */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    if (!app.project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a aucun projet actif."
        );
    }
    return CUTMACHINE_AFTER.success(app.project, {
        active_item_id: app.project.activeItem ?
            String(app.project.activeItem.id) : ""
    });
};
