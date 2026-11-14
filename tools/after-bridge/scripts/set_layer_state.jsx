/* AFTER-COMP -- Changes visible/timing layer state using exact frame inputs. */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    var layer = CUTMACHINE_AFTER.layerById(comp, request.arguments.layer_id);
    var before;
    var after;
    var key;
    var ended = false;

    function snapshot(target) {
        return {
            enabled: target.enabled,
            locked: target.locked,
            start_frame: CUTMACHINE_AFTER.timeToFrame(comp, target.startTime),
            in_frame: CUTMACHINE_AFTER.timeToFrame(comp, target.inPoint),
            out_frame: CUTMACHINE_AFTER.timeToFrame(comp, target.outPoint)
        };
    }

    function restore(target, state) {
        target.locked = false;
        target.enabled = state.enabled;
        target.startTime = CUTMACHINE_AFTER.frameToTime(
            comp, state.start_frame
        );
        target.inPoint = CUTMACHINE_AFTER.frameToTime(comp, state.in_frame);
        target.outPoint = CUTMACHINE_AFTER.frameToTime(comp, state.out_frame);
        target.locked = state.locked;
    }

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
    before = snapshot(layer);
    if (request.arguments.expected !== undefined) {
        for (key in request.arguments.expected) {
            if (request.arguments.expected.hasOwnProperty(key) &&
                    request.arguments.expected[key] !== before[key]) {
                return CUTMACHINE_AFTER.staleValue(
                    "L'état « " + key + " » du calque",
                    request.arguments.expected[key], before[key]
                );
            }
        }
    }
    app.beginUndoGroup("CUTMACHINE : modifier le calque");
    try {
        layer.locked = false;
        if (request.arguments.enabled !== undefined) {
            layer.enabled = request.arguments.enabled;
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
        if (request.arguments.locked !== undefined) {
            layer.locked = request.arguments.locked;
        } else {
            layer.locked = before.locked;
        }
        CUTMACHINE_AFTER.saveIfRequested(request);
        app.endUndoGroup();
        ended = true;
    } catch (error) {
        try {
            restore(layer, before);
        } catch (ignored) {
        }
        if (!ended) {
            app.endUndoGroup();
        }
        throw error;
    }
    after = snapshot(layer);
    return CUTMACHINE_AFTER.success(project, {
        comp_id: String(comp.id),
        layer_id: String(layer.id),
        before: before,
        after: after
    });
};
