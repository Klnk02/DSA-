#!/usr/bin/env bash
# AgentENV resume baseline. Run on the Linux AgentENV host; no source injection.
# A: task-clock vs requested dirty MiB, interleaved sizes, same sandbox.
# B: separate perf record events (never included in A regression).
# Two windows: return = resume CLI only; touch = resume + guest file size check/read.
# touch includes exec/guest work and is NOT all-RAM restore or pure disk2mem cost.
# Default process scope: explicit/unique target roles only. PID churn invalidates
# the event; NEVER fall back to system scope for inference.
# Usage:
#   SBX=<running-id> REPEAT_A=2 REPEAT_B=1 bash measure_resume_baseline.sh
#   TEMPLATE=ubuntu REPEAT_A=20 REPEAT_B=10 bash measure_resume_baseline.sh
#   SCOPE=system ...  # captures newly spawned processes, includes other host work
# Overrides: DIRTY_MB_LIST, DIRTY_B, WINDOWS="return touch", OUT_DIR,
# SERVER_PIDS, UBLK_PIDS, FIRECRACKER_PIDS (otherwise require unique candidates).
# Uses python3 for summaries; outputs are unique run directories, never overwritten.
set -euo pipefail
export LC_ALL=C
cd "$(dirname "$0")"

# perf child helper: keep the CLI exit status separate from perf's exit status.
if [[ ${1:-} == --window ]]; then
    event_dir=$2; window=$3; sandbox=$4; dirty_file=$5; mb=$6
    date +%s%N >"$event_dir/window.start_ns"
    trap 'date +%s%N >"$event_dir/window.end_ns"' EXIT
    rc=0
    aenv resume "$sandbox" >"$event_dir/resume.stdout" 2>"$event_dir/resume.stderr" || rc=$?
    printf '%s\n' "$rc" >"$event_dir/resume.rc"
    (( rc == 0 )) || exit "$rc"
    if [[ $window == touch ]]; then
        rc=0
        aenv exec "$sandbox" -- sh -c \
            'test "$(wc -c < "$1")" -eq "$2" && dd if="$1" of=/dev/null bs=1M' \
            sh "$dirty_file" "$(( mb * 1048576 ))" \
            >"$event_dir/touch.stdout" 2>"$event_dir/touch.stderr" || rc=$?
        printf '%s\n' "$rc" >"$event_dir/touch.rc"
        exit "$rc"
    fi
    exit 0
fi

SBX=${SBX:-}; TEMPLATE=${TEMPLATE:-ubuntu}
REPEAT_A=${REPEAT_A:-20}; REPEAT_B=${REPEAT_B:-60}; DIRTY_B=${DIRTY_B:-256}
DIRTY_MB_LIST=${DIRTY_MB_LIST:-"0 64 256 1024"}
WINDOWS=${WINDOWS:-"return touch"}; SCOPE=${SCOPE:-process}; FREQ=${FREQ:-4000}
BG_SEC=${BG_SEC:-1}; MAX_BACKGROUND_RATIO=${MAX_BACKGROUND_RATIO:-0.10}
SETTLE_SEC=${SETTLE_SEC:-1}; MEM_MIB=${MEM_MIB:-}; CPU_COUNT=${CPU_COUNT:-}; COLD=${COLD:-0}
GUEST_DIRTY=${GUEST_DIRTY:-/dev/shm/aenv_resume_dirty}
for cmd in aenv perf python3 pgrep; do command -v "$cmd" >/dev/null || { echo "ERROR: missing $cmd"; exit 1; }; done
for value in "$REPEAT_A" "$REPEAT_B" "$DIRTY_B" "$FREQ"; do
    [[ $value =~ ^[0-9]+$ ]] || { echo "ERROR: invalid nonnegative integer: $value"; exit 1; }
done
(( REPEAT_A > 0 && FREQ > 0 )) || { echo 'ERROR: REPEAT_A/FREQ must be positive'; exit 1; }
for mb in $DIRTY_MB_LIST; do [[ $mb =~ ^[0-9]+$ ]] || { echo "ERROR: invalid MiB $mb"; exit 1; }; done
for win in $WINDOWS; do [[ $win == return || $win == touch ]] || { echo "ERROR: invalid window $win"; exit 1; }; done
[[ -n $DIRTY_MB_LIST && -n $WINDOWS ]] || { echo 'ERROR: empty sizes/windows'; exit 1; }
[[ $SCOPE == auto || $SCOPE == process || $SCOPE == system ]] || { echo 'ERROR: SCOPE=auto|process|system'; exit 1; }
[[ $SCOPE != auto ]] || SCOPE=process  # compatibility alias; no broad fallback
[[ $BG_SEC =~ ^[0-9]+([.][0-9]+)?$ && $MAX_BACKGROUND_RATIO =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo 'ERROR: invalid background parameters'; exit 1; }
awk -v s="$BG_SEC" -v r="$MAX_BACKGROUND_RATIO" 'BEGIN{exit !(s>0 && r>=0 && r<=1)}' || { echo 'ERROR: BG_SEC>0, background ratio in [0,1]'; exit 1; }
[[ -f analyze_resume_baseline.py ]] || { echo 'ERROR: copy analyze_resume_baseline.py alongside this script'; exit 1; }

OUT_DIR=${OUT_DIR:-"results/stage4-sandbox/resume_baseline/$(date +%Y%m%d_%H%M%S)_$$"}
mkdir -p "$(dirname "$OUT_DIR")"
mkdir "$OUT_DIR"  # fail rather than overwrite an existing run
OUT_DIR=$(cd "$OUT_DIR" && pwd)
SELF=$(pwd)/measure_resume_baseline.sh
trap 'rc=$?; if (( rc != 0 )); then echo "Stopped rc=$rc; logs=$OUT_DIR; sandbox=${SBX:-unknown}. Inspect state before retrying." >&2; fi' EXIT
printf 'event,phase,window,dirty_mib,round,scope,pids,perf_rc,resume_rc,touch_rc,valid,reason,taskclk_ms,wall_ms,operation_wall_ms,idle_cpu_ms,idle_wall_ms,background_ratio\n' >"$OUT_DIR/events.csv"

log_env() {
    date -Is; uname -a; perf --version
    printf 'sandbox=%s scope=%s windows=%s sizes=%s repeat_A=%s repeat_B=%s dirty_B=%s\n' \
        "$SBX" "$SCOPE" "$WINDOWS" "$DIRTY_MB_LIST" "$REPEAT_A" "$REPEAT_B" "$DIRTY_B"
    printf 'background_seconds=%s max_background_ratio=%s sampler=cpu-clock frequency=%s\n' "$BG_SEC" "$MAX_BACKGROUND_RATIO" "$FREQ"
    printf 'loadavg='; cat /proc/loadavg
    printf 'perf_event_paranoid='; cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unavailable
    printf 'kptr_restrict='; cat /proc/sys/kernel/kptr_restrict 2>/dev/null || echo unavailable
    df -h .
}
run_checked() {
    local tag=$1; shift
    local rc=0
    "$@" >"$event_dir/$tag.stdout" 2>"$event_dir/$tag.stderr" || rc=$?
    printf '%s\n' "$rc" >"$event_dir/$tag.rc"
    if (( rc != 0 )) || grep -Eiq 'HTTP[ /]+[45][0-9][0-9]|Traceback|"success"[[:space:]]*:[[:space:]]*false' \
            "$event_dir/$tag.stdout" "$event_dir/$tag.stderr"; then
        echo "ERROR: $tag failed; logs: $event_dir" >&2
        return 1
    fi
}
if [[ -z $SBX ]]; then
    args=(-d)
    if [[ $COLD == 1 ]]; then
        args+=(--cold)
        [[ -z $MEM_MIB ]] || args+=(--memory "$MEM_MIB")
        [[ -z $CPU_COUNT ]] || args+=(--cpu "$CPU_COUNT")
    fi
    event_dir=$OUT_DIR
    run_checked start aenv start "${args[@]}" "$TEMPLATE"
    SBX=$(grep -oE '[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}' "$OUT_DIR/start.stdout" | head -1)
    [[ -n $SBX ]] || { echo "ERROR: no sandbox id; see $OUT_DIR/start.stdout"; exit 1; }
fi
event_dir=$OUT_DIR
run_checked timeout aenv timeout "$SBX" 43200
# The supplied sandbox must be running; do not mask an initial resume error.
run_checked ready aenv exec "$SBX" -- true
log_env >"$OUT_DIR/env_before.txt"
sha256sum "$SELF" >"$OUT_DIR/script.sha256"
sha256sum analyze_resume_baseline.py >>"$OUT_DIR/script.sha256"

discover() {
    # Discovery alone is NOT sandbox ownership proof. Ambiguous roles require overrides.
    local role=$1 override=$2
    if [[ -n $override ]]; then printf '%s\n' "${override//,/ }" | tr ' ' '\n'
    else
        case $role in
            server) pgrep -x server || true ;;
            ublk) pgrep -f '(^|/)uvm-ublk-daemon([[:space:]]|$)' || true ;;
            fc) pgrep -x firecracker || true ;;
        esac
    fi
}
if [[ $SCOPE == process ]]; then
    for role in server ublk fc; do
        case $role in server) name=SERVER_PIDS ;; ublk) name=UBLK_PIDS ;; fc) name=FIRECRACKER_PIDS ;; esac
        override=${!name:-}
        mapfile -t candidates < <(discover "$role" "$override" | sed '/^$/d' | sort -un)
        if (( ${#candidates[@]} == 0 )) || { [[ -z $override ]] && (( ${#candidates[@]} != 1 )); }; then
            printf 'ERROR: role=%s candidates=%s; set %s to confirmed target PID(s), not all same-name processes\n' "$role" "${candidates[*]:-none}" "$name" >&2
            exit 1
        fi
        for p in "${candidates[@]}"; do [[ $p =~ ^[0-9]+$ && -r /proc/$p/stat ]] || { echo "ERROR: unreadable PID $p"; exit 1; }; done
        printf -v "$name" '%s' "$(IFS=,; echo "${candidates[*]}")"
        printf '%s=%s\n' "$name" "${!name}" >>"$OUT_DIR/selected_pids.txt"
    done
fi
read_taskclk() {
    [[ -r $1 ]] || return 0
    awk -F, '$3~/^task-clock/ && $1~/^[0-9.]+$/ {v=$1; if($2=="sec")v*=1000; if($2=="usec")v/=1000; if($2=="nsec")v/=1000000; printf "%.6f",v; exit}' "$1"
}
snapshot_pids() {
    local target=$1 p threads
    : >"$target"
    for p in $( { discover server "${SERVER_PIDS:-}"; discover ublk "${UBLK_PIDS:-}"; discover fc "${FIRECRACKER_PIDS:-}"; } | sort -un); do
        [[ $p =~ ^[0-9]+$ && -r /proc/$p/stat ]] || continue
        # starttime guards PID reuse; tail after final ')' handles spaces in comm.
        threads=$(for statfile in /proc/"$p"/task/*/stat; do
            [[ -r $statfile ]] || continue
            sed 's/.*) //' "$statfile" | awk -v tid="$(basename "$(dirname "$statfile")")" '{print tid, $20}'
        done | sort -n | sha256sum | awk '{print $1}')
        sed 's/.*) //' "/proc/$p/stat" | awk -v p="$p" -v th="$threads" '{print p, $20, th}' >>"$target"
    done
}

event_n=0
one_event() {
    local phase=$1 window=$2 mb=$3 round=$4
    event_n=$((event_n + 1)); event_dir="$OUT_DIR/$(printf '%04d' "$event_n")_${phase}_${window}_${mb}m"
    mkdir "$event_dir"
    local reason=ok valid=1 rc=0 resume_rc=NA touch_rc=NA tc= wall= scope=$SCOPE pids=
    local idle_cpu= idle_wall= bg_ratio= opwall= idle_t0 idle_t1
    # Prepare all guest data outside both measured windows, including zero control.
    run_checked dirty aenv exec "$SBX" -- dd if=/dev/urandom of="$GUEST_DIRTY" bs=1M count="$mb"
    run_checked size aenv exec "$SBX" -- sh -c 'test "$(wc -c < "$1")" -eq "$2"' sh "$GUEST_DIRTY" "$(( mb * 1048576 ))"
    sleep "$SETTLE_SEC"
    snapshot_pids "$event_dir/pids_running.txt"
    run_checked pause aenv pause "$SBX"
    snapshot_pids "$event_dir/pids_paused.txt"
    local has_fc=0 has_server=0 has_ublk=0 p
    for p in $(discover fc "${FIRECRACKER_PIDS:-}"); do
        [[ ! -r /proc/$p/stat ]] || has_fc=1
    done
    for p in $(discover server "${SERVER_PIDS:-}"); do [[ ! -r /proc/$p/stat ]] || has_server=1; done
    for p in $(discover ublk "${UBLK_PIDS:-}"); do [[ ! -r /proc/$p/stat ]] || has_ublk=1; done
    printf 'server=%s ublk=%s fc=%s requested_scope=%s\n' "$has_server" "$has_ublk" "$has_fc" "$SCOPE" >"$event_dir/coverage.txt"
    local attach=(-a)
    if [[ $scope == process ]]; then
        pids=$(awk '{print $1}' "$event_dir/pids_paused.txt" | paste -sd, -)
        if [[ -z $pids ]] || (( ! has_fc || ! has_server || ! has_ublk )) || ! cmp -s "$event_dir/pids_running.txt" "$event_dir/pids_paused.txt"; then
            echo "ERROR: target PID coverage changed/missing before resume; no system fallback. Sandbox is paused: $SBX; logs=$event_dir" >&2
            return 1
        fi
        attach=(-p "$pids" --no-inherit)
    fi
    # Matched target idle control while paused; reject high background, do not subtract it as causal truth.
    idle_t0=$(date +%s%N)
    perf stat "${attach[@]}" -x, -o "$event_dir/background.stat" -e task-clock -- sleep "$BG_SEC" \
        >"$event_dir/background.stdout" 2>"$event_dir/background.stderr" || { echo "ERROR: background perf failed: $event_dir" >&2; return 1; }
    idle_t1=$(date +%s%N); idle_wall=$(( (idle_t1-idle_t0)/1000000 ))
    idle_cpu=$(read_taskclk "$event_dir/background.stat")
    [[ -n $idle_cpu ]] || { echo "ERROR: missing background counter: $event_dir" >&2; return 1; }
    local t0 t1
    t0=$(date +%s%N)
    if [[ $phase == stat ]]; then
        perf stat "${attach[@]}" -x, -o "$event_dir/perf.stat" \
            -e task-clock,page-faults,minor-faults,major-faults \
            -- bash "$SELF" --window "$event_dir" "$window" "$SBX" "$GUEST_DIRTY" "$mb" \
            >"$event_dir/perf.stdout" 2>"$event_dir/perf.stderr" || rc=$?
    else
        perf stat "${attach[@]}" -x, -o "$event_dir/perf.stat" -e task-clock -- \
          perf record "${attach[@]}" -e cpu-clock -F "$FREQ" -o "$event_dir/perf.data" \
            -- bash "$SELF" --window "$event_dir" "$window" "$SBX" "$GUEST_DIRTY" "$mb" \
            >"$event_dir/perf.stdout" 2>"$event_dir/perf.stderr" || rc=$?
    fi
    t1=$(date +%s%N); wall=$(( (t1 - t0) / 1000000 ))
    printf 'start_ns=%s\nend_ns=%s\n' "$t0" "$t1" >"$event_dir/timing.txt"
    if [[ -r $event_dir/window.start_ns && -r $event_dir/window.end_ns ]]; then
        opwall=$(( ( $(cat "$event_dir/window.end_ns") - $(cat "$event_dir/window.start_ns") ) / 1000000 ))
    fi
    [[ ! -f $event_dir/resume.rc ]] || resume_rc=$(cat "$event_dir/resume.rc")
    [[ ! -f $event_dir/touch.rc ]] || touch_rc=$(cat "$event_dir/touch.rc")
    if (( rc != 0 )) || [[ $resume_rc != 0 ]] || [[ $window == touch && $touch_rc != 0 ]]; then valid=0; reason=command_failed; fi
    if grep -Eiq 'HTTP[ /]+[45][0-9][0-9]|Traceback|"success"[[:space:]]*:[[:space:]]*false' \
            "$event_dir"/*.stdout "$event_dir"/*.stderr; then valid=0; reason=reported_failure; fi
    snapshot_pids "$event_dir/pids_after.txt"
    if [[ $scope == process ]] && ! cmp -s "$event_dir/pids_paused.txt" "$event_dir/pids_after.txt"; then
        valid=0; reason=pid_churn
    fi
    tc=$(read_taskclk "$event_dir/perf.stat")
    if [[ -z $tc ]]; then valid=0; reason=missing_counter; fi
    if [[ -n $tc && -n $opwall ]]; then
        bg_ratio=$(awk -v b="$idle_cpu" -v iw="$idle_wall" -v w="$opwall" -v c="$tc" 'BEGIN{if(iw>0 && c>0)printf "%.6f", b/iw*w/c; else print 1}')
        if awk -v b="$bg_ratio" -v m="$MAX_BACKGROUND_RATIO" 'BEGIN{exit !(b>m)}'; then valid=0; reason=background_high; fi
    fi
    if [[ $phase == stat ]]; then
        if [[ -z $tc ]]; then valid=0; reason=missing_counter; fi
    elif (( valid )); then
        # Raw data remains authoritative; unresolved symbols are retained, never called copy=0.
        perf report -i "$event_dir/perf.data" --stdio -n --no-children --percent-limit 0 --sort dso,symbol \
            >"$event_dir/symbols.txt" 2>"$event_dir/report.stderr" || { valid=0; reason=report_failed; }
    fi
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$event_n" "$phase" "$window" "$mb" "$round" "$scope" "${pids//,/;}" "$rc" "$resume_rc" "$touch_rc" "$valid" "$reason" "$tc" "$wall" "$opwall" "$idle_cpu" "$idle_wall" "$bg_ratio" >>"$OUT_DIR/events.csv"
    echo "[$event_n] $phase $window ${mb}MiB scope=$scope valid=$valid reason=$reason CPU=${tc:-NA}ms wall=${wall}ms"
    # Fail fast on operation errors; never try to repair unknown sandbox state silently.
    if (( rc != 0 )) || [[ $resume_rc != 0 ]] || [[ $window == touch && $touch_rc != 0 ]] || [[ $reason == reported_failure ]]; then
        echo "ERROR: event failed; inspect $event_dir; sandbox=$SBX" >&2; return 1
    fi
}

# Per-round rotation balances size/window order without claiming randomization.
read -ra sizes <<<"$DIRTY_MB_LIST"; read -ra wins <<<"$WINDOWS"
for ((round=1; round<=REPEAT_A; round++)); do
    for ((j=0; j<${#sizes[@]}; j++)); do
        mb=${sizes[$(( (j + round - 1) % ${#sizes[@]} ))]}
        for ((k=0; k<${#wins[@]}; k++)); do
            win=${wins[$(( (k + round - 1) % ${#wins[@]} ))]}
            one_event stat "$win" "$mb" "$round"
        done
    done
done
for ((round=1; round<=REPEAT_B; round++)); do
    for win in "${wins[@]}"; do one_event record "$win" "$DIRTY_B" "$round"; done
done
log_env >"$OUT_DIR/env_after.txt"
python3 - "$OUT_DIR" <<'PY'
import csv, pathlib, statistics, sys
root = pathlib.Path(sys.argv[1])
rows = list(csv.DictReader((root / 'events.csv').open()))
out = ['Resume baseline: counters are host CPU; no DSA replacement tested.',
       'return: resume CLI only, NOT full restore. touch: resume + exec + file read, NOT pure disk2mem.',
       'wall_ms includes perf/helper startup/teardown; no direct comparison to old CLI-only wall.',
       'process: confirmed/unique selected roles; no inheritance. Shared requests and transient workers can still affect coverage; review isolation before inference.',
       'system: diagnostic only, ALL host CPU; NOT eligible for inference. No automatic fallback.',
       'Neither requested dirty MiB nor file bytes proves actual physical disk-read volume.',
       f'Events={len(rows)} invalid={sum(r["valid"] != "1" for r in rows)}']
for win, scope in sorted({(r['window'], r['scope']) for r in rows}):
    data = [r for r in rows if r['phase']=='stat' and r['valid']=='1' and r['window']==win and r['scope']==scope]
    out.append(f'\nwindow={win} scope={scope} valid_stat_events={len(data)}')
    for mb in sorted({int(r['dirty_mib']) for r in data}):
        group = [r for r in data if int(r['dirty_mib'])==mb]
        vals = [float(r['taskclk_ms']) for r in group]
        out.append(f'  {mb}MiB n={len(vals)} cpu_mean={statistics.mean(vals):.3f}ms cpu_sd={statistics.stdev(vals) if len(vals)>1 else 0:.3f}ms wall_mean={statistics.mean(float(r["wall_ms"]) for r in group):.3f}ms')
    n = len(data)
    if n < 3: continue
    x = [int(r['dirty_mib'])/1024 for r in data]; y = [float(r['taskclk_ms']) for r in data]
    xm, ym = statistics.mean(x), statistics.mean(y)
    sxx = sum((v-xm)**2 for v in x)
    if not sxx: continue
    b = sum((u-xm)*(v-ym) for u,v in zip(x,y))/sxx; a = ym-b*xm
    sse = sum((v-a-b*u)**2 for u,v in zip(x,y)); sst = sum((v-ym)**2 for v in y)
    ci = 1.96*(sse/(n-2)/sxx)**0.5
    out.append(f'  DESCRIPTIVE OLS fixed={a:.3f}ms slope={b:.3f}ms/GiB approx95CI_halfwidth={ci:.3f} R2={1-sse/sst if sst else 0:.3f}')
    out.append('  OLS interval assumes independent residuals; repeated sandbox/round correlations and host interference are NOT covered. Negative/weak slope is inconclusive, not negative copy cost.')
text = '\n'.join(out)+'\n'
(root/'summary.txt').write_text(text)
print(text)
PY
python3 analyze_resume_baseline.py "$OUT_DIR"
echo "Artifacts: $OUT_DIR ; sandbox left running: $SBX"
