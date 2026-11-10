#!/usr/bin/env bash
# Steam Deck benchmark: scripted walks through a save, the same start each run, with the frame rate and the APU's
# power draw of each run in a table. On a Deck it measures the Deck; on another Linux PC, --sim pins the game to four
# cores with their SMT siblings (the Deck's 4 cores / 8 threads) and turns the Deck device profile on, which gives the
# Deck's CPU budget but not its GPU, clocks or 15 W limit, so --sim numbers only rank settings against each other.
#
#   tools/deck_bench.sh [options] <install dir> <config>...
#
# A config is name or name:KEY=VALUE,KEY=VALUE with totk.ini keys (fps_limit=40,replay_async=1); "base" alone runs the
# defaults. The configs run interleaved, A B C A B C ..., --runs times each, so drift (heat, background work) spreads
# over all of them. Before each run the save, its backups and the cache are put back from a copy this script makes once
# in <install dir>/bench-backup: autosaves would otherwise move the start point. Delete that folder to start from a
# newer save.
#
# Options:
#   --runs N        runs per config (default 3)
#   --seconds S     length of each run (default 240: the walk is about 170 s of game time after the save loads)
#   --script FILE   scripted input (default tools/input/walk.txt beside this script)
#   --exe NAME      game executable in the install dir (default totk); exe=NAME in a config picks one per config
#   --sim           PC run: four cores with SMT siblings and the Deck profile's settings (1280x800, the game's 3D
#                   scale, vsync, 40 fps), set through the environment so they win over totk.ini, in a headless
#                   gamescope when it is installed
#   --refresh HZ    --sim: the headless gamescope's refresh rate (default 60). Its Vulkan offers fifo only, so the
#                   game cannot run faster than this; 240 measures how far past 60 a config gets
#   --out FILE      results as CSV (default <install dir>/bench-<date>.csv)
#
# Frames, mean fps and 1% low (time-weighted over the game's 60 s windows) come from the game's log; power is the mean of the amdgpu sensor (on an APU the
# whole chip, CPU and GPU) sampled once a second, the lsfg-vk layer is off (DISABLE_LSFGVK=1) and the screen kept on.
set -euo pipefail

runs=3 seconds=240 script="" exe=totk sim=0 out="" refresh=60
here=$(cd "$(dirname "$0")" && pwd)
while [[ $# -gt 0 && $1 == --* ]]; do
    case $1 in
        --runs) runs=$2; shift 2 ;;
        --seconds) seconds=$2; shift 2 ;;
        --script) script=$2; shift 2 ;;
        --exe) exe=$2; shift 2 ;;
        --sim) sim=1; shift ;;
        --out) out=$2; shift 2 ;;
        --refresh) refresh=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[[ $# -ge 2 ]] || { sed -n '2,30p' "$0"; exit 2; }
dir=$(cd "$1" && pwd); shift
configs=("$@")
script=${script:-$here/input/walk.txt}
[[ -f $script ]] || { echo "no input script $script" >&2; exit 1; }
out=${out:-$dir/bench-$(date +%Y%m%d-%H%M%S).csv}
prof=$dir/profile
backup=$dir/bench-backup
[[ -d $prof/save ]] || { echo "no save in $prof: play to the spot to start from and save there first" >&2; exit 1; }

if [[ ! -d $backup ]]; then
    mkdir -p "$backup"
    for d in save save-backup save-tx cache playtime.txt; do [[ -e $prof/$d ]] && cp -a "$prof/$d" "$backup/"; done
    echo "bench: saved the start point to $backup"
fi
restore() {
    for d in save save-backup save-tx cache playtime.txt; do
        rm -rf "${prof:?}/$d"
        [[ -e $backup/$d ]] && cp -a "$backup/$d" "$prof/"
    done
}

power_file=""
for h in /sys/class/hwmon/hwmon*; do
    if [[ $(cat "$h/name" 2>/dev/null) == amdgpu ]]; then
        for f in power1_average power1_input; do [[ -r $h/$f ]] && { power_file=$h/$f; break 2; }; done
    fi
done

cpus=""
if (( sim )); then
    # The first four cores and their SMT siblings, from the topology the kernel reports
    cpus=$(ls -d /sys/devices/system/cpu/cpu[0-9]* | sort -V |
           while read -r c; do cat "$c/topology/thread_siblings_list"; done | awk '!seen[$0]++' | head -4 | paste -sd, -)
    echo "bench: --sim on CPUs $cpus"
fi

ini_to_env() {   # fps_limit=40 -> TOTK_FPS_LIMIT=40
    local kv=$1 k=${1%%=*}
    echo "TOTK_${k^^}=${kv#*=}"
}

echo "config,run,exe,frames,seconds,mean_fps,low1_fps,over50ms,watts" > "$out"
logdir=$dir/bench-logs; mkdir -p "$logdir"
for ((r = 1; r <= runs; r++)); do
    for cfg in "${configs[@]}"; do
        name=${cfg%%:*}; sets=""; [[ $cfg == *:* ]] && sets=${cfg#*:}
        envs=(DISABLE_LSFGVK=1 "TOTK_INPUT_SCRIPT=$script")
        (( sim )) && envs+=(TOTK_DEVICE_PROFILE=deck TOTK_FULLSCREEN=0 TOTK_WINDOW_SIZE=1280x800 TOTK_OUTPUT_RES=1280x800
                            TOTK_RENDER_SCALE=game TOTK_VK_PRESENT=fifo TOTK_FPS_LIMIT=40)
        run_exe=$exe
        IFS=, read -ra kvs <<< "$sets"
        for kv in "${kvs[@]}"; do
            [[ -z $kv ]] && continue
            if [[ ${kv%%=*} == exe ]]; then run_exe=${kv#*=}; else envs+=("$(ini_to_env "$kv")"); fi
        done
        restore
        log=$logdir/$name-$r.log
        echo "bench: $name run $r/$runs ($run_exe${sets:+, $sets})"
        cmd=(env "${envs[@]}" "$dir/$run_exe")
        # --sim runs the game in a headless gamescope, the compositor of the Deck's Game Mode: it presents with the
        # desktop locked or the screen off, and shares the four cores with the game as it does on the Deck
        (( sim )) && command -v gamescope >/dev/null &&
            cmd=(gamescope --backend headless -W 1280 -H 800 -w 1280 -h 800 -r "$refresh" -- "${cmd[@]}")
        [[ -n $cpus ]] && cmd=(taskset -c "$cpus" "${cmd[@]}")
        cmd=(timeout -s TERM -k 20 "$seconds" "${cmd[@]}")
        if command -v systemd-inhibit >/dev/null; then
            cmd=(systemd-inhibit --what=idle:sleep --who=deck_bench --why="game benchmark" "${cmd[@]}")
        fi
        samples=$logdir/$name-$r.power
        : > "$samples"
        if [[ -n $power_file ]]; then
            ( while sleep 1; do cat "$power_file"; done >> "$samples" ) &
            sampler=$!
        fi
        (cd "$dir" && "${cmd[@]}") > "$log" 2>&1 || true
        [[ -n $power_file ]] && { kill "$sampler" 2>/dev/null || true; wait "$sampler" 2>/dev/null || true; }
        # The game logs a frametime line every 60 s and one at exit. The first covers the title screen and the save's
        # load, so the run is the sum of the others; the exit line can be missing when the compositor goes first.
        # frametime: 60 s: 3538 frames in 60.0 s, mean 59.0 fps, 1% low 44.9 fps, over 25 ms 15, 50 ms 3, 100 ms 1
        frames=""
        read -r frames secs mean low over50 < <(grep -a -E "frametime: (60 s|exit):" "$log" | sed -E \
            's/.*: ([0-9]+) frames in ([0-9.]+) s, mean [0-9.]+ fps, 1% low ([0-9.]+) fps, over 25 ms [0-9]+, 50 ms ([0-9]+).*/\1 \2 \3 \4/' |
            awk 'NR > 1 && $2 >= 10 {f += $1; t += $2; l += $3 * $2; o += $4}
                 END {if (t) printf "%d %.1f %.1f %.1f %d\n", f, t, f / t, l / t, o}') || true
        if [[ -z ${frames:-} ]]; then
            echo "bench: $name run $r has no frametime lines past the load, see $log" >&2
            echo "$name,$r,$run_exe,,,,,," >> "$out"; continue
        fi
        watts=$(awk 'NR > 60 {s += $1; n++} END {if (n) printf "%.1f", s / n / 1e6}' "$samples")   # past the load
        echo "bench:   $frames frames in $secs s, mean $mean fps, 1% low $low, over 50 ms $over50${watts:+, $watts W}"
        echo "$name,$r,$run_exe,$frames,$secs,$mean,$low,$over50,$watts" >> "$out"
    done
done
restore

echo
echo "config        runs  frames(mean)  fps(mean)  1%low(mean)  W(mean)  fps/W"
awk -F, 'NR > 1 && $4 != "" {n[$1]++; f[$1] += $4; m[$1] += $6; l[$1] += $7; w[$1] += $9; if (!($1 in o)) o[$1] = ++k}
    END {for (c in o) ord[o[c]] = c
         for (i = 1; i <= k; i++) {c = ord[i]; W = w[c] / n[c]
             printf "%-12s %5d %13.0f %10.1f %12.1f %8.1f %6s\n", c, n[c], f[c] / n[c], m[c] / n[c], l[c] / n[c], W,
                    (W > 0 ? sprintf("%.2f", m[c] / n[c] / W) : "-")}}' "$out"
echo "bench: results in $out, logs in $logdir"
