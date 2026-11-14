// Pont After -- ROADMAP.md. The bridge's JSX handlers run under
// JavaScriptCore (osascript -l JavaScript), against a stand-in for After's
// object model: what a handler decides -- refuse a write whose value moved
// since it was read, write otherwise, open no undo group for a refusal -- is
// checked without After. The stand-in holds only what these handlers touch,
// with After's shapes (1-based collections, a TextDocument copied on read);
// it says nothing of After's own behaviour, which runs by hand against After
// (tools/after-bridge/README.md).
//
// usage: osascript -l JavaScript after_jsx_tests.js <scripts directory>

ObjC.import("Foundation");

var failures = [];

function check(condition, label) {
    if (!condition) {
        failures.push(label);
    }
}

function read(path) {
    var text = $.NSString.stringWithContentsOfFileEncodingError(
        path, $.NSUTF8StringEncoding, null);
    if (text.isNil()) {
        throw new Error("cannot read " + path);
    }
    return text.js;
}

// --- After's object model, as far as the handlers reach -------------------

var PropertyType = {PROPERTY: 6212, INDEXED_GROUP: 6213, NAMED_GROUP: 6214};

function CompItem(id, frameDuration) {
    this.id = id;
    this.frameDuration = frameDuration;
    this.layers = [];
}
CompItem.prototype.layer = function (index) {
    return this.layers[index - 1];
};
Object.defineProperty(CompItem.prototype, "numLayers", {
    get: function () {
        return this.layers.length;
    }
});

function Group(matchName, children) {
    this.matchName = matchName;
    this.name = matchName;
    this.propertyType = PropertyType.NAMED_GROUP;
    this.children = children;
}
Group.prototype.property = function (key) {
    var index;
    if (typeof key === "number") {
        return this.children[key - 1];
    }
    for (index = 0; index < this.children.length; index += 1) {
        if (this.children[index].matchName === key) {
            return this.children[index];
        }
    }
    return null;
};
Object.defineProperty(Group.prototype, "numProperties", {
    get: function () {
        return this.children.length;
    }
});

// A value property: static, or keyed at times (seconds).
function Property(matchName, value) {
    this.matchName = matchName;
    this.name = matchName;
    this.propertyType = PropertyType.PROPERTY;
    this.expressionEnabled = false;
    this.static = value;
    this.keys = [];
    this.writes = 0;
}
Object.defineProperty(Property.prototype, "numKeys", {
    get: function () {
        return this.keys.length;
    }
});
Object.defineProperty(Property.prototype, "value", {
    get: function () {
        return this.static;
    }
});
Property.prototype.setValue = function (value) {
    this.static = value;
    this.writes += 1;
};
Property.prototype.nearestKeyIndex = function (time) {
    var best = 1;
    var index;
    for (index = 1; index < this.keys.length; index += 1) {
        if (Math.abs(this.keys[index].time - time) <
                Math.abs(this.keys[best - 1].time - time)) {
            best = index + 1;
        }
    }
    return best;
};
Property.prototype.keyTime = function (index) {
    return this.keys[index - 1].time;
};
Property.prototype.keyValue = function (index) {
    return this.keys[index - 1].value;
};
Property.prototype.valueAtTime = function (time) {
    var index;
    var held = this.keys.length ? this.keys[0].value : this.static;
    for (index = 0; index < this.keys.length; index += 1) {
        if (this.keys[index].time <= time) {
            held = this.keys[index].value;
        }
    }
    return held;
};
Property.prototype.setValueAtTime = function (time, value) {
    var index;
    this.writes += 1;
    for (index = 0; index < this.keys.length; index += 1) {
        if (Math.abs(this.keys[index].time - time) < 1e-7) {
            this.keys[index].value = value;
            return;
        }
    }
    this.keys.push({time: time, value: value});
};

// Source text: reading it gives a copy, as After's TextDocument does.
function SourceText(text) {
    this.matchName = "ADBE Text Document";
    this.name = "Source Text";
    this.propertyType = PropertyType.PROPERTY;
    this.text = text;
    this.writes = 0;
}
Object.defineProperty(SourceText.prototype, "value", {
    get: function () {
        return {text: this.text};
    }
});
SourceText.prototype.setValue = function (document) {
    this.text = document.text;
    this.writes += 1;
};

function Layer(id, groups) {
    this.id = id;
    this.enabled = true;
    this.locked = false;
    this.startTime = 0;
    this.inPoint = 0;
    this.outPoint = 2;
    this.root = new Group("ADBE Root", groups);
}
Layer.prototype.property = function (key) {
    return this.root.property(key);
};
Object.defineProperty(Layer.prototype, "numProperties", {
    get: function () {
        return this.root.numProperties;
    }
});

var app;

function makeApp() {
    var comp = new CompItem(7, 1 / 25);
    var source = new SourceText("COUPER");
    var opacity = new Property("ADBE Opacity", 100);
    var position = new Property("ADBE Position", [960, 540]);
    position.keys = [{time: 0, value: [960, 540]},
                     {time: 0.4, value: [1000, 540]}];
    comp.layers.push(new Layer(11, [
        new Group("ADBE Text Properties", [source]),
        new Group("ADBE Transform Group", [opacity, position])
    ]));
    var groups = {begun: 0, ended: 0};
    app = {
        version: "26.0",
        project: {
            name: "Essai",
            file: null,
            numItems: 1,
            item: function (index) {
                return index === 1 ? comp : null;
            }
        },
        beginUndoGroup: function () {
            groups.begun += 1;
        },
        endUndoGroup: function () {
            groups.ended += 1;
        }
    };
    return {comp: comp, layer: comp.layers[0], source: source,
            opacity: opacity, position: position, groups: groups};
}

function request(method, args) {
    args.comp_id = "7";
    args.layer_id = "11";
    args.save = false;
    return {schema: "after-request.v2", method: method, arguments: args};
}

// --- the handlers ---------------------------------------------------------

function run(argv) {
    var directory = argv[0];
    var global = this;
    var load = function (name) {
        (0, eval)(read(directory + "/" + name));
        return global.CUTMACHINE_AFTER_HANDLER;
    };
    (0, eval)(read(directory + "/common.jsx"));
    var setText = load("set_text.jsx");
    var setProperty = load("set_property.jsx");
    var setLayerState = load("set_layer_state.jsx");
    var world;
    var response;

    // set_text
    world = makeApp();
    response = setText(request("set_text", {text: "RACCOURCIR",
                                            expected: "COUPER"}));
    check(response.ok && world.source.text === "RACCOURCIR" &&
              response.result.before === "COUPER",
          "set_text writes when the text is the one expected");
    world = makeApp();
    response = setText(request("set_text", {text: "RACCOURCIR",
                                            expected: "COUP"}));
    check(!response.ok && response.error === "StaleValue" &&
              response.detail.indexOf("\"COUPER\"") !== -1 &&
              world.source.writes === 0 && world.groups.begun === 0,
          "set_text refuses when After holds another text, writes nothing, " +
              "opens no undo group, and gives the text found");
    world = makeApp();
    response = setText(request("set_text", {text: ""}));
    check(response.ok && world.source.text === "",
          "set_text without expected writes as before");

    // set_property, static
    world = makeApp();
    response = setProperty(request("set_property", {
        property_path: ["ADBE Transform Group", "ADBE Opacity"],
        value: 50, expected: 100.00000001}));
    check(response.ok && world.opacity.static === 50,
          "set_property writes when the value read back matches, to a " +
              "millionth");
    world = makeApp();
    response = setProperty(request("set_property", {
        property_path: ["ADBE Transform Group", "ADBE Opacity"],
        value: 50, expected: 80}));
    check(!response.ok && response.error === "StaleValue" &&
              response.detail.indexOf("trouvé 100") !== -1 &&
              world.opacity.writes === 0 && world.groups.begun === 0,
          "set_property refuses a value changed by hand in After");

    // set_property, keyed: compared at the frame
    world = makeApp();
    response = setProperty(request("set_property", {
        property_path: ["ADBE Transform Group", "ADBE Position"],
        value: [1100, 540], frame: 10, expected: [1000, 540]}));
    check(response.ok && world.position.keys[1].value[0] === 1100,
          "set_property compares a keyed value at the frame asked");
    world = makeApp();
    response = setProperty(request("set_property", {
        property_path: ["ADBE Transform Group", "ADBE Position"],
        value: [1100, 540], frame: 10, expected: [960, 540]}));
    check(!response.ok && response.error === "StaleValue" &&
              world.position.writes === 0,
          "set_property refuses when the key at that frame moved");
    world = makeApp();
    response = setProperty(request("set_property", {
        property_path: ["ADBE Transform Group", "ADBE Position"],
        value: [1100, 540], frame: 10, expected: [1000]}));
    check(!response.ok && response.error === "StaleValue",
          "an expected value of another length is not the value");

    // A 2D layer's position reads [x, y, 0] and is written [x, y].
    world = makeApp();
    world.opacity.static = [200, 100, 0];
    response = setProperty(request("set_property", {
        property_path: ["ADBE Transform Group", "ADBE Opacity"],
        value: [210, 100], expected: [200, 100]}));
    check(response.ok,
          "an expected [x, y] matches [x, y, 0] as After reads it");
    world = makeApp();
    world.opacity.static = [200, 100, 5];
    response = setProperty(request("set_property", {
        property_path: ["ADBE Transform Group", "ADBE Opacity"],
        value: [210, 100], expected: [200, 100]}));
    check(!response.ok && response.error === "StaleValue",
          "but not [x, y, z] with z moved");

    // set_layer_state
    world = makeApp();
    response = setLayerState(request("set_layer_state", {
        enabled: false, expected: {enabled: true, in_frame: 0}}));
    check(response.ok && world.layer.enabled === false,
          "set_layer_state writes when every state expected holds");
    world = makeApp();
    world.layer.inPoint = 0.2;
    response = setLayerState(request("set_layer_state", {
        enabled: false, expected: {enabled: true, in_frame: 0}}));
    check(!response.ok && response.error === "StaleValue" &&
              response.detail.indexOf("in_frame") !== -1 &&
              response.detail.indexOf("trouvé 5") !== -1 &&
              world.layer.enabled === true && world.groups.begun === 0,
          "set_layer_state names the state that moved and writes nothing");

    if (failures.length) {
        throw new Error(failures.length + " failure(s):\n" +
                        failures.join("\n"));
    }
    return "after jsx: ok";
}
