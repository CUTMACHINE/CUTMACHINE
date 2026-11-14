#!/bin/sh
# SN6 -- docs/roadmap/SON-2026-10.md. PROTOTYPE: one render of each workload, in each engine.
#   tools/cutmachine-sound/jsc/bench.sh <work-folder>
# Node with node:vm (the renderer as it is), Node running the JSC bundle (no node:vm), the jsc
# shell, and a JSContext host signed without and with com.apple.security.cs.allow-jit (and the
# hardened runtime). Prints seconds of sound per second of render and the samples' FNV-1a.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:?a work folder}
mkdir -p "$work"
jsc=/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc
node "$here/jsc/bundle.mjs" > "$work/runtime-jsc.js"
node "$here/cli.mjs" edit --edl "$here/examples/demo.edl" --fps 25 > "$work/demo-edit.json"
swiftc -O "$here/jsc/host.swift" -o "$work/host"
cat > "$work/jit.entitlements" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>
PLIST
cp "$work/host" "$work/host-nojit"
codesign -f -s - --options runtime "$work/host-nojit" 2>/dev/null
codesign -f -s - --options runtime --entitlements "$work/jit.entitlements" "$work/host" 2>/dev/null
cat > "$work/driver.js" <<'JS'
const [ditty, editFile, duration, partition] = arguments;
const job = { code: read(ditty) };
if (editFile !== '-') job.edit = JSON.parse(read(editFile)); else job.duration = Number(duration);
if (partition) job.partition = JSON.parse(read(partition));
print(benchRender(JSON.stringify(job)));
JS
cat > "$work/node-driver.cjs" <<'JS'
const fs = require('fs');
const [bundle, ditty, editFile, duration, partition] = process.argv.slice(2);
(0, eval)(fs.readFileSync(bundle, 'utf8'));
const job = { code: fs.readFileSync(ditty, 'utf8') };
if (editFile !== '-') job.edit = JSON.parse(fs.readFileSync(editFile, 'utf8')); else job.duration = Number(duration);
if (partition) job.partition = JSON.parse(fs.readFileSync(partition, 'utf8'));
console.log(globalThis.benchRender(JSON.stringify(job)));
JS
cat > "$work/node-vm.mjs" <<'JS'
const [runtime, bundle, ditty, editFile, duration, partition] = process.argv.slice(2);
const fs = await import('node:fs');
const { render } = await import(runtime);
const src = fs.readFileSync(bundle, 'utf8');
const fnv24 = new Function(src.slice(src.indexOf('function fnv24'), src.indexOf('globalThis.benchRender')) + '; return fnv24;')();
const job = { code: fs.readFileSync(ditty, 'utf8') };
if (editFile !== '-') job.edit = JSON.parse(fs.readFileSync(editFile, 'utf8')); else job.duration = Number(duration);
if (partition) job.partition = JSON.parse(fs.readFileSync(partition, 'utf8'));
const t0 = Date.now(); const r = render(job); const ms = Date.now() - t0;
console.log(JSON.stringify({ seconds: r.left.length / r.sampleRate, ms, fnv: fnv24(r.left, r.right) }));
JS
row() {  # label, then the command printing {"seconds","ms","fnv"}
    label=$1; shift
    "$@" | tail -1 | python3 -c "import json,sys; r=json.loads(sys.stdin.read()); print(f'  {sys.argv[1]:<28} {r[\"ms\"]:>7} ms  {r[\"seconds\"]/r[\"ms\"]*1000:6.2f}x  fnv {r[\"fnv\"]}')" "$label"
}
host_row() {  # label, binary, then host arguments
    label=$1; bin=$2; shift 2
    SOUND_HOST_FRESH=1 "$bin" "$work/runtime-jsc.js" "$@" | tail -1 |
        sed -E "s/run [0-9]+: ([0-9.]+) s of sound in ([0-9]+) ms, ([0-9.]+)x real time, fnv ([0-9]+).*/  $(printf '%-28s' "$label") \2 ms  \3x  fnv \4/"
}
workload() {  # name, ditty, edit|-, duration, [partition]
    name=$1; ditty=$2; edit=$3; duration=$4; partition=${5:-}
    echo "== $name"
    row "node, node:vm" node "$work/node-vm.mjs" "$here/runtime.mjs" "$work/runtime-jsc.js" "$ditty" "$edit" "$duration" $partition
    row "node, no node:vm" node "$work/node-driver.cjs" "$work/runtime-jsc.js" "$ditty" "$edit" "$duration" $partition
    row "jsc shell" "$jsc" "$work/runtime-jsc.js" "$work/driver.js" -- "$ditty" "$edit" "$duration" $partition
    if [ "$edit" = "-" ]; then
        host_row "JSContext, allow-jit" "$work/host" "$ditty" - "$duration" 2
        host_row "JSContext, hardened, no jit" "$work/host-nojit" "$ditty" - "$duration" 1
    else
        host_row "JSContext, allow-jit" "$work/host" "$ditty" "$edit" 2 $partition
        host_row "JSContext, hardened, no jit" "$work/host-nojit" "$ditty" "$edit" 1 $partition
    fi
}
workload "impacts.js, demo.edl (20.4 s)" "$here/ditties/impacts.js" "$work/demo-edit.json" 0
workload "verre.js + partition, demo.edl (20.4 s)" "$here/instruments/verre.js" "$work/demo-edit.json" 0 "$here/partitions/impacts-demo.json"
workload "brume.js (60 s)" "$here/ditties/brume.js" - 60
for extra in ${SOUND_BENCH_EXTRA:-}; do workload "$(basename "$extra") (60 s)" "$extra" - 60; done
