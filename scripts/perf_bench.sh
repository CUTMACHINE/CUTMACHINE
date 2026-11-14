#!/bin/zsh
# PERF8 -- ROADMAP.md. Runs the save and load benchmarks on the volume a
# project lives on, warm then cold, and prints the medians across passes.
#
#   scripts/perf_bench.sh <folder on the volume> [project.cutmachine.json ...]
#
# The benchmarks write their fixture under <folder>, so its reads and fsyncs
# hit that volume; the folder they create is removed at the end. A real
# project given after it is only read (--project): the session lock is taken
# and released, nothing else is written. The editor must not have it open.
#
# Cold opens run `sudo purge` first, which asks for the password once.
# CUTMACHINE_BENCH_NO_PURGE=1 skips it: every open is then warm, and the
# result says so.

set -eu

repo_dir="${0:A:h:h}"
build_dir="${CUTMACHINE_BENCH_BUILD_DIR:-${repo_dir}/build}"
passes="${CUTMACHINE_BENCH_PASSES:-5}"
save_bench="${build_dir}/cutmachine_save_benchmark_tests"
load_bench="${build_dir}/cutmachine_load_benchmark_tests"

if (( $# < 1 )); then
    print -u2 "usage: scripts/perf_bench.sh <dossier sur le volume> [project.cutmachine.json ...]"
    exit 2
fi
target="${1:A}"
shift
for binary in "${save_bench}" "${load_bench}"; do
    if [[ ! -x "${binary}" ]]; then
        print -u2 "${binary} manque : cmake --build ${build_dir} d'abord"
        exit 2
    fi
done


stamp="$(date +%Y%m%d-%H%M%S)"
bench_dir="${target}/CUTMACHINE_BENCH_${stamp}"
result="${build_dir}/perf8-${stamp}.txt"
mkdir -p "${bench_dir}"
trap 'rm -rf "${bench_dir}"' EXIT

purge_cache() {
    if [[ "${CUTMACHINE_BENCH_NO_PURGE:-0}" == 1 ]]; then
        return
    fi
    sudo purge
}

{
    print "# PERF8 $(date '+%Y-%m-%d %H:%M:%S')"
    print "# commit $(git -C "${repo_dir}" rev-parse --short HEAD 2>/dev/null || print '?')"
    print "# machine $(sysctl -n hw.model) macOS $(sw_vers -productVersion)"
    print "# volume $(df -h "${target}" | tail -1)"
    print "# file system $(diskutil info "${target}" 2>/dev/null | awk -F': *' '/File System Personality/ {print $2}')"
    print "# passes ${passes}, purge $([[ "${CUTMACHINE_BENCH_NO_PURGE:-0}" == 1 ]] && print non || print oui)"

    # Writes and warm reads: the page cache does not absorb an F_FULLFSYNC.
    for pass in {1..${passes}}; do
        print "== warm save"
        TMPDIR="${bench_dir}" "${save_bench}"
        print "== warm load"
        TMPDIR="${bench_dir}" "${load_bench}"
    done

    # Cold opens: the fixture written on the volume, then the page cache
    # dropped before each pass. Only the first step of each pass is cold.
    fixture="$("${load_bench}" --create-fixture "${bench_dir}")"
    for pass in {1..${passes}}; do
        purge_cache
        print "== cold fixture"
        "${load_bench}" --project "${fixture}"
    done
    for project in "$@"; do
        for pass in {1..${passes}}; do
            purge_cache
            print "== cold ${project:A:h:t}"
            "${load_bench}" --project "${project:A}"
        done
    done
} 2>&1 | tee "${result}"

# The median, across passes, of each step's median.
python3 - "${result}" <<'PYTHON'
import re
import statistics
import sys

section = None
values = {}
order = []
s8 = re.compile(r"^  (project operation \(CLI\)|timeline edit \(CLI/MCP\)|"
                r"reopen project \+ journals|editor save of one edit)"
                r"\s+([0-9.]+) ms")
for line in open(sys.argv[1], encoding="utf-8"):
    if line.startswith("== "):
        section = line[3:].strip()
        continue
    fields = line.split()
    match = s8.match(line)
    if match:
        key = (section, match.group(1))
        value = float(match.group(2))
    elif section and len(fields) == 8 and fields[1].isdigit():
        key = (section, fields[0])
        value = float(fields[2])
    else:
        continue
    if key not in values:
        order.append(key)
        values[key] = []
    values[key].append(value)

print("\nmédianes sur les passes (ms)")
for key in order:
    runs = values[key]
    print(f"{key[0]:<22} {key[1]:<28} {statistics.median(runs):9.1f}"
          f"   ({len(runs)} passes, {min(runs):.1f} à {max(runs):.1f})")
PYTHON
print "\nrésultat complet : ${result}"
