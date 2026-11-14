/*
 * AFTER-COMP -- Edits one addressable property, statically or at an exact
 * integer frame. The property path uses matchName/name segments, not indices.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    var layer = CUTMACHINE_AFTER.layerById(comp, request.arguments.layer_id);
    var property;
    var time = null;
    var before;
    var existingKey = 0;
    var ended = false;

    function childByAddress(group, address) {
        var found = null;
        var index;
        var child;
        for (index = 1; index <= group.numProperties; index += 1) {
            child = group.property(index);
            if (String(child.matchName) === address ||
                    String(child.name) === address) {
                if (found && found !== child) {
                    throw new Error("ambiguous property segment: " + address);
                }
                found = child;
            }
        }
        return found;
    }

    function resolveProperty(root, path) {
        var current = root;
        var index;
        for (index = 0; index < path.length; index += 1) {
            current = childByAddress(current, String(path[index]));
            if (!current) {
                throw new Error("unknown property segment: " + path[index]);
            }
        }
        if (current.propertyType !== PropertyType.PROPERTY) {
            throw new Error("property_path must address a value property");
        }
        return current;
    }

    function jsonValue(value) {
        var result;
        var index;
        if (value === null || typeof value === "string" ||
                typeof value === "number" || typeof value === "boolean") {
            return value;
        }
        if (value instanceof Array) {
            result = [];
            for (index = 0; index < value.length; index += 1) {
                result.push(jsonValue(value[index]));
            }
            return result;
        }
        return String(value);
    }

    function keyAtTime(target, keyTime) {
        var nearest;
        if (target.numKeys === 0) {
            return 0;
        }
        nearest = target.nearestKeyIndex(keyTime);
        if (Math.abs(target.keyTime(nearest) - keyTime) < 0.0000001) {
            return nearest;
        }
        return 0;
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
    property = resolveProperty(layer, request.arguments.property_path);
    if (property.expressionEnabled) {
        return CUTMACHINE_AFTER.failure(
            "ExpressionActive",
            "Cette propriété est pilotée par une expression active."
        );
    }
    if (request.arguments.frame !== undefined) {
        time = CUTMACHINE_AFTER.frameToTime(comp, request.arguments.frame);
        existingKey = keyAtTime(property, time);
        before = existingKey ? property.keyValue(existingKey) :
            property.valueAtTime(time, false);
    } else {
        if (property.numKeys !== 0) {
            return CUTMACHINE_AFTER.failure(
                "FrameRequired",
                "Cette propriété est animée ; indiquez une frame exacte."
            );
        }
        before = property.value;
    }
    if (request.arguments.expected !== undefined &&
            !CUTMACHINE_AFTER.sameValue(request.arguments.expected,
                jsonValue(before))) {
        return CUTMACHINE_AFTER.staleValue(
            "La propriété", request.arguments.expected, jsonValue(before)
        );
    }
    app.beginUndoGroup("CUTMACHINE : modifier une propriété");
    try {
        if (time === null) {
            property.setValue(request.arguments.value);
        } else {
            property.setValueAtTime(time, request.arguments.value);
        }
        CUTMACHINE_AFTER.saveIfRequested(request);
        app.endUndoGroup();
        ended = true;
    } catch (error) {
        try {
            if (time === null) {
                property.setValue(before);
            } else if (existingKey) {
                property.setValueAtKey(existingKey, before);
            } else {
                property.removeKey(keyAtTime(property, time));
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
        layer_id: String(layer.id),
        property_path: request.arguments.property_path,
        frame: request.arguments.frame === undefined ? null :
            request.arguments.frame,
        before: jsonValue(before),
        after: jsonValue(time === null ? property.value :
            property.valueAtTime(time, false))
    });
};
