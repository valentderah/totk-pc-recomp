#!/bin/bash
# Measure the game on a Steam Deck (or any Linux machine) and keep the log with what makes the number mean something.
#
#   tools/profile_deck.sh --install /run/media/deck/GF8S5/totk-install --script tools/input/walk_pan.txt --tag base
#   tools/profile_deck.sh --install ... --binary /path/to/new/totk --tag perf    # a build the installer did not place
#   tools/profile_deck.sh --install ... --tag ubdefer --env TOTK_AB=10 --env TOTK_UB_DEFER=ab
#
# The number alone is not evidence: a Deck halves its frame rate when the APU drops a power state, when the present
# mode changes, or when the internal resolution grows, and all three look identical in an fps line. So this records the
# clock during the run, the power state, and the header lines the runtime prints about itself, and keeps every run's
# log under <install>/profiles/ (--log-dir), named <UTC time>-<build>-<tag>.log, with the clock samples beside it and
# one line per run in summary.tsv. TOTK_TRACE=perf,sched is on, so tools/perf_summary.py reads the saved log.
set -uo pipefail

INSTALL=""
BINARY=""
SECONDS_TO_RUN=90      # of world clock, after the save loads: the game ends itself (--seconds), no kill needed
WARMUP=10              # seconds before the clock sampling starts (the load)
SCRIPT=""
TAG="run"
LOG_DIR=""
FPS_LIMIT=""
ENVS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --install)   INSTALL=$2; shift 2 ;;
    --binary)    BINARY=$2;  shift 2 ;;
    --seconds)   SECONDS_TO_RUN=$2; shift 2 ;;
    --warmup)    WARMUP=$2;  shift 2 ;;
    --script)    SCRIPT=$2;  shift 2 ;;
    --tag)       TAG=$2;     shift 2 ;;
    --log-dir)   LOG_DIR=$2; shift 2 ;;
    --fps-limit) FPS_LIMIT=$2; shift 2 ;;
    --env)       ENVS+=("$2"); shift 2 ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$INSTALL" ] || { echo "--install is required" >&2; exit 2; }
[ -x "$INSTALL/totk" ] || { echo "$INSTALL/totk is not executable" >&2; exit 2; }
if [ -n "$SCRIPT" ]; then
  [ -f "$SCRIPT" ] || { echo "$SCRIPT: no such script" >&2; exit 2; }
  SCRIPT=$(readlink -f "$SCRIPT")
fi
LOG_DIR=${LOG_DIR:-$INSTALL/profiles}
mkdir -p "$LOG_DIR" || exit 2

# A swapped binary may carry another machine's baked paths, so name the folders explicitly rather than trusting them.
RUN_BIN=$INSTALL/totk
if [ -n "$BINARY" ]; then
  cp -f "$BINARY" "$INSTALL/totk.profile" && chmod +x "$INSTALL/totk.profile" || exit 2
  RUN_BIN=$INSTALL/totk.profile
fi

# The build's name: the release that made the install (totk-install.json), or the binary's own hash when it was swapped.
build_id() {
  if [ -z "$BINARY" ] && [ -f "$INSTALL/totk-install.json" ]; then
    sed -n 's/^ *"release": *"\([^"]*\)".*/\1/p' "$INSTALL/totk-install.json" | head -1 | tr '+' '-'
  else
    sha256sum "$RUN_BIN" | cut -c1-8
  fi
}
BUILD=$(build_id); BUILD=${BUILD:-unknown}
STAMP=$(date -u +%Y%m%d-%H%M%S)
LOG=$LOG_DIR/$STAMP-$BUILD-$TAG.log
SCLK=$LOG_DIR/$STAMP-$BUILD-$TAG.sclk

sclk() { cat /sys/class/drm/card*/device/pp_dpm_sclk 2>/dev/null | grep '\*' | awk '{print $2}' | paste -sd, - ; }
power() {
  local out="" p
  for p in /sys/class/power_supply/*; do
    [ -e "$p/status" ] || continue
    out+="$(basename "$p")=$(cat "$p/status" 2>/dev/null)($(cat "$p/capacity" 2>/dev/null || echo ?)%) "
  done
  echo "${out:-n/a}"
}
thermal() { cat /sys/class/drm/card*/device/hwmon/hwmon*/temp1_input 2>/dev/null | head -1 | awk '{print $1/1000 " C"}'; }

{
  echo "device:   $(uname -r), $(ls /usr/lib/libSDL3.so* 2>/dev/null | head -1 | xargs -r basename)"
  echo "vulkan:   $(vulkaninfo --summary 2>/dev/null | awk -F= '/deviceName/{print $2; exit}' | xargs || echo n/a)"
  echo "power:    $(power)"
  echo "thermals: $(thermal)"; [ -n "$(thermal)" ] || echo "thermals: n/a"
  echo "binary:   $RUN_BIN ($(stat -c %s "$RUN_BIN") bytes, $(stat -c %y "$RUN_BIN" | cut -d. -f1)), build $BUILD"
  echo "run:      tag $TAG, $SECONDS_TO_RUN s of world clock${SCRIPT:+, script $SCRIPT}${FPS_LIMIT:+, fps limit $FPS_LIMIT}${ENVS[*]:+, env ${ENVS[*]}}"
  echo
} | tee "$LOG.head"

cd "$INSTALL" || exit 2
# The display: the runtime reads one even for the probes. These are the Deck's names; a desktop session already has its own.
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [ -z "${DISPLAY:-}" ]; then
  export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-0}
  export SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-wayland}
fi
export TOTK_TRACE=${TOTK_TRACE:-perf,sched}
[ -n "$SCRIPT" ] && export TOTK_INPUT_SCRIPT=$SCRIPT
[ -n "$FPS_LIMIT" ] && export TOTK_FPS_LIMIT=$FPS_LIMIT
for kv in "${ENVS[@]}"; do export "$kv"; done

"$RUN_BIN" --exefs ./exefs --romfs ./romfs --profile ./profile --seconds "$SECONDS_TO_RUN" > "$LOG.body" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT

sleep "$WARMUP"
: > "$SCLK"
# Sample the clock every 2 s until the game ends by itself; a run that never loads is stopped after the load allowance.
for _ in $(seq 1 $(( (SECONDS_TO_RUN + 300) / 2 ))); do
  kill -0 $PID 2>/dev/null || break
  sclk >> "$SCLK"
  sleep 2
done
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null; CODE=$?
trap - EXIT

CLOCKS=$(sort "$SCLK" | grep . | uniq -c | sort -rn | head -3 | awk '{printf "%s x%d, ", $2, $1}')
{
  echo "gpu clock during the run: ${CLOCKS:-n/a}"
  echo "power at the end:         $(power)"
  echo "exit code:                $CODE"
  echo
  grep -E "device profile|frame rate: limit|present mode|resolution: output|shadow storage|ASTC textures|patches:|text:" "$LOG.body" | sed 's/^\[host\] //'
  echo
  # Both matter: a 60 s window when the run lasted that long, and the exit summary when it did not.
  grep -E "frametime: ([0-9]+ s|exit):" "$LOG.body" | sed 's/^\[host\] /  /' || echo "  no frametime summary: the run ended early, see $LOG"
  echo
} | tee "$LOG.tail"
cat "$LOG.head" "$LOG.tail" "$LOG.body" > "$LOG" && rm -f "$LOG.head" "$LOG.tail" "$LOG.body"

# One line per run: when, build, tag, the exit summary's mean and 1% low, the last perf line's GPU time and bound, clocks.
SUMMARY=$LOG_DIR/summary.tsv
[ -f "$SUMMARY" ] || printf 'time\tbuild\ttag\tfps\tlow1\tgpu_ms\tbound\tclocks\tlog\n' > "$SUMMARY"
EXIT_LINE=$(grep -E "frametime: exit:" "$LOG" | tail -1)
FPS=$(echo "$EXIT_LINE" | sed -n 's/.*mean \([0-9.]*\) fps.*/\1/p')
LOW=$(echo "$EXIT_LINE" | sed -n 's/.*1% low \([0-9.]*\) fps.*/\1/p')
PERF_LINE=$(grep -E "perf: frame [0-9]+: gpu avg" "$LOG" | tail -1)
GPU=$(echo "$PERF_LINE" | sed -n 's/.*gpu avg \([0-9.]*\) ms.*/\1/p')
BOUND=$(echo "$PERF_LINE" | sed -n 's/.*bound: \([a-z]*\).*/\1/p')
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$STAMP" "$BUILD" "$TAG" "${FPS:-?}" "${LOW:-?}" "${GPU:-?}" "${BOUND:-?}" "${CLOCKS:-n/a}" "$(basename "$LOG")" >> "$SUMMARY"
echo "log: $LOG"
echo "summary: $SUMMARY"
