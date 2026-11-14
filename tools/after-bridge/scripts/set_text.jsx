/* AFTER-COMP -- Changes source text while preserving the TextDocument style. */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    var layer = CUTMACHINE_AFTER.layerById(comp, request.arguments.layer_id);
    var textGroup;
    var sourceText;
    var document;
    var before;
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
    textGroup = layer.property("ADBE Text Properties");
    sourceText = textGroup ? textGroup.property("ADBE Text Document") : null;
    if (!sourceText) {
        return CUTMACHINE_AFTER.failure(
            "NotTextLayer", "Le calque demandé n'est pas un calque de texte."
        );
    }
    before = sourceText.value;
    if (request.arguments.expected !== undefined &&
            String(before.text) !== request.arguments.expected) {
        return CUTMACHINE_AFTER.staleValue(
            "Le texte", request.arguments.expected, String(before.text)
        );
    }
    document = sourceText.value;
    app.beginUndoGroup("CUTMACHINE : modifier le texte");
    try {
        document.text = request.arguments.text;
        sourceText.setValue(document);
        CUTMACHINE_AFTER.saveIfRequested(request);
        app.endUndoGroup();
        ended = true;
    } catch (error) {
        try {
            sourceText.setValue(before);
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
        property_path: ["ADBE Text Properties", "ADBE Text Document"],
        before: String(before.text),
        after: String(sourceText.value.text)
    });
};
