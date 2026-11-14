/*
 * AFTER-COMP -- Places an already-imported item into a composition. Adding a
 * layer and importing a file stay separate so a failed placement never leaves
 * an orphan item, and so the same media can be placed twice.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    var item = CUTMACHINE_AFTER.itemById(request.arguments.item_id);
    var layer = null;
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
    if (!item) {
        return CUTMACHINE_AFTER.failure(
            "UnknownItem", "L'élément demandé est introuvable dans le projet."
        );
    }
    if (!(item instanceof FootageItem) && !(item instanceof CompItem)) {
        return CUTMACHINE_AFTER.failure(
            "InvalidLayerSource",
            "Seuls un métrage ou une composition peuvent devenir un calque."
        );
    }
    if (item === comp) {
        return CUTMACHINE_AFTER.failure(
            "InvalidLayerSource",
            "Une composition ne peut pas se contenir elle-même."
        );
    }
    app.beginUndoGroup("CUTMACHINE : ajouter un calque");
    try {
        layer = comp.layers.add(item);
        if (request.arguments.index !== undefined) {
            if (request.arguments.index < 1 ||
                    request.arguments.index > comp.numLayers) {
                throw new Error(
                    "index must be between 1 and " + String(comp.numLayers)
                );
            }
            // moveTo belongs to Property, not Layer: a layer is restacked
            // relative to another layer. Which side to move to depends on the
            // direction, because the target index shifts once this layer
            // leaves its current slot.
            if (request.arguments.index > layer.index) {
                layer.moveAfter(comp.layer(request.arguments.index));
            } else if (request.arguments.index < layer.index) {
                layer.moveBefore(comp.layer(request.arguments.index));
            }
        }
        if (request.arguments.start_frame !== undefined) {
            layer.startTime = CUTMACHINE_AFTER.frameToTime(
                comp, request.arguments.start_frame
            );
        }
        if (request.arguments.in_frame !== undefined) {
            layer.inPoint = CUTMACHINE_AFTER.frameToTime(
                comp, request.arguments.in_frame
            );
        }
        if (request.arguments.out_frame !== undefined) {
            layer.outPoint = CUTMACHINE_AFTER.frameToTime(
                comp, request.arguments.out_frame
            );
        }
        if (layer.inPoint >= layer.outPoint) {
            throw new Error("in_frame must be lower than out_frame");
        }
        CUTMACHINE_AFTER.saveIfRequested(request);
        app.endUndoGroup();
        ended = true;
    } catch (error) {
        try {
            if (layer) {
                layer.locked = false;
                layer.remove();
            }
        } catch (ignored) {
        }
        if (!ended) {
            app.endUndoGroup();
        }
        throw error;
    }
    return CUTMACHINE_AFTER.success(project, {
        comp_id: String(comp.id),
        item_id: String(item.id),
        layer_id: String(layer.id),
        name: String(layer.name),
        index: layer.index,
        start_frame: CUTMACHINE_AFTER.timeToFrame(comp, layer.startTime),
        in_frame: CUTMACHINE_AFTER.timeToFrame(comp, layer.inPoint),
        out_frame: CUTMACHINE_AFTER.timeToFrame(comp, layer.outPoint)
    });
};
