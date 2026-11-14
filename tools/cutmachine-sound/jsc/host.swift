// SN6 -- docs/roadmap/SON-2026-10.md. PROTOTYPE, not production code: the sound runtime in a
// JSContext, as the app would hold it, without Node. Measures one render and prints the samples'
// FNV-1a (as the WAV would write them) to compare with Node.
//
//   swiftc -O jsc/host.swift -o jsc-host
//   ./jsc-host runtime-jsc.js ditties/impacts.js demo-edit.json [repeats]
//   ./jsc-host runtime-jsc.js ditties/brume.js - 60 [repeats]       (no edit: a duration)
//   ./jsc-host runtime-jsc.js instruments/verre.js demo-edit.json 1 partitions/impacts-demo.json

import Foundation
import JavaScriptCore

func read(_ path: String) -> String {
    guard let text = try? String(contentsOfFile: path, encoding: .utf8) else {
        FileHandle.standardError.write("cannot read \(path)\n".data(using: .utf8)!)
        exit(2)
    }
    return text
}

let args = CommandLine.arguments
guard args.count >= 4 else {
    print("usage: jsc-host <runtime-jsc.js> <ditty.js> <edit.json | -> [duration-or-repeats] [repeats] [partition.json]")
    exit(2)
}
let bundle = read(args[1])
let ditty = read(args[2])
let editPath = args[3]
var job: [String: Any] = ["code": ditty]
var repeats = 3
if editPath == "-" {
    job["duration"] = Double(args.count > 4 ? args[4] : "60") ?? 60
    if args.count > 5 { repeats = Int(args[5]) ?? 3 }
} else {
    job["edit"] = try! JSONSerialization.jsonObject(with: read(editPath).data(using: .utf8)!)
    if args.count > 4 { repeats = Int(args[4]) ?? 3 }
    if args.count > 5 {
        job["partition"] = try! JSONSerialization.jsonObject(with: read(args[5]).data(using: .utf8)!)
    }
}
let jobText = String(data: try! JSONSerialization.data(withJSONObject: job), encoding: .utf8)!

// SOUND_HOST_FRESH=1: a new context for every render, as the app could hold one per render.
let fresh = ProcessInfo.processInfo.environment["SOUND_HOST_FRESH"] == "1"
var loadMs = 0.0
func makeRender() -> JSValue {
    let context = JSContext()!
    context.exceptionHandler = { _, exception in
        FileHandle.standardError.write("JS exception: \(exception?.toString() ?? "?")\n".data(using: .utf8)!)
        exit(1)
    }
    let loadStart = DispatchTime.now()
    context.evaluateScript(bundle)
    loadMs = Double(DispatchTime.now().uptimeNanoseconds - loadStart.uptimeNanoseconds) / 1e6
    return context.objectForKeyedSubscript("benchRender")!
}
var benchRender = makeRender()

for run in 1...repeats {
    if fresh && run > 1 { benchRender = makeRender() }
    let start = DispatchTime.now()
    let answer = benchRender.call(withArguments: [jobText])!.toString()!
    let ms = Double(DispatchTime.now().uptimeNanoseconds - start.uptimeNanoseconds) / 1e6
    let result = try! JSONSerialization.jsonObject(with: answer.data(using: .utf8)!) as! [String: Any]
    let seconds = result["seconds"] as! Double
    print(String(format: "run %d: %.1f s of sound in %.0f ms, %.2fx real time, fnv %@ (bundle %.0f ms)",
                 run, seconds, ms, seconds * 1000 / ms, "\(result["fnv"]!)", loadMs))
}
