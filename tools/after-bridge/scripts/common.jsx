/*
 * AFTER-COMP -- Shared protocol, addressing and response-mailbox helpers.
 * The active .aep is an explicit external workspace; no handler mutates a
 * CUTMACHINE Document.
 */
var CUTMACHINE_AFTER = (function () {
    function hex4(value) {
        var text = value.toString(16);
        while (text.length < 4) {
            text = "0" + text;
        }
        return text;
    }

    function quote(value) {
        var text = String(value);
        var result = "\"";
        var index;
        var code;
        var character;
        for (index = 0; index < text.length; index += 1) {
            character = text.charAt(index);
            code = text.charCodeAt(index);
            if (character === "\"" || character === "\\") {
                result += "\\" + character;
            } else if (character === "\b") {
                result += "\\b";
            } else if (character === "\f") {
                result += "\\f";
            } else if (character === "\n") {
                result += "\\n";
            } else if (character === "\r") {
                result += "\\r";
            } else if (character === "\t") {
                result += "\\t";
            } else if (code < 32 || code === 0x2028 || code === 0x2029) {
                result += "\\u" + hex4(code);
            } else {
                result += character;
            }
        }
        return result + "\"";
    }

    function stringify(value) {
        var parts;
        var index;
        var key;
        if (value === null) {
            return "null";
        }
        if (value === undefined) {
            return "null";
        }
        if (typeof value === "string") {
            return quote(value);
        }
        if (typeof value === "number" || typeof value === "boolean") {
            if (typeof value === "number" && !isFinite(value)) {
                return "null";
            }
            return String(value);
        }
        if (value instanceof Array) {
            parts = [];
            for (index = 0; index < value.length; index += 1) {
                parts.push(stringify(value[index]));
            }
            return "[" + parts.join(",") + "]";
        }
        parts = [];
        for (key in value) {
            if (value.hasOwnProperty(key)) {
                parts.push(quote(key) + ":" + stringify(value[key]));
            }
        }
        return "{" + parts.join(",") + "}";
    }

    function failure(code, detail) {
        return {
            schema: "after-response.v2",
            ok: false,
            error: code,
            detail: detail
        };
    }

    // Pont After -- a value read back, as the caller last saw it: numbers
    // equal to a millionth of their size (they went through a decimal
    // spelling and back), strings and booleans exactly, arrays item by item.
    // After reads a 2D layer's position as [x, y, 0] and takes [x, y] when
    // it is written: an expected array shorter than the value read matches
    // it when what it leaves out is zero.
    function sameValue(expected, actual) {
        var index;
        if (expected instanceof Array || actual instanceof Array) {
            if (!(expected instanceof Array) || !(actual instanceof Array) ||
                    expected.length > actual.length) {
                return false;
            }
            for (index = 0; index < actual.length; index += 1) {
                if (!sameValue(index < expected.length ? expected[index] : 0,
                        actual[index])) {
                    return false;
                }
            }
            return true;
        }
        if (typeof expected === "number" && typeof actual === "number") {
            return Math.abs(expected - actual) <=
                1e-6 * Math.max(1, Math.abs(expected), Math.abs(actual));
        }
        return expected === actual;
    }

    // The refusal of a write whose value moved in After since it was read:
    // nothing is written, and the value found is given back.
    function staleValue(what, expected, actual) {
        return failure(
            "StaleValue",
            what + " a changé dans After depuis sa lecture : attendu " +
                stringify(expected) + ", trouvé " + stringify(actual) + "."
        );
    }

    function projectPath(project) {
        return project && project.file ? project.file.fsName : "";
    }

    function projectName(project) {
        var name = project && project.file ? project.file.name : project.name;
        if (!name) {
            return "Sans titre";
        }
        return String(name).replace(/\.aep$/i, "");
    }

    function success(project, result) {
        var response = {
            schema: "after-response.v2",
            ok: true,
            project_name: projectName(project),
            path: projectPath(project),
            item_count: project.numItems,
            after_version: String(app.version)
        };
        if (result !== undefined) {
            response.result = result;
        }
        return response;
    }

    function itemById(id) {
        var index;
        var item;
        if (!app.project) {
            return null;
        }
        for (index = 1; index <= app.project.numItems; index += 1) {
            item = app.project.item(index);
            if (String(item.id) === String(id)) {
                return item;
            }
        }
        return null;
    }

    function compById(id) {
        var item = itemById(id);
        return item && item instanceof CompItem ? item : null;
    }

    function layerById(comp, id) {
        var index;
        var layer;
        if (!comp) {
            return null;
        }
        for (index = 1; index <= comp.numLayers; index += 1) {
            layer = comp.layer(index);
            if (String(layer.id) === String(id)) {
                return layer;
            }
        }
        return null;
    }

    function frameToTime(comp, frame) {
        var time;
        var roundTrip;
        if (typeof frame !== "number" || frame % 1 !== 0) {
            throw new Error("frame must be an exact integer");
        }
        time = frame * comp.frameDuration;
        roundTrip = Math.round(time / comp.frameDuration);
        if (roundTrip !== frame) {
            throw new Error("frame does not round-trip through After time");
        }
        return time;
    }

    function timeToFrame(comp, time) {
        return Math.round(time / comp.frameDuration);
    }

    function saveIfRequested(request) {
        var shouldSave = request.arguments.save;
        if (shouldSave === undefined) {
            shouldSave = true;
        }
        if (!shouldSave) {
            return;
        }
        if (!app.project || !app.project.file) {
            throw new Error("the active project has no save path");
        }
        app.project.save();
    }

    function writeResponse(text) {
        var output = new File(CUTMACHINE_AFTER_RESPONSE_PATH);
        var temporary = new File(CUTMACHINE_AFTER_RESPONSE_PATH + ".tmp");
        temporary.encoding = "UTF-8";
        temporary.lineFeed = "Unix";
        if (!temporary.open("w")) {
            return false;
        }
        temporary.write(text);
        temporary.close();
        if (output.exists) {
            output.remove();
        }
        return temporary.rename(output.name);
    }

    function execute(request, handler) {
        var response;
        try {
            response = handler(request);
        } catch (error) {
            response = failure("AfterScriptError", String(error));
        }
        writeResponse(stringify(response));
        return 1;
    }

    return {
        compById: compById,
        execute: execute,
        failure: failure,
        frameToTime: frameToTime,
        itemById: itemById,
        layerById: layerById,
        projectPath: projectPath,
        sameValue: sameValue,
        saveIfRequested: saveIfRequested,
        staleValue: staleValue,
        stringify: stringify,
        timeToFrame: timeToFrame,
        success: success
    };
}());
