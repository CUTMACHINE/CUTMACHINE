/*
 * MOTION-AFTER-2026-09 -- builds an After Effects project from the
 * after-motion.v1 description `cutmachine --export-motion-after` writes
 * (src/motion/MotionAfterExport.h). Every node is a layer or a precomposition and
 * every keyframe a keyframe: the project is there to be retouched.
 *
 * Two ways in. Through the bridge (MA1, method build_motion_project, tool
 * export_motion_after): common.jsx precedes this file and the handler below
 * builds into the empty project After has open, like create_project, and
 * saves it; a project with work in it is refused, never closed. By hand
 * (File > Scripts > Run Script File...): pick the description, or set
 * CUTMACHINE_MOTION_DESCRIPTION (and optionally CUTMACHINE_MOTION_RESPONSE,
 * a JSON file it answers in) before evaluating this file; unsaved work then
 * stays where it is and the composition is built into it, in a folder named
 * with the hour, saving nothing.
 *
 * Host traps met on After 26: a property reference dies as soon as a sibling
 * is added under the same group, so every group is looked up again after an
 * addProperty; a new project over unsaved work opens a modal dialog, which
 * stops After answering until someone clicks. ExtendScript reads a nested
 * conditional (a ? b : c ? d : e) from the left, as (a ? b : c) ? d : e,
 * which turned every centred text right-justified: no nested conditional
 * here, tables.
 */

function CUTMACHINE_MOTION_READ(path) {
    var file = new File(path);
    file.encoding = "UTF-8";
    if (!file.open("r")) {
        throw new Error("lecture impossible : " + path);
    }
    var text = file.read();
    file.close();
    return text;
}

// Builds description `d` into the open project and returns its main
// composition. `stamp` names the folder when the build lands in someone's
// project; `progress.layers` counts the layers made, also after a failure,
// `progress.rows` the texts a row moves by expression (MA3) and
// `progress.rows_fixed` those left in their fixed places.
function CUTMACHINE_MOTION_BUILD(d, stamp, progress) {
    if (d.schema !== "after-motion.v1") {
        throw new Error("schéma inconnu : " + d.schema);
    }
    app.beginUndoGroup("CUTMACHINE motion");
    try {
        var fps = d.fps_numerator / d.fps_denominator;
        var frameDuration = d.fps_denominator / d.fps_numerator;
        // MOTION-WEB-2026-10 MW2 -- a key between two frames comes as "n/d"
        // frames, the motion format's spelling: After keeps a key set by
        // script at that instant (checked in After 26), one division away
        // from the exact time.
        var t = function (frame) {
            if (typeof frame === "string") {
                var parts = frame.split("/");
                return (Number(parts[0]) * d.fps_denominator) /
                    (Number(parts[1]) * d.fps_numerator);
            }
            return frame * frameDuration;
        };
        var folder = app.project.items.addFolder(
            d.name + " (CUTMACHINE" + (stamp ? " " + stamp : "") + ")");
        var precompFolder = app.project.items.addFolder("précompositions");
        precompFolder.parentFolder = folder;

        var footage = {};
        var i;
        for (i = 0; i < d.footage.length; i += 1) {
            var imported = app.project.importFile(
                new ImportOptions(new File(d.footage[i].path)));
            imported.parentFolder = folder;
            footage[d.footage[i].id] = imported;
        }

        // Every composition first: a precomposition layer needs its item.
        var comps = {};
        for (i = 0; i < d.comps.length; i += 1) {
            var c = d.comps[i];
            var item = app.project.items.addComp(
                c.name, c.width, c.height, 1, c.duration * frameDuration, fps);
            item.parentFolder = c.id === d.main ? folder : precompFolder;
            item.bgColor = [0, 0, 0];
            if (d.motion_blur) {
                // MA12 -- the engine's shutter: its instants, its angle, and
                // phase 0 (it opens on the frame; After's -90 shows a fast
                // move's start a frame late). The sample count is fixed:
                // the engine never adapts it.
                var samples = Math.max(2, Math.min(64, d.motion_blur.samples));
                item.motionBlur = true;
                item.shutterAngle = d.motion_blur.shutter_angle;
                item.shutterPhase = d.motion_blur.shutter_phase;
                item.motionBlurSamplesPerFrame = samples;
                item.motionBlurAdaptiveSampleLimit = samples;
            }
            comps[c.id] = item;
        }

        var INTERPOLATION = {
            hold: KeyframeInterpolationType.HOLD,
            linear: KeyframeInterpolationType.LINEAR,
            bezier: KeyframeInterpolationType.BEZIER
        };

        function eases(list, count) {
            var out = [];
            for (var e = 0; e < count; e += 1) {
                var pair = list && list.length
                    ? list[Math.min(e, list.length - 1)]
                    : [0, 16.666667];
                out.push(new KeyframeEase(pair[0], Math.max(0.1, pair[1])));
            }
            return out;
        }

        // Keyframes on one property; `pick` turns the description's value
        // array into what the property takes.
        function keyframes(property, keys, pick) {
            var k;
            var times = [];
            var values = [];
            for (k = 0; k < keys.length; k += 1) {
                times.push(t(keys[k].f));
                values.push(pick(keys[k].v));
            }
            // One call for every value; keys are then linear, which only
            // the others need changing from.
            property.setValuesAtTimes(times, values);
            for (k = 0; k < keys.length; k += 1) {
                if (keys[k]["in"] === "linear" && keys[k].out === "linear") {
                    continue;
                }
                var index = property.nearestKeyIndex(t(keys[k].f));
                // MW2 -- the eases first, then the types: an ease set on a
                // linear side turned it into a curve (a straight segment
                // before an eased one slowed into its key, measured in
                // After 26). A side without an ease keeps the one After
                // gave it.
                if (keys[k].ease_in || keys[k].ease_out) {
                    property.setTemporalEaseAtKey(
                        index,
                        keys[k].ease_in
                            ? eases(keys[k].ease_in,
                                property.keyInTemporalEase(index).length)
                            : property.keyInTemporalEase(index),
                        keys[k].ease_out
                            ? eases(keys[k].ease_out,
                                property.keyOutTemporalEase(index).length)
                            : property.keyOutTemporalEase(index));
                }
                property.setInterpolationTypeAtKey(
                    index, INTERPOLATION[keys[k]["in"]],
                    INTERPOLATION[keys[k].out]);
            }
        }

        function one(v) {
            return v[0];
        }

        function both(v) {
            return [v[0], v[1]];
        }

        function color(rgb, alpha) {
            return [rgb[0], rgb[1], rgb[2], alpha === undefined ? 1 : alpha];
        }

        // MA11 -- a colour key's value, [r, g, b, 1].
        function colour(v) {
            return [v[0], v[1], v[2], v.length > 3 ? v[3] : 1];
        }

        // MA11 -- a mask's path: vertices, tangents, closed.
        function shapeOf(spec) {
            var shape = new Shape();
            shape.vertices = spec.vertices;
            shape.inTangents = spec["in"];
            shape.outTangents = spec.out;
            shape.closed = spec.closed !== false;
            return shape;
        }

        // MA11 -- a clip-path, and the box a picture covers or a group
        // hides its overflow on: masks, the second intersecting the first,
        // their paths keyed where they move.
        function masks(layer, list) {
            for (var m = 0; m < list.length; m += 1) {
                layer.property("ADBE Mask Parade").addProperty("ADBE Mask Atom");
                var mask = layer.property("ADBE Mask Parade").property(m + 1);
                mask.maskMode = list[m].mode === "intersect"
                    ? MaskMode.INTERSECT
                    : MaskMode.ADD;
                var path = layer.property("ADBE Mask Parade").property(m + 1)
                    .property("ADBE Mask Shape");
                path.setValue(shapeOf(list[m].shape));
                if (list[m].keys) {
                    keyframes(path, list[m].keys, shapeOf);
                }
            }
        }

        // MA11 -- a sound's volume: the engine's linear gain on a slider,
        // which the levels read in decibels, so its curve stays the
        // engine's.
        function volume(layer, spec) {
            var levels = layer.property("ADBE Audio Group")
                .property("ADBE Audio Levels");
            levels.setValue([spec.gain_db || 0, spec.gain_db || 0]);
            if (!spec.volume_keys) {
                return;
            }
            var read = slider(layer, "volume",
                spec.volume_keys.length ? spec.volume_keys[0].v[0] : 1);
            keyframes(read(), spec.volume_keys, one);
            layer.property("ADBE Audio Group").property("ADBE Audio Levels")
                .expression =
                "// volume (MA11) : le gain du moteur, linéaire (1 = tel quel).\n" +
                "var g = effect(\"volume\")(1);\n" +
                "var db = g > 0 ? 20 * Math.log(g) / Math.LN10 : -192;\n" +
                "[db, db]";
        }

        function transform(layer, tr) {
            var group = layer.property("ADBE Transform Group");
            group.property("ADBE Anchor Point").setValue(tr.anchor);
            var position = group.property("ADBE Position");
            position.dimensionsSeparated = true;
            var x = group.property("ADBE Position_0");
            var y = group.property("ADBE Position_1");
            x.setValue(tr.position[0]);
            y.setValue(tr.position[1]);
            group.property("ADBE Scale").setValue(tr.scale);
            group.property("ADBE Rotate Z").setValue(tr.rotation);
            group.property("ADBE Opacity").setValue(tr.opacity);
            var keys = tr.keys || {};
            if (keys["position.x"]) {
                keyframes(x, keys["position.x"], one);
            }
            if (keys["position.y"]) {
                keyframes(y, keys["position.y"], one);
            }
            if (keys.scale) {
                keyframes(group.property("ADBE Scale"), keys.scale, both);
            }
            if (keys.rotation) {
                keyframes(group.property("ADBE Rotate Z"), keys.rotation, one);
            }
            if (keys.opacity) {
                keyframes(group.property("ADBE Opacity"), keys.opacity, one);
            }
            // MA4 -- a texture that boils: a jump drawn by an expression.
            var expressions = tr.expressions || {};
            if (expressions["position.x"]) {
                x.expression = expressions["position.x"];
            }
            if (expressions["position.y"]) {
                y.expression = expressions["position.y"];
            }
        }

        // Vector contents of a shape layer: one group, looked up again after
        // each addition (After invalidates the references).
        function contentsOf(layer) {
            return layer.property("ADBE Root Vectors Group")
                .property(1).property("ADBE Vectors Group");
        }

        function rectLayer(comp, spec) {
            var layer = comp.layers.addShape();
            layer.property("ADBE Root Vectors Group")
                .addProperty("ADBE Vector Group");
            contentsOf(layer).addProperty("ADBE Vector Shape - Rect");
            var rect = contentsOf(layer).property(1);
            rect.property("ADBE Vector Rect Size").setValue(spec.size);
            // MA11 -- a box the layout resizes: the rectangle is centred on
            // the layer, which the layout moves.
            rect.property("ADBE Vector Rect Position").setValue(spec.position ||
                (spec.centered ? [0, 0] : [spec.size[0] / 2, spec.size[1] / 2]));
            rect.property("ADBE Vector Rect Roundness").setValue(spec.roundness);
            contentsOf(layer).addProperty("ADBE Vector Graphic - Fill");
            var fill = contentsOf(layer).property(2);
            fill.property("ADBE Vector Fill Color").setValue(color(spec.color));
            fill.property("ADBE Vector Fill Opacity").setValue(spec.alpha * 100);
            if (spec.size_keys) {
                keyframes(contentsOf(layer).property(1)
                    .property("ADBE Vector Rect Size"), spec.size_keys, both);
            }
            if (spec.position_keys) {
                keyframes(contentsOf(layer).property(1)
                    .property("ADBE Vector Rect Position"),
                    spec.position_keys, both);
            }
            if (spec.color_keys) {
                keyframes(contentsOf(layer).property(2)
                    .property("ADBE Vector Fill Color"), spec.color_keys, colour);
            }
            if (spec.alpha_keys) {
                keyframes(contentsOf(layer).property(2)
                    .property("ADBE Vector Fill Opacity"), spec.alpha_keys, one);
            }
            return layer;
        }

        function shapeLayer(comp, spec) {
            var layer = comp.layers.addShape();
            layer.property("ADBE Root Vectors Group")
                .addProperty("ADBE Vector Group");
            var p;
            for (p = 0; p < spec.paths.length; p += 1) {
                contentsOf(layer).addProperty("ADBE Vector Shape - Group");
                var shape = new Shape();
                shape.vertices = spec.paths[p].vertices;
                shape.inTangents = spec.paths[p]["in"];
                shape.outTangents = spec.paths[p].out;
                shape.closed = spec.paths[p].closed;
                contentsOf(layer).property(p + 1)
                    .property("ADBE Vector Shape").setValue(shape);
            }
            var slot = spec.paths.length;
            if (spec.trim) {
                contentsOf(layer).addProperty("ADBE Vector Filter - Trim");
                slot += 1;
                var trim = contentsOf(layer).property(slot);
                trim.property("ADBE Vector Trim Start").setValue(spec.trim.start);
                trim.property("ADBE Vector Trim End").setValue(spec.trim.end);
                if (spec.trim.start_keys) {
                    keyframes(contentsOf(layer).property(slot)
                        .property("ADBE Vector Trim Start"),
                        spec.trim.start_keys, one);
                }
                if (spec.trim.end_keys) {
                    keyframes(contentsOf(layer).property(slot)
                        .property("ADBE Vector Trim End"),
                        spec.trim.end_keys, one);
                }
            }
            if (spec.stroke) {
                contentsOf(layer).addProperty("ADBE Vector Graphic - Stroke");
                slot += 1;
                var stroke = contentsOf(layer).property(slot);
                stroke.property("ADBE Vector Stroke Color")
                    .setValue(color(spec.stroke.color));
                stroke.property("ADBE Vector Stroke Opacity")
                    .setValue(spec.stroke.alpha * 100);
                stroke.property("ADBE Vector Stroke Width")
                    .setValue(spec.stroke.width);
                stroke.property("ADBE Vector Stroke Line Cap").setValue(
                    { butt: 1, round: 2, square: 3 }[spec.stroke.cap] || 1);
                stroke.property("ADBE Vector Stroke Line Join").setValue(
                    { miter: 1, round: 2, bevel: 3 }[spec.stroke.join] || 1);
                if (spec.stroke.color_keys) {
                    keyframes(contentsOf(layer).property(slot)
                        .property("ADBE Vector Stroke Color"),
                        spec.stroke.color_keys, colour);
                }
                if (spec.stroke.alpha_keys) {
                    keyframes(contentsOf(layer).property(slot)
                        .property("ADBE Vector Stroke Opacity"),
                        spec.stroke.alpha_keys, one);
                }
            }
            if (spec.fill) {
                contentsOf(layer).addProperty("ADBE Vector Graphic - Fill");
                slot += 1;
                var fill = contentsOf(layer).property(slot);
                fill.property("ADBE Vector Fill Color")
                    .setValue(color(spec.fill.color));
                fill.property("ADBE Vector Fill Opacity")
                    .setValue(spec.fill.alpha * 100);
                fill.property("ADBE Vector Fill Rule")
                    .setValue(spec.fill.evenodd ? 2 : 1);
                if (spec.fill.color_keys) {
                    keyframes(contentsOf(layer).property(slot)
                        .property("ADBE Vector Fill Color"),
                        spec.fill.color_keys, colour);
                }
                if (spec.fill.alpha_keys) {
                    keyframes(contentsOf(layer).property(slot)
                        .property("ADBE Vector Fill Opacity"),
                        spec.fill.alpha_keys, one);
                }
            }
            return layer;
        }

        function textLayer(comp, spec) {
            var layer = comp.layers.addText(spec.content);
            var source = layer.property("ADBE Text Properties")
                .property("ADBE Text Document");
            var doc = source.value;
            doc.resetCharStyle();
            doc.font = spec.variable ? variableFont(spec.variable, spec.font)
                : spec.font;
            doc.fontSize = spec.size;
            doc.applyFill = true;
            doc.fillColor = spec.color;
            doc.applyStroke = false;
            doc.tracking = spec.tracking;
            // The engine sets Core Text's kern attribute, which turns the
            // font's pair kerning off: After's metrics kerning would set
            // the letters closer.
            try {
                doc.autoKernType = AutoKernType.NO_AUTO_KERN;
            } catch (noKerningSetting) {
            }
            doc.justification = {
                center: ParagraphJustification.CENTER_JUSTIFY,
                right: ParagraphJustification.RIGHT_JUSTIFY
            }[spec.justify] || ParagraphJustification.LEFT_JUSTIFY;
            if (spec.leading) {
                doc.autoLeading = false;
                doc.leading = spec.leading;
            }
            source.setValue(doc);
            if (spec.expression) {
                layer.property("ADBE Text Properties")
                    .property("ADBE Text Document").expression = spec.expression;
            }
            if (spec.tracking_keys) {
                tracking(layer, spec.tracking_keys);
            }
            if (spec.units) {
                units(layer, spec.units);
            }
            if (spec.color_keys || spec.alpha_keys) {
                // MA11 -- the colour it shows, keyed: an animator over every
                // character.
                animatorsOf(layer).addProperty("ADBE Text Animator");
                lastAnimator(layer).name = "couleur";
                lastAnimator(layer).property("ADBE Text Selectors")
                    .addProperty("ADBE Text Selector");
                if (spec.color_keys) {
                    lastAnimator(layer).property("ADBE Text Animator Properties")
                        .addProperty("ADBE Text Fill Color");
                    keyframes(lastAnimator(layer)
                        .property("ADBE Text Animator Properties")
                        .property("ADBE Text Fill Color"), spec.color_keys,
                        colour);
                }
                if (spec.alpha_keys) {
                    lastAnimator(layer).property("ADBE Text Animator Properties")
                        .addProperty("ADBE Text Fill Opacity");
                    keyframes(lastAnimator(layer)
                        .property("ADBE Text Animator Properties")
                        .property("ADBE Text Fill Opacity"), spec.alpha_keys,
                        one);
                }
            }
            return layer;
        }

        function animatorsOf(layer) {
            return layer.property("ADBE Text Properties")
                .property("ADBE Text Animators");
        }

        function lastAnimator(layer) {
            return animatorsOf(layer).property(animatorsOf(layer).numProperties);
        }

        // letterSpacing animated: one animator over every character, its
        // tracking keyed.
        function tracking(layer, keys) {
            animatorsOf(layer).addProperty("ADBE Text Animator");
            lastAnimator(layer).name = "approche";
            lastAnimator(layer).property("ADBE Text Selectors")
                .addProperty("ADBE Text Selector");
            lastAnimator(layer).property("ADBE Text Animator Properties")
                .addProperty("ADBE Text Tracking Amount");
            // CSS letter-spacing follows each letter; After's default puts
            // the amount before and after, twice as wide. The type is hidden
            // until added.
            lastAnimator(layer).property("ADBE Text Animator Properties")
                .addProperty("ADBE Text Track Type");
            lastAnimator(layer).property("ADBE Text Animator Properties")
                .property("ADBE Text Track Type").setValue(3);
            keyframes(lastAnimator(layer).property("ADBE Text Animator Properties")
                .property("ADBE Text Tracking Amount"), keys, one);
        }

        function slider(layer, name, value) {
            layer.property("ADBE Effect Parade").addProperty("ADBE Slider Control");
            var parade = layer.property("ADBE Effect Parade");
            var fx = parade.property(parade.numProperties);
            fx.name = name;
            fx.property(1).setValue(value);
            return function () {
                return layer.property("ADBE Effect Parade").property(name)
                    .property(1);
            };
        }

        // Units in cascade: a delay slider, and per property a slider with
        // the engine's keyframes that an expression selector reads at each
        // unit's delay.
        function units(layer, spec) {
            var basedOn = { characters_excluding_spaces: 2, words: 3, lines: 4 }[
                spec.based_on] || 2;
            var more = layer.property("ADBE Text Properties")
                .property("ADBE Text More Options");
            // Anchor grouping: characters 1, words 2, lines 3.
            more.property("ADBE Text Anchor Point Option")
                .setValue({ 3: 2, 4: 3 }[basedOn] || 1);
            var delay;
            if (spec.order === "list") {
                delay = "[" + spec.delays.join(",") + "][textIndex - 1]";
            } else {
                slider(layer, "décalage (images)", spec.each);
                delay = (spec.order === "reverse"
                    ? "(textTotal - textIndex)"
                    : "(textIndex - 1)") +
                    " * effect(\"décalage (images)\")(1)";
            }
            var matchNames = {
                opacity: "ADBE Text Opacity",
                position_x: "ADBE Text Position 3D",
                position_y: "ADBE Text Position 3D",
                rotation: "ADBE Text Rotation",
                scale: "ADBE Text Scale 3D",
                scale_x: "ADBE Text Scale 3D",
                scale_y: "ADBE Text Scale 3D"
            };
            var i;
            var anchorNeeded = false;
            for (i = 0; i < spec.animators.length; i += 1) {
                var a = spec.animators[i];
                if (/scale|rotation/.test(a.property)) {
                    anchorNeeded = true;
                }
                var read = slider(layer, a.slider, a.keys.length ? a.keys[0].v[0] : 0);
                keyframes(read(), a.keys, one);
                var R = a.reference;
                var value = {
                    opacity: R,
                    position_x: [R, 0, 0],
                    position_y: [0, R, 0],
                    rotation: R,
                    scale: [R, R, 100],
                    scale_x: [R, 100, 100],
                    scale_y: [100, R, 100]
                }[a.property];
                animatorsOf(layer).addProperty("ADBE Text Animator");
                lastAnimator(layer).name = a.slider;
                lastAnimator(layer).property("ADBE Text Animator Properties")
                    .addProperty(matchNames[a.property]);
                lastAnimator(layer).property("ADBE Text Animator Properties")
                    .property(matchNames[a.property]).setValue(value);
                lastAnimator(layer).property("ADBE Text Selectors")
                    .addProperty("ADBE Text Expressible Selector");
                var selector = lastAnimator(layer).property("ADBE Text Selectors")
                    .property(1);
                selector.property("ADBE Text Range Type2").setValue(basedOn);
                lastAnimator(layer).property("ADBE Text Selectors").property(1)
                    .property("ADBE Text Expressible Amount").expression =
                    "var v = effect(\"" + a.slider + "\")(1).valueAtTime(time - " +
                    delay + " * thisComp.frameDuration);\n" +
                    "var k = (v - " + a.neutral + ") / (" + R + " - " + a.neutral +
                    ") * 100;\n[k, k, k]";
            }
            if (anchorNeeded && spec.anchor_y) {
                // Units turn about the middle of their line.
                animatorsOf(layer).addProperty("ADBE Text Animator");
                lastAnimator(layer).name = "centre des unités";
                lastAnimator(layer).property("ADBE Text Animator Properties")
                    .addProperty("ADBE Text Anchor Point 3D");
                lastAnimator(layer).property("ADBE Text Animator Properties")
                    .property("ADBE Text Anchor Point 3D")
                    .setValue([0, spec.anchor_y, 0]);
                // Moving a character's anchor moves the character the other
                // way: the same offset in position puts it back.
                lastAnimator(layer).property("ADBE Text Animator Properties")
                    .addProperty("ADBE Text Position 3D");
                lastAnimator(layer).property("ADBE Text Animator Properties")
                    .property("ADBE Text Position 3D")
                    .setValue([0, spec.anchor_y, 0]);
                lastAnimator(layer).property("ADBE Text Selectors")
                    .addProperty("ADBE Text Selector");
            }
        }

        function effects(layer, list) {
            for (var e = 0; e < list.length; e += 1) {
                if (list[e].kind !== "wave_warp") {
                    continue;
                }
                var parade = layer.property("ADBE Effect Parade");
                parade.addProperty("ADBE Wave Warp");
                var fx = layer.property("ADBE Effect Parade")
                    .property(layer.property("ADBE Effect Parade").numProperties);
                fx.name = "slice (approché)";
                // Square wave, rows displaced sideways, still.
                fx.property(1).setValue(2);
                fx.property(2).setValue(list[e].height);
                fx.property(3).setValue(list[e].width);
                fx.property(4).setValue(0);
                fx.property(5).setValue(0);
                // A new band pattern every frame, as slice()'s phase gives.
                fx.property(7).expression =
                    "posterizeTime(1 / thisComp.frameDuration);\n" +
                    "seedRandom(index, false);\nrandom(360);";
                if (list[e].height_keys) {
                    keyframes(fx.property(2), list[e].height_keys, one);
                }
            }
        }

        // MOTION-WEB-2026-10 MW7 -- a text in a variable font: After names
        // an instance at any point of its axes (FontObject, After 24 and
        // later): postScriptNameForDesignVector gives a named instance's
        // name when the weight is one ("Nunito-ExtraBold"), and makes one
        // up otherwise ("Nunito_650.000wght_0ital"), which a text layer then
        // takes, drawn at that weight (measured in After 26). The other axes
        // stay at their default. A family After does not list keeps the
        // name Core Text gave.
        function variableFont(v, fallback) {
            try {
                var list = app.fonts.fontsWithDefaultDesignAxes;
                for (var f = 0; f < list.length; f += 1) {
                    var fo = list[f];
                    var italic = /italic|oblique/i.test(fo.styleName);
                    if (fo.familyName !== v.family || italic !== v.italic) {
                        continue;
                    }
                    var axes = fo.designAxesData;
                    var vector = [];
                    var weighted = false;
                    for (var a = 0; a < axes.length; a += 1) {
                        if (axes[a].tag === "wght") {
                            vector.push(Math.max(axes[a].min,
                                Math.min(axes[a].max, v.wght)));
                            weighted = true;
                        } else {
                            vector.push(axes[a]["default"]);
                        }
                    }
                    if (weighted) {
                        return fo.postScriptNameForDesignVector(vector);
                    }
                }
            } catch (error) {
            }
            return fallback;
        }

        // MOTION-WEB-2026-10 MW5 -- a skew: the Transform effect, last in
        // the stack (a filter comes before the transform, as in CSS),
        // turning about the layer's anchor, which is the node's
        // transformOrigin. A shape or a text layer is rasterized after its
        // transform, so its effect works in the composition's space: the
        // pivot follows the anchor there, and the axis the layer's turn, by
        // expressions (the axis turns the other way from a rotation:
        // measured in After 26 on a box turned and skewed at once). A footage or precomposition layer takes the effect in
        // its own space, the anchor fixed (MotionAfterExport.cc, Skew).
        function skew(layer, spec, kind) {
            var vector = kind === "rect" || kind === "shape" || kind === "text";
            var anchor = layer.property("ADBE Transform Group")
                .property("ADBE Anchor Point").value;
            layer.property("ADBE Effect Parade").addProperty("ADBE Geometry2");
            var parade = layer.property("ADBE Effect Parade");
            var at = parade.numProperties;
            var fx = parade.property(at);
            fx.name = "inclinaison";
            var pivot = "// inclinaison (MW5) : autour de l'ancre du calque.\n" +
                "var p = toComp(transform.anchorPoint);\n[p[0], p[1]]";
            var names = ["ADBE Geometry2-0001", "ADBE Geometry2-0002"];
            for (var n = 0; n < names.length; n += 1) {
                fx = layer.property("ADBE Effect Parade").property(at);
                if (vector) {
                    fx.property(names[n]).expression = pivot;
                } else {
                    fx.property(names[n]).setValue([anchor[0], anchor[1]]);
                }
            }
            fx = layer.property("ADBE Effect Parade").property(at);
            fx.property("ADBE Geometry2-0006").setValue(spec.axis);
            if (vector) {
                fx = layer.property("ADBE Effect Parade").property(at);
                fx.property("ADBE Geometry2-0006").expression =
                    "// inclinaison (MW5) : l'axe tourne avec le calque.\n" +
                    "var r = 0, l = thisLayer;\n" +
                    "while (true) { r += l.transform.rotation; " +
                    "if (!l.hasParent) break; l = l.parent; }\nvalue - r";
            }
            var angle = spec.angle;
            var keys = spec.keys;
            if (!vector && spec.raster_angle !== undefined) {
                angle = spec.raster_angle;
                keys = spec.raster_keys;
            }
            fx = layer.property("ADBE Effect Parade").property(at);
            fx.property("ADBE Geometry2-0005").setValue(angle);
            if (keys) {
                fx = layer.property("ADBE Effect Parade").property(at);
                keyframes(fx.property("ADBE Geometry2-0005"), keys, one);
            }
        }

        // MA3 -- a row of words stays a row: each text moves by the change of
        // width of the texts the engine said move it (`flow`, one
        // coefficient each), After measuring each one -- where its letters
        // end, sourceRectAtTime at its in point -- against what it measured
        // here, right after building it. Untouched, every change is 0 (a
        // thousandth of a pixel is ignored) and the places are the engine's;
        // a word replaced pushes the others. A text that scales or turns
        // also moves its anchor by `anchor` of its own change, the point of
        // its box it turns about. A text named like another layer of the
        // composition, or that After cannot measure (After 26 may throw
        // "divide by zero" there), leaves its row in its fixed places.
        function quotedName(text) {
            return '"' + String(text).replace(/\\/g, "\\\\")
                .replace(/"/g, '\\"') + '"';
        }

        // `k * ecart("name", width)` for one text, or null when the text
        // cannot be named or measured.
        function term(comp, source, k) {
            var named = 0;
            var end = NaN;
            if (!source) {
                return null;
            }
            for (var n = 1; n <= comp.numLayers; n += 1) {
                if (comp.layer(n).name === source.name) {
                    named += 1;
                }
            }
            try {
                var rect = source.sourceRectAtTime(source.inPoint, false);
                end = rect.left + rect.width;
            } catch (unmeasured) {
                end = NaN;
            }
            if (named !== 1 || isNaN(end)) {
                return null;
            }
            return String(k) + " * ecart(" + quotedName(source.name) + ", " +
                String(end) + ")";
        }

        var FLOW_FUNCTIONS =
            "// CUTMACHINE (MA3) : la rangée suit la largeur de ses mots.\n" +
            "// Un mot changé pousse les autres comme dans le moteur ; " +
            "sans\n// changement, chaque écart vaut 0.\n" +
            "function fin(nom) {\n" +
            "    var l = thisComp.layer(nom);\n" +
            "    var r = l.sourceRectAtTime(l.inPoint, false);\n" +
            "    return r.left + r.width;\n}\n" +
            "function ecart(nom, mesure) {\n" +
            "    var d = fin(nom) - mesure;\n" +
            "    return Math.abs(d) < 0.001 ? 0 : d;\n}\n";

        function flows(comp, spec, layers) {
            for (var f = 0; f < spec.layers.length; f += 1) {
                var F = spec.layers[f];
                if (!F.flow || !layers[F.id]) {
                    continue;
                }
                var sum = [];
                var whole = true;
                for (var k = 0; k < F.flow.terms.length && whole; k += 1) {
                    var said = term(comp, layers[F.flow.terms[k].layer],
                        F.flow.terms[k].k);
                    whole = said !== null;
                    sum.push(said);
                }
                var own = null;
                if (whole && F.flow.anchor !== undefined) {
                    own = term(comp, layers[F.flow.self], F.flow.anchor);
                    whole = own !== null;
                }
                if (!whole) {
                    progress.rows_fixed += 1;
                    continue;
                }
                if (own !== null) {
                    sum.push(own);
                    layers[F.id].property("ADBE Transform Group")
                        .property("ADBE Anchor Point").expression =
                        FLOW_FUNCTIONS + "var a = value;\na[0] = a[0] + " +
                        own + ";\na";
                }
                layers[F.id].property("ADBE Transform Group")
                    .property("ADBE Position_0").expression =
                    FLOW_FUNCTIONS + "value + " + sum.join(" + ");
                progress.rows += 1;
            }
        }

        var built = {};
        for (i = 0; i < d.comps.length; i += 1) {
            var spec = d.comps[i];
            var comp = comps[spec.id];
            var layers = {};
            for (var l = 0; l < spec.layers.length; l += 1) {
                var L = spec.layers[l];
                var layer;
                if (L.kind === "rect") {
                    layer = rectLayer(comp, L.rect);
                } else if (L.kind === "shape") {
                    layer = shapeLayer(comp, L.shape);
                } else if (L.kind === "text") {
                    layer = textLayer(comp, L.text);
                } else if (L.kind === "image" || L.kind === "video") {
                    layer = comp.layers.add(footage[L.footage]);
                    // MA11 -- a video at another speed: stretched before
                    // it is placed, since the stretch moves its out point.
                    if (L.stretch) {
                        layer.stretch = L.stretch;
                    }
                } else if (L.kind === "null") {
                    layer = comp.layers.addNull();
                } else if (L.kind === "precomp") {
                    layer = comp.layers.add(comps[L.comp]);
                } else if (L.kind === "audio") {
                    layer = comp.layers.add(footage[L.footage]);
                    layer.name = L.name;
                    layer.startTime = L.start_s;
                    layer.inPoint = L.in_s;
                    layer.outPoint = L.out_s;
                    var levels = layer.property("ADBE Audio Group")
                        .property("ADBE Audio Levels");
                    levels.setValue([L.gain_db, L.gain_db]);
                    for (var a = 0; a < L.levels.length; a += 1) {
                        levels.setValueAtTime(
                            L.levels[a][0], [L.levels[a][1], L.levels[a][1]]);
                    }
                    if (L.volume_keys) {
                        volume(layer, L);
                    }
                    layers[L.id] = layer;
                    progress.layers += 1;
                    continue;
                } else {
                    continue;
                }
                layer.name = L.name;
                if (L.parent && layers[L.parent]) {
                    layer.setParentWithJump(layers[L.parent]);
                }
                if (L.start !== undefined) {
                    layer.startTime = t(L.start);
                    layer.inPoint = t(L["in"]);
                    layer.outPoint = t(L.out);
                }
                if (L.blend) {
                    layer.blendingMode = BlendingMode[L.blend];
                }
                transform(layer, L.transform);
                if (L.effects) {
                    effects(layer, L.effects);
                }
                if (L.transform && L.transform.skew) {
                    skew(layer, L.transform.skew, L.kind);
                }
                if (L.masks) {
                    masks(layer, L.masks);
                }
                if (L.kind === "video") {
                    layer.audioEnabled = L.audio === true;
                    if (L.audio) {
                        volume(layer, L);
                    }
                }
                // MA12 -- every layer that shows something blurs as it
                // moves, as in the engine.
                if (d.motion_blur && L.kind !== "null") {
                    layer.motionBlur = true;
                }
                layers[L.id] = layer;
                progress.layers += 1;
            }
            flows(comp, spec, layers);
            built[spec.id] = true;
        }

    } finally {
        app.endUndoGroup();
    }
    comps[d.main].openInViewer();
    return comps[d.main];
}

// A request the bridge left in After's globals is not this run's: only a
// request that is there takes the bridge's way (MA11).
if (typeof CUTMACHINE_AFTER_REQUEST !== "undefined" &&
        CUTMACHINE_AFTER_REQUEST) {
    var CUTMACHINE_AFTER_HANDLER = function (request) {
        var project = app.project;
        var target = new File(request.arguments.project_path);
        var started = new Date().getTime();
        var progress = {layers: 0, rows: 0, rows_fixed: 0};
        var d;
        var main;
        if (!project) {
            project = app.newProject();
        }
        if (!project) {
            return CUTMACHINE_AFTER.failure(
                "NoProject", "After Effects n'a créé aucun projet actif."
            );
        }
        if (CUTMACHINE_AFTER.projectPath(project) !== "" ||
                project.numItems !== 0) {
            return CUTMACHINE_AFTER.failure(
                "ProjectAlreadyOpen",
                "Un autre projet After Effects est ouvert : ferme-le " +
                    "(Fichier > Fermer le projet) avant de construire."
            );
        }
        if (target.exists) {
            return CUTMACHINE_AFTER.failure(
                "ProjectExists", "Le projet After Effects cible existe déjà."
            );
        }
        d = eval("(" + CUTMACHINE_MOTION_READ(
            request.arguments.description_path) + ")");
        if (d.schema !== "after-motion.v1") {
            return CUTMACHINE_AFTER.failure(
                "InvalidDescription",
                "La description n'est pas au schéma after-motion.v1."
            );
        }
        app.beginSuppressDialogs();
        try {
            main = CUTMACHINE_MOTION_BUILD(d, "", progress);
            project.save(target);
        } catch (error) {
            // The project was empty: what a failed build left in it is
            // only ours, and closing it asks nothing.
            app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES);
            app.endSuppressDialogs(false);
            return CUTMACHINE_AFTER.failure(
                "AfterScriptError",
                String(error) + " (ligne " + (error.line || 0) + ", " +
                    progress.layers + " calques faits)"
            );
        }
        app.endSuppressDialogs(false);
        if (!target.exists) {
            return CUTMACHINE_AFTER.failure(
                "SaveFailed", "After Effects n'a pas créé le fichier demandé."
            );
        }
        return CUTMACHINE_AFTER.success(app.project, {
            main_comp_id: String(main.id),
            comps: d.comps.length,
            layers: progress.layers,
            rows: progress.rows,
            rows_fixed: progress.rows_fixed,
            seconds: (new Date().getTime() - started) / 1000
        });
    };
} else {
    (function () {
        var descriptionPath =
            typeof CUTMACHINE_MOTION_DESCRIPTION !== "undefined"
                ? CUTMACHINE_MOTION_DESCRIPTION
                : null;
        var responsePath =
            typeof CUTMACHINE_MOTION_RESPONSE !== "undefined"
                ? CUTMACHINE_MOTION_RESPONSE
                : null;

        function quote(text) {
            return '"' + String(text).replace(/\\/g, "\\\\")
                .replace(/"/g, '\\"').replace(/\n/g, "\\n")
                .replace(/\r/g, "\\r") + '"';
        }

        function answer(ok, fields) {
            if (!responsePath) {
                return;
            }
            var parts = ['"ok":' + (ok ? "true" : "false")];
            for (var key in fields) {
                if (fields.hasOwnProperty(key)) {
                    var value = fields[key];
                    parts.push(quote(key) + ":" + (typeof value === "number"
                        ? String(value)
                        : quote(value)));
                }
            }
            var file = new File(responsePath);
            file.encoding = "UTF-8";
            file.open("w");
            file.write("{" + parts.join(",") + "}");
            file.close();
        }

        if (!descriptionPath) {
            var picked = File.openDialog("Description after-motion.v1 (.json)");
            if (!picked) {
                return;
            }
            descriptionPath = picked.fsName;
        }

        var started = new Date().getTime();
        var progress = {layers: 0, rows: 0, rows_fixed: 0};
        try {
            var d = eval("(" + CUTMACHINE_MOTION_READ(descriptionPath) + ")");
            // Unsaved work stays where it is: the composition is then built
            // into the open project, in a folder named with the hour, and
            // nothing is saved.
            var into = app.project && app.project.dirty;
            var now = new Date();
            app.beginSuppressDialogs();
            if (!into) {
                app.newProject();
            }
            CUTMACHINE_MOTION_BUILD(d, into
                ? ("0" + now.getHours()).slice(-2) + ":" +
                  ("0" + now.getMinutes()).slice(-2)
                : "", progress);
            if (d.aep_path && !into) {
                app.project.save(new File(d.aep_path));
            }
            app.endSuppressDialogs(false);
            answer(true, {
                aep_path: into ? "" : (d.aep_path || ""),
                mode: into
                    ? "dans le projet ouvert (non enregistré)"
                    : "nouveau projet",
                comps: d.comps.length,
                layers: progress.layers,
                rows: progress.rows,
                rows_fixed: progress.rows_fixed,
                seconds: (new Date().getTime() - started) / 1000
            });
        } catch (error) {
            try {
                app.endSuppressDialogs(false);
            } catch (ignored) {
            }
            answer(false, {
                error: String(error),
                line: error.line || 0,
                layers: progress.layers
            });
            if (!responsePath) {
                alert("CUTMACHINE : " + error);
            }
        }
    })();
}
