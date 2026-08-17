#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PEBBLE_DIR="$ROOT_DIR/pebblecode"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/dist/pebble-screenshots/$(date +%Y%m%d-%H%M%S)}"
SDK_IMAGE="${SDK_IMAGE:-rebble/pebble-sdk}"
EMULATOR="${EMULATOR:-emery}"
TOTAL_TIMEOUT="${TOTAL_TIMEOUT:-1200s}"
STEP_TIMEOUT="${STEP_TIMEOUT:-120s}"
PAGE_HOLD_SCALE="${PAGE_HOLD_SCALE:-4}"
CAPTURE_TRIES="${CAPTURE_TRIES:-4}"

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required; the local pebble command is a Docker wrapper and cannot keep one emulator alive across commands." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "Writing Pebble Time 2 screenshots to $OUT_DIR"
echo "Emulator step timeout is $STEP_TIMEOUT; raise it with STEP_TIMEOUT=180s if your machine boots QEMU slowly."

cat >"$OUT_DIR/manifest.txt" <<EOF
T3 Pebble screenshot run
Emulator: $EMULATOR
Fixture mode: SCREENSHOT_FIXTURES forced true at build time

Files:
- 00-host-dashboard.png
- 01-thread-list.png
- 02-thread-detail.png
- 03-thread-transcript.png
- 04-diagnostics.png
- 05-host-offline.png

The fixture storyboard is driven by src/pkjs/index.js and CMD_SCREENSHOT_PAGE in src/c/main.c.
EOF

if ! timeout "$TOTAL_TIMEOUT" docker run --rm \
  -e EMULATOR="$EMULATOR" \
  -e STEP_TIMEOUT="$STEP_TIMEOUT" \
  -e PAGE_HOLD_SCALE="$PAGE_HOLD_SCALE" \
  -e CAPTURE_TRIES="$CAPTURE_TRIES" \
  -v "$PEBBLE_DIR:/pebblecode:ro" \
  -v "$OUT_DIR:/shots" \
  -w /pebblecode \
  "$SDK_IMAGE" \
  bash -c '
set -euo pipefail

# QEMU opens an SDL window as soon as the firmware console connects, and the
# SDK image has no video device, so it dies with "Could not initialize SDL"
# before the firmware ever boots -- which surfaces as pebble hanging on
# "Waiting for the firmware to boot". pebble-tool has no flag for this, but it
# honours PEBBLE_QEMU_PATH, so the display is turned off in a wrapper.
cat >/usr/local/bin/qemu-pebble-headless <<"WRAP"
#!/bin/sh
exec qemu-pebble "$@" -display none
WRAP
chmod +x /usr/local/bin/qemu-pebble-headless
export PEBBLE_QEMU_PATH=/usr/local/bin/qemu-pebble-headless

# pebble-tool is Python 2 and block-buffers stdout when it is redirected, so a
# step killed by `timeout` would otherwise lose everything it had printed.
export PYTHONUNBUFFERED=1

# The fixtures are a build-time switch, not a runtime one. pypkjs has no
# `process` global, so the T3PEBBLE_SCREENSHOT_FIXTURES environment variable
# this script used to pass never reached the bridge and the storyboard never
# ran. The app is built from a copy so the checkout is left untouched.
# The staging directory keeps the project name, because waf names the bundle
# after the directory it builds in: staging as /work would produce work.pbw.
echo "staging a fixture build in /work/pebblecode"
mkdir -p /work/pebblecode
tar -C /pebblecode --exclude=./build --exclude=./.lock-waf_linux_build -cf - . | tar -C /work/pebblecode -xf -
cd /work/pebblecode
python2 - <<PATCH
import re
PAGE_HOLD_SCALE = $PAGE_HOLD_SCALE
path = "src/pkjs/index.js"
src = open(path).read()
patched, count = re.subn(r"var\s+SCREENSHOT_FIXTURES\s*=.*?;",
                         "var SCREENSHOT_FIXTURES = true;", src, count=1, flags=re.S)
if count != 1:
    raise SystemExit("could not find the SCREENSHOT_FIXTURES declaration in " + path)

# The storyboard timings are stretched for the same reason the captures
# below retry: a screenshot only lands between painted frames, and on QEMU a
# frame is slow enough that the handshake often misses. Holding each page four
# times longer turns a one-shot race into a window several attempts fit inside.
body = re.search(r"function runScreenshotStoryboard\(\) \{.*?\n\}\n", patched, re.S)
if not body:
    raise SystemExit("could not find runScreenshotStoryboard in " + path)
stretched = re.sub(r"\}, (\d+)\);",
                   lambda m: "}, %d);" % (int(m.group(1)) * PAGE_HOLD_SCALE),
                   body.group(0))
patched = patched.replace(body.group(0), stretched, 1)

open(path, "w").write(patched)
print("fixtures forced on and storyboard stretched %dx for this build" % PAGE_HOLD_SCALE)
PATCH

# Every capture is scheduled against the moment the app launches, because that
# is when pypkjs fires `ready` and the storyboard starts its own timers. Waiting
# a fixed amount between captures instead lets the two clocks drift apart, and a
# screenshot that takes a few seconds then lands on the following page.
wait_until() {
  local target_ms="$1" now_ms sleep_ms
  now_ms=$(( $(date +%s%3N) - T0_MS ))
  sleep_ms=$(( target_ms - now_ms ))
  if [ "$sleep_ms" -gt 0 ]; then
    sleep "$(printf "%d.%03d" $((sleep_ms / 1000)) $((sleep_ms % 1000)))"
  fi
}

FAILED=""

# A step killed by `timeout` takes the emulator with it: pebble-tool spawns QEMU
# and pypkjs as its children, so every later step fails with "Connection
# refused" rather than a timeout. Captures are non-fatal so a partial set still
# reaches $OUT_DIR, and the names of the missing ones are reported at the end.
# A screenshot is only answered between painted frames, and a frame on QEMU is
# slow enough that a single attempt is a coin flip -- the same build captures
# one run and times out the next. Each page is held long enough for several
# tries, so a miss costs an attempt rather than the whole run.
capture() {
  local at_ms="$1" name="$2" try=1
  wait_until "$at_ms"
  while [ "$try" -le "$CAPTURE_TRIES" ]; do
    echo "capturing $name at +${at_ms}ms (attempt $try/$CAPTURE_TRIES)"
    if timeout "$STEP_TIMEOUT" pebble screenshot --emulator "$EMULATOR" --no-open --no-correction "/shots/$name.png"; then
      return
    fi
    try=$(( try + 1 ))
  done
  FAILED="$FAILED $name"
}

pebble kill >/dev/null 2>&1 || true
# The watch half of the rig is a build-time switch too: CMD_SCREENSHOT_PAGE and
# show_screenshot_page() are behind SCREENSHOT_BUILD, which wscript defines from
# this variable. A released app must not accept a command that rearranges its
# window stack, so a normal `pebble build` leaves it out.
export T3PEBBLE_SCREENSHOT_FIXTURES=1
timeout "$STEP_TIMEOUT" pebble build
echo "installing build/pebblecode.pbw on $EMULATOR"
timeout "$STEP_TIMEOUT" pebble install --emulator "$EMULATOR" build/pebblecode.pbw
T0_MS=$(date +%s%3N)

# The storyboard sets each page at 1200/4300/8300/12300/16300/19600ms and loads
# the next screens fixture ~3.3s later, so each capture goes out about 600ms
# after its page lands, leaving the rest of the window for the transfer.
capture $((  1800 * PAGE_HOLD_SCALE )) 00-host-dashboard
capture $((  4900 * PAGE_HOLD_SCALE )) 01-thread-list
capture $((  8900 * PAGE_HOLD_SCALE )) 02-thread-detail
capture $(( 12900 * PAGE_HOLD_SCALE )) 03-thread-transcript
capture $(( 16900 * PAGE_HOLD_SCALE )) 04-diagnostics
capture $(( 20200 * PAGE_HOLD_SCALE )) 05-host-offline

pebble kill >/dev/null 2>&1 || true

if [ -n "$FAILED" ]; then
  echo "screenshots that did not arrive:$FAILED" >&2
  exit 1
fi
'
then
  cat >&2 <<'MSG'

Capture failed. Partial output and the manifest are in the output directory.

If the screenshots time out while a stock emulator works, the watch app is
wedging the firmware on its first paint rather than the rig being broken.
GTextOverflowModeTrailingEllipsis with the custom Dot6x10 fonts is one known
cause: the fonts carry no U+2026, and the layout never returns. Check it with a
throwaway build that points font_legend() at a system font.
MSG
  echo "Output directory: $OUT_DIR" >&2
  exit 1
fi

echo "Done. Screenshots are in $OUT_DIR"
