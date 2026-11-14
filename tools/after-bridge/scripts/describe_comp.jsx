/*
 * AFTER-COMP -- Exposes addressable layer/property metadata so a model can
 * inspect before editing instead of guessing localized names or indices.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    var layers = [];
    var layerIndex;

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

    function keyframes(property) {
        var result = [];
        var index;
        var count;
        var value;
        try {
            count = property.numKeys;
        } catch (ignoredCount) {
            return result;
        }
        for (index = 1; index <= count; index += 1) {
            try {
                value = jsonValue(property.keyValue(index));
            } catch (ignoredValue) {
                value = null;
            }
            result.push({
                frame: CUTMACHINE_AFTER.timeToFrame(
                    comp, property.keyTime(index)
                ),
                value: value
            });
        }
        return result;
    }

    function propertyDescription(property, path) {
        var description = {
            name: String(property.name),
            match_name: String(property.matchName),
            path: path,
            keyframes: keyframes(property)
        };
        try {
            description.expression_enabled = property.expressionEnabled;
        } catch (ignoredExpression) {
            description.expression_enabled = false;
        }
        try {
            description.value = jsonValue(property.value);
            description.readable = true;
        } catch (ignored) {
            description.value = null;
            description.readable = false;
        }
        return description;
    }

    function groupProperties(group, prefix) {
        var result = [];
        var index;
        var property;
        var path;
        if (!group) {
            return result;
        }
        for (index = 1; index <= group.numProperties; index += 1) {
            property = group.property(index);
            path = prefix.concat([String(property.matchName)]);
            if (property.propertyType === PropertyType.PROPERTY) {
                result.push(propertyDescription(property, path));
            }
        }
        return result;
    }

    function effectDescriptions(layer) {
        var effects = layer.property("ADBE Effect Parade");
        var result = [];
        var effectIndex;
        var effect;
        if (!effects) {
            return result;
        }
        for (effectIndex = 1; effectIndex <= effects.numProperties;
                effectIndex += 1) {
            effect = effects.property(effectIndex);
            result.push({
                name: String(effect.name),
                match_name: String(effect.matchName),
                path: ["ADBE Effect Parade", String(effect.matchName)],
                properties: groupProperties(
                    effect,
                    ["ADBE Effect Parade", String(effect.matchName)]
                )
            });
        }
        return result;
    }

    function layerKind(layer) {
        if (layer.property("ADBE Text Properties")) {
            return "text";
        }
        if (layer instanceof CameraLayer) {
            return "camera";
        }
        if (layer instanceof LightLayer) {
            return "light";
        }
        if (layer.nullLayer) {
            return "null";
        }
        if (layer.adjustmentLayer) {
            return "adjustment";
        }
        return "av";
    }

    function summarizeLayer(layer, stackOrder) {
        var sourceId = "";
        var sourceName = "";
        var textProperties = layer.property("ADBE Text Properties");
        var text = null;
        var sourceText;
        try {
            if (layer.source) {
                sourceId = String(layer.source.id);
                sourceName = String(layer.source.name);
            }
        } catch (ignoredSource) {
        }
        if (textProperties) {
            sourceText = textProperties.property("ADBE Text Document");
            if (sourceText) {
                text = String(sourceText.value.text);
            }
        }
        return {
            id: String(layer.id),
            name: String(layer.name),
            kind: layerKind(layer),
            stack_order: stackOrder,
            enabled: layer.enabled,
            locked: layer.locked,
            shy: layer.shy,
            three_d: Boolean(layer.threeDLayer),
            start_frame: CUTMACHINE_AFTER.timeToFrame(comp, layer.startTime),
            in_frame: CUTMACHINE_AFTER.timeToFrame(comp, layer.inPoint),
            out_frame: CUTMACHINE_AFTER.timeToFrame(comp, layer.outPoint),
            source_item_id: sourceId,
            source_name: sourceName,
            text: text
        };
    }

    function describeLayer(layer, stackOrder) {
        var description = summarizeLayer(layer, stackOrder);
        description.transform = groupProperties(
            layer.property("ADBE Transform Group"),
            ["ADBE Transform Group"]
        );
        description.effects = effectDescriptions(layer);
        return description;
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
    if (request.arguments.layer_id !== undefined) {
        var selectedLayer = CUTMACHINE_AFTER.layerById(
            comp, request.arguments.layer_id
        );
        if (!selectedLayer) {
            return CUTMACHINE_AFTER.failure(
                "UnknownLayer", "Le calque demandé est introuvable."
            );
        }
        layers.push(describeLayer(selectedLayer, selectedLayer.index));
    } else {
        for (layerIndex = 1; layerIndex <= comp.numLayers; layerIndex += 1) {
            layers.push(summarizeLayer(comp.layer(layerIndex), layerIndex));
        }
    }
    return CUTMACHINE_AFTER.success(project, {
        comp: {
            id: String(comp.id),
            name: String(comp.name),
            width: comp.width,
            height: comp.height,
            frame_rate: comp.frameRate,
            duration_frames: CUTMACHINE_AFTER.timeToFrame(comp, comp.duration),
            detailed_layer_id: request.arguments.layer_id === undefined ?
                "" : String(request.arguments.layer_id),
            layers: layers
        }
    });
};
