#!/bin/bash
# MOTION-WEB-2026-10 MW4 -- the comparison of compare.mjs, prepared and run
# on the Mac from the repository root, after a build:
#   tools/motion-web-import/compare-on-mac.sh <pieces-dir> [names...]
# <pieces-dir>: the unpacked bibliotheque-hyperframes folder.
# Needs node 22+, ffmpeg and git; installs Playwright's Chromium once.
set -euo pipefail
pieces="$1"; shift
here="$(cd "$(dirname "$0")" && pwd)"
work="${WORK:-$HOME/.cache/cutmachine-motion-web}"
mkdir -p "$work"
if [ ! -d "$work/hf" ]; then
  git clone --filter=blob:limit=2m https://github.com/heygen-com/hyperframes "$work/hf"
  git -C "$work/hf" checkout -q 8bca7c4
fi
if [ ! -d "$work/cdn/node_modules/gsap" ]; then
  mkdir -p "$work/cdn" && (cd "$work/cdn" && npm init -y >/dev/null && \
    npm i gsap@3.14.2 three@0.181.0 three147@npm:three@0.147.0 lottie-web d3 \
      topojson-client d3-delaunay clipper-lib us-atlas@3 playwright@1.56.1 >/dev/null && \
    npx playwright install chromium)
fi
export PLAYWRIGHT_MODULE="$work/cdn/node_modules/playwright/index.mjs"
export CUTMACHINE="${CUTMACHINE:-$PWD/build/cutmachine}"
out="${OUT:-$PWD/build/motion-web-comparison}"
node "$here/compare.mjs" "$work/hf/registry" "$work/cdn/node_modules" "$pieces" "$out" "$@"
echo "Rapport : $out/comparaison.md"
