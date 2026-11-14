/*
 * AFTER-COMP -- Writes one still so a caller can look at what it changed
 * instead of inferring it from properties. saveFrameToPng bypasses the render
 * queue: the user's queued items and output modules are never touched.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;

    // saveFrameToPng publishes the file before the bytes land, and a File
    // caches what it learned about the path when it was built. Both make a
    // plain `exists` check report a finished render over an empty file, which
    // is worse than no render at all: the caller would look at nothing and
    // believe it. Wait for the PNG end-of-stream marker on a fresh handle.
    function complete(path) {
        var file = new File(path);
        var tail = "";
        if (!file.exists || file.length < 12) {
            return false;
        }
        file.encoding = "BINARY";
        if (!file.open("r")) {
            return false;
        }
        try {
            file.seek(file.length - 12, 0);
            tail = file.read(12);
        } finally {
            file.close();
        }
        return tail.length === 12 && tail.substr(4, 4) === "IEND";
    }

    function rendered(path) {
        var attempt;
        for (attempt = 0; attempt < 600; attempt += 1) {
            if (complete(path)) {
                return true;
            }
            $.sleep(50);
        }
        return false;
    }

    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    var frame = request.arguments.frame;
    var target;
    var time;
    var duration;
    var resolution = null;
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
    if (typeof comp.saveFrameToPng !== "function") {
        return CUTMACHINE_AFTER.failure(
            "RenderUnavailable",
            "Cette version d'After Effects n'expose pas saveFrameToPng."
        );
    }
    duration = CUTMACHINE_AFTER.timeToFrame(comp, comp.duration);
    if (frame < 0 || frame >= duration) {
        return CUTMACHINE_AFTER.failure(
            "FrameOutOfRange",
            "La frame demandée est hors de la composition (0 à " +
                String(duration - 1) + ")."
        );
    }
    time = CUTMACHINE_AFTER.frameToTime(comp, frame);
    target = new File(request.arguments.image_path);

    // A comp left at half resolution would silently return a smaller image
    // than the caller asked about. Pin full resolution, then put back exactly
    // what the user had. Any scripted write marks the project dirty even when
    // it restores the previous value, so touch the setting only when it is
    // actually wrong: looking at a frame then costs the user nothing.
    try {
        if (comp.resolutionFactor[0] !== 1 || comp.resolutionFactor[1] !== 1) {
            resolution = comp.resolutionFactor;
            comp.resolutionFactor = [1, 1];
        }
        comp.saveFrameToPng(time, target);
    } finally {
        if (resolution !== null) {
            comp.resolutionFactor = resolution;
        }
    }
    if (!rendered(request.arguments.image_path)) {
        return CUTMACHINE_AFTER.failure(
            "RenderFailed",
            "After Effects n'a pas terminé d'écrire l'image demandée."
        );
    }
    return CUTMACHINE_AFTER.success(project, {
        comp_id: String(comp.id),
        frame: frame,
        image_path: target.fsName,
        width: comp.width,
        height: comp.height
    });
};
