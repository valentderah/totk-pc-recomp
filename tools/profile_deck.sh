#!/bin/bash
# Measure the game on a Steam Deck, and print what makes the number mean something.
#
#   tools/profile_deck.sh --install /run/media/deck/GF8S5/totk-install --seconds 60
#   tools/profile_deck.sh --install ... --binary /path/to/new/totk      # a build the installer did not place
#
# The number alone is not evidence: a Deck halves its frame rate when the APU drops a power state, when the present
# mode changes, or when the internal resolution grows, and all three look identical in an fps line. So this records the
# clock during the run, the power state, and the header lines the runtime prints about itself.
set -euo pipefail

INSTALL=""
BINARY=""
SECONDS_TO_RUN=90      # the runtime prints a summary every 60 s of runtime, and one on exit
WARMUP=10

while [ $# -gt 0 ]; do
  case "$1" in
    --install) INSTALL=$2; shift 2 ;;
    --binary)  BINARY=$2;  shift 2 ;;
    --seconds) SECONDS_TO_RUN=$2; shift 2 ;;
    --warmup)  WARMUP=$2;  shift 2 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$INSTALL" ] || { echo "--install is required" >&2; exit 2; }
[ -x "$INSTALL/totk" ] || { echo "$INSTALL/totk is not executable" >&2; exit 2; }

# A swapped binary may carry another machine's baked paths, so name the folders explicitly rather than trusting them.
RUN_BIN=$INSTALL/totk
if [ -n "$BINARY" ]; then
  cp -f "$BINARY" "$INSTALL/totk.profile" && chmod +x "$INSTALL/totk.profile"
  RUN_BIN=$INSTALL/totk.profile
fi

sclk() { cat /sys/class/drm/card*/device/pp_dpm_sclk 2>/dev/null | grep '\*' | awk '{print $2}' | paste -sd, - ; }
power() {
  for p in /sys/class/power_supply/*; do
    printf '%s=%s(%s%%) ' "$(basename "$p")" "$(cat "$p/status" 2>/dev/null)" "$(cat "$p/capacity" 2>/dev/null)"
  done
}

echo "device:   $(uname -r), $(ls /usr/lib/libSDL3.so* 2>/dev/null | head -1 | xargs -r basename)"
echo "vulkan:   $(vulkaninfo --summary 2>/dev/null | awk -F= '/deviceName/{print $2; exit}' | xargs)"
echo "power:    $(power)"
echo "thermals: $(cat /sys/class/drm/card*/device/hwmon/hwmon*/temp1_input 2>/dev/null | head -1 | awk '{print $1/1000 " C"}')"
echo "binary:   $RUN_BIN ($(stat -c %s "$RUN_BIN") bytes, $(stat -c %y "$RUN_BIN" | cut -d. -f1))"
echo

cd "$INSTALL"
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-wayland}
LOG=$(mktemp /tmp/deck-profile-XXXX.log)

# The three environment variables above are the whole difference between "no graphics+compute+present queue family"
# and a measurement; the runtime reads a display even for the probes.
"$RUN_BIN" --exefs ./exefs --romfs ./romfs --profile ./profile > "$LOG" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT

sleep "$WARMUP"
: > /tmp/deck-sclk.txt
for _ in $(seq 1 $((SECONDS_TO_RUN / 2))); do
  kill -0 $PID 2>/dev/null || break
  sclk >> /tmp/deck-sclk.txt
  sleep 2
done
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true

echo "gpu clock during the run: $(sort /tmp/deck-sclk.txt | uniq -c | sort -rn | head -3 | awk '{printf "%s x%d, ", $2, $1}')"
echo "power at the end:         $(power)"
echo
grep -E "device profile|frame rate: limit|present mode|resolution: output|shadow storage|ASTC textures|patches:" "$LOG" | sed 's/^\[host\] //'
echo
# Both matter: a 60 s window when the run lasted that long, and the exit summary when it did not.
grep -E "frametime: ([0-9]+ s|exit):" "$LOG" | sed 's/^\[host\] /  /' || echo "  no frametime summary: the run ended early, see $LOG"
echo
echo "log: $LOG"
