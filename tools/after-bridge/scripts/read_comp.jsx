/*
 * MB7 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. Reads one composition
 * as the importer needs it (MotionAfterImport.h): every layer with its
 * timing, parent, switches, transform, masks, text, shape contents and
 * effects, each property with its value, its keys and their eases. Nothing is
 * judged here: what the format cannot say is decided and named by the engine.
 * Read only: the project is never changed or saved.
 *
 * After 26 can raise "divide by zero" reading a property or a font object
 * (MOTION-WEB-2026-10, 14 November 2026): every field is read on its own,
 * and a field that throws is left out, never guessed.
 */
var CUTMACHINE_AFTER_HANDLER = function (request) {
    var project = app.project;
    var comp = CUTMACHINE_AFTER.compById(request.arguments.comp_id);
    // Expressions and curved trajectories are sampled once per frame over
    // the layer's span; a composition longer than this is refused rather
    // than read for minutes.
    var kMaxSamples = 3000;

    function read(getter) {
        try {
            return getter();
        } catch (ignored) {
            return undefined;
        }
    }

    function enumName(table, value) {
        var key;
        for (key in table) {
            if (table.hasOwnProperty(key) && table[key] === value) {
                return String(key);
            }
        }
        return String(value);
    }

    function plain(value) {
        var result;
        var index;
        if (value === null || value === undefined) {
            return null;
        }
        if (typeof value === "number" || typeof value === "string" ||
                typeof value === "boolean") {
            return value;
        }
        if (value instanceof Array) {
            result = [];
            for (index = 0; index < value.length; index += 1) {
                result.push(plain(value[index]));
            }
            return result;
        }
        if (value instanceof Shape) {
            return {
                vertices: plain(value.vertices),
                in_tangents: plain(value.inTangents),
                out_tangents: plain(value.outTangents),
                closed: Boolean(value.closed)
            };
        }
        return String(value);
    }

    function interpolationName(type) {
        if (type === KeyframeInterpolationType.HOLD) {
            return "hold";
        }
        if (type === KeyframeInterpolationType.LINEAR) {
            return "linear";
        }
        return "bezier";
    }

    function eases(list) {
        var result = [];
        var index;
        for (index = 0; index < list.length; index += 1) {
            result.push([list[index].speed, list[index].influence]);
        }
        return result;
    }

    function keyframes(property) {
        var result = [];
        var count = read(function () { return property.numKeys; }) || 0;
        var spatial = Boolean(read(function () { return property.isSpatial; }));
        var index;
        var key;
        for (index = 1; index <= count; index += 1) {
            key = {
                t: read(function () { return property.keyTime(index); }),
                v: plain(read(function () { return property.keyValue(index); })),
                in_type: interpolationName(read(function () {
                    return property.keyInInterpolationType(index);
                })),
                out_type: interpolationName(read(function () {
                    return property.keyOutInterpolationType(index);
                }))
            };
            key.in_ease = read(function () {
                return eases(property.keyInTemporalEase(index));
            });
            key.out_ease = read(function () {
                return eases(property.keyOutTemporalEase(index));
            });
            if (spatial) {
                key.in_tangent = plain(read(function () {
                    return property.keyInSpatialTangent(index);
                }));
                key.out_tangent = plain(read(function () {
                    return property.keyOutSpatialTangent(index);
                }));
            }
            result.push(key);
        }
        return result;
    }

    function curved(keys) {
        var index;
        var axis;
        var tangent;
        for (index = 0; index < keys.length; index += 1) {
            tangent = (keys[index].in_tangent || []).concat(
                keys[index].out_tangent || []
            );
            for (axis = 0; axis < tangent.length; axis += 1) {
                if (Math.abs(tangent[axis]) > 1e-6) {
                    return true;
                }
            }
        }
        return false;
    }

    // The value After shows at each frame of [first, last): what an
    // expression or a curved path gives, read instead of re-implemented.
    function samples(property, first, last) {
        var result = [];
        var frame;
        for (frame = first; frame < last; frame += 1) {
            result.push(plain(read(function () {
                return property.valueAtTime(frame * comp.frameDuration, false);
            })));
        }
        return result;
    }

    function propertyRecord(property, span) {
        var record = {name: String(property.name)};
        var keys;
        var expression = "";
        if (read(function () { return property.expressionEnabled; })) {
            expression = String(read(function () {
                return property.expression;
            }) || "");
        }
        record.value = plain(read(function () {
            return property.valueAtTime(span.first * comp.frameDuration,
                                        false);
        }));
        keys = keyframes(property);
        if (keys.length > 0) {
            record.keys = keys;
        }
        if (expression !== "") {
            record.expression = expression;
            record.expression_error = String(read(function () {
                return property.expressionError;
            }) || "");
        }
        if (expression !== "" || curved(keys)) {
            record.samples_from = span.first;
            record.samples = samples(property, span.first, span.last);
        }
        return record;
    }

    // A property group as a tree: its properties by match name, its groups
    // in order. Depth-bounded: an import reads transforms, shapes, masks
    // and effects, never the material options of a 3D layer.
    function groupRecord(group, span, depth) {
        var record = {
            match_name: String(group.matchName),
            name: String(group.name),
            properties: {},
            groups: []
        };
        var count = read(function () { return group.numProperties; }) || 0;
        var index;
        var child;
        var type;
        var enabled = read(function () { return group.enabled; });
        if (enabled !== undefined) {
            record.enabled = Boolean(enabled);
        }
        if (group.matchName === "ADBE Mask Atom") {
            record.mask_mode = enumName(MaskMode, read(function () {
                return group.maskMode;
            }));
            record.inverted = Boolean(read(function () {
                return group.inverted;
            }));
        }
        for (index = 1; index <= count; index += 1) {
            child = read(function () { return group.property(index); });
            if (!child) {
                continue;
            }
            type = read(function () { return child.propertyType; });
            if (type === PropertyType.PROPERTY) {
                if (read(function () {
                    return child.propertyValueType;
                }) === PropertyValueType.NO_VALUE) {
                    continue;
                }
                record.properties[String(child.matchName)] =
                    propertyRecord(child, span);
            } else if (depth > 0) {
                record.groups.push(groupRecord(child, span, depth - 1));
            }
        }
        return record;
    }

    function colour(value) {
        return value === undefined ? null : plain(value);
    }

    function textRecord(layer, span) {
        var document = layer.property("ADBE Text Properties")
            .property("ADBE Text Document");
        var value = read(function () {
            return document.valueAtTime(span.first * comp.frameDuration, false);
        });
        var animators = read(function () {
            return layer.property("ADBE Text Properties")
                .property("ADBE Text Animators");
        });
        var record = {
            keyed: (read(function () { return document.numKeys; }) || 0) > 0,
            expression: Boolean(read(function () {
                return document.expressionEnabled;
            })),
            animators: animators ? (read(function () {
                return animators.numProperties;
            }) || 0) : 0
        };
        var path = read(function () {
            return layer.property("ADBE Text Properties")
                .property("ADBE Text Path Options").property("ADBE Text Path");
        });
        record.on_path = Boolean(path && read(function () {
            return path.value;
        }));
        if (!value) {
            return record;
        }
        record.text = String(read(function () { return value.text; }) || "");
        record.font = read(function () { return String(value.font); });
        record.font_family = read(function () {
            return String(value.fontFamily);
        });
        record.font_style = read(function () {
            return String(value.fontStyle);
        });
        record.font_size = read(function () { return value.fontSize; });
        record.apply_fill = read(function () { return value.applyFill; });
        record.fill_color = colour(read(function () {
            return value.fillColor;
        }));
        record.apply_stroke = read(function () { return value.applyStroke; });
        record.stroke_color = colour(read(function () {
            return value.strokeColor;
        }));
        record.stroke_width = read(function () { return value.strokeWidth; });
        record.stroke_over_fill = read(function () {
            return value.strokeOverFill;
        });
        record.justification = enumName(ParagraphJustification, read(
            function () { return value.justification; }
        ));
        record.tracking = read(function () { return value.tracking; });
        record.leading = read(function () { return value.leading; });
        record.auto_leading = read(function () { return value.autoLeading; });
        record.all_caps = read(function () { return value.allCaps; });
        record.small_caps = read(function () { return value.smallCaps; });
        record.faux_bold = read(function () { return value.fauxBold; });
        record.faux_italic = read(function () { return value.fauxItalic; });
        record.baseline_shift = read(function () {
            return value.baselineShift;
        });
        record.horizontal_scale = read(function () {
            return value.horizontalScale;
        });
        record.vertical_scale = read(function () {
            return value.verticalScale;
        });
        // After 24 and later; absent before (metric kerning then).
        record.auto_kern = read(function () {
            return enumName(AutoKernType, value.autoKernType);
        });
        record.box_text = Boolean(read(function () { return value.boxText; }));
        if (record.box_text) {
            record.box_size = plain(read(function () {
                return value.boxTextSize;
            }));
            record.box_position = plain(read(function () {
                return value.boxTextPos;
            }));
        }
        return record;
    }

    function sourceRecord(layer) {
        var source = read(function () { return layer.source; });
        var main;
        if (!source) {
            return null;
        }
        if (source instanceof CompItem) {
            return {kind: "comp", id: String(source.id),
                    name: String(source.name)};
        }
        main = read(function () { return source.mainSource; });
        if (main instanceof SolidSource) {
            return {kind: "solid", name: String(source.name),
                    color: plain(main.color)};
        }
        if (main instanceof FileSource) {
            return {kind: "file", name: String(source.name),
                    path: read(function () { return main.file.fsName; }),
                    still: Boolean(read(function () {
                        return main.isStill;
                    }))};
        }
        return {kind: "other", name: String(source.name)};
    }

    function layerKind(layer) {
        if (layer instanceof CameraLayer) {
            return "camera";
        }
        if (layer instanceof LightLayer) {
            return "light";
        }
        if (layer instanceof TextLayer) {
            return "text";
        }
        if (layer instanceof ShapeLayer) {
            return "shape";
        }
        if (read(function () { return layer.nullLayer; })) {
            return "null";
        }
        if (read(function () { return layer.adjustmentLayer; })) {
            return "adjustment";
        }
        return "av";
    }

    function layerRecord(layer) {
        var kind = layerKind(layer);
        var inPoint = read(function () { return layer.inPoint; }) || 0;
        var outPoint = read(function () { return layer.outPoint; }) || 0;
        var span = {
            first: Math.max(0, Math.floor(inPoint / comp.frameDuration +
                                          1e-6)),
            last: Math.min(Math.round(comp.duration / comp.frameDuration),
                           Math.ceil(outPoint / comp.frameDuration - 1e-6))
        };
        var record;
        var parent = read(function () { return layer.parent; });
        var masks = read(function () {
            return layer.property("ADBE Mask Parade");
        });
        var effects = read(function () {
            return layer.property("ADBE Effect Parade");
        });
        var styles = read(function () {
            return layer.property("ADBE Layer Styles");
        });
        if (span.last < span.first) {
            span.last = span.first;
        }
        record = {
            id: String(layer.id),
            index: layer.index,
            name: String(layer.name),
            kind: kind,
            enabled: Boolean(read(function () { return layer.enabled; })),
            guide: Boolean(read(function () { return layer.guideLayer; })),
            solo: Boolean(read(function () { return layer.solo; })),
            three_d: Boolean(read(function () { return layer.threeDLayer; })),
            motion_blur: Boolean(read(function () {
                return layer.motionBlur;
            })),
            parent_id: parent ? String(parent.id) : "",
            start: read(function () { return layer.startTime; }),
            in_point: inPoint,
            out_point: outPoint,
            stretch: read(function () { return layer.stretch; }),
            blending_mode: enumName(BlendingMode, read(function () {
                return layer.blendingMode;
            })),
            track_matte: enumName(TrackMatteType, read(function () {
                return layer.trackMatteType;
            })),
            is_track_matte: Boolean(read(function () {
                return layer.isTrackMatte;
            })),
            // After 23 and later: the matte is any layer, not only the one
            // above (left empty before, the importer then falls back).
            track_matte_layer_id: String(read(function () {
                return layer.trackMatteLayer ? layer.trackMatteLayer.id : "";
            }) || ""),
            time_remap: Boolean(read(function () {
                return layer.timeRemapEnabled;
            })),
            width: read(function () { return layer.width; }),
            height: read(function () { return layer.height; }),
            source: sourceRecord(layer),
            transform: groupRecord(layer.property("ADBE Transform Group"),
                                   span, 0),
            masks: masks ? groupRecord(masks, span, 1) : null,
            effects: effects ? groupRecord(effects, span, 2) : null,
            styles: Boolean(styles && read(function () {
                return styles.canSetEnabled && styles.enabled;
            }))
        };
        if (kind === "text") {
            record.text = textRecord(layer, span);
        }
        if (kind === "shape") {
            record.contents = groupRecord(
                layer.property("ADBE Root Vectors Group"), span, 12
            );
        }
        return record;
    }

    var layers = [];
    var index;
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
    if (comp.duration / comp.frameDuration > kMaxSamples) {
        return CUTMACHINE_AFTER.failure(
            "CompTooLong",
            "La composition dépasse " + kMaxSamples + " images."
        );
    }
    for (index = 1; index <= comp.numLayers; index += 1) {
        layers.push(layerRecord(comp.layer(index)));
    }
    return CUTMACHINE_AFTER.success(project, {
        schema: "after-comp.v1",
        comp: {
            id: String(comp.id),
            name: String(comp.name),
            width: comp.width,
            height: comp.height,
            frame_rate: comp.frameRate,
            frame_duration: comp.frameDuration,
            duration: comp.duration,
            pixel_aspect: comp.pixelAspect,
            background: plain(comp.bgColor),
            motion_blur: Boolean(read(function () { return comp.motionBlur; })),
            shutter_angle: read(function () { return comp.shutterAngle; }),
            shutter_phase: read(function () { return comp.shutterPhase; }),
            layers: layers
        }
    });
};
