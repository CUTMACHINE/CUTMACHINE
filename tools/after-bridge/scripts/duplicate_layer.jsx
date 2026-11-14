/*
 * AFTER-COMP -- Copies a layer with its animation intact. This is how a card
 * gains a second line without reverse-engineering the keyframes that make the
 * first one move.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    var layer = CUTMACHINE_AFTER.layerById(comp, request.arguments.layer_id);
    var copy = null;
    var wasLocked = false;
    var ended = false;
    if (!project) {
        return CUTMACHINE_AFTER.failure(
            "NoProject", "After Effects n'a aucun projet actif."
        );
    }
    if (!comp) {
        return CUTMACHINE_AFTER.failure(
            "UnknownComp", "La composition demandée est introuvable."
        );
    }
    if (!layer) {
        return CUTMACHINE_AFTER.failure(
            "UnknownLayer", "Le calque demandé est introuvable."
        );
    }
    wasLocked = layer.locked;
    app.beginUndoGroup("CUTMACHINE : dupliquer un calque");
    try {
        layer.locked = false;
        copy = layer.duplicate();
        layer.locked = wasLocked;
        CUTMACHINE_AFTER.saveIfRequested(request);
        app.endUndoGroup();
        ended = true;
    } catch (error) {
        try {
            if (copy) {
                copy.locked = false;
                copy.remove();
            }
            layer.locked = wasLocked;
        } catch (ignored) {
        }
        if (!ended) {
            app.endUndoGroup();
        }
        throw error;
    }
    return CUTMACHINE_AFTER.success(project, {
        comp_id: String(comp.id),
        source_layer_id: String(layer.id),
        layer_id: String(copy.id),
        name: String(copy.name),
        index: copy.index,
        locked: copy.locked
    });
};
