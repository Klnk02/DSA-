#!/usr/bin/env bash
# Integration checks with exported command doubles; no AgentENV/perf access.
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'echo "Test artifacts: $test_dir"' EXIT
export TEST_CALLS="$test_dir/calls"
aenv() {
    printf '%s\n' "$*" >>"$TEST_CALLS"
    if [[ ${MOCK_FAIL:-} == resume && $1 == resume ]]; then echo 'resume failed' >&2; return 7; fi
    if [[ ${MOCK_FAIL:-} == dirty && $1 == exec && $* == *if=/dev/urandom* ]]; then return 8; fi
    if [[ ${MOCK_FAIL:-} == http && $1 == resume ]]; then echo 'HTTP 500' >&2; return 0; fi
    return 0
}
pgrep() { return 1; }
perf() {
    if [[ $1 == --version ]]; then echo 'perf mock'; return 0; fi
    if [[ $1 == report ]]; then printf ' 60.00%% 60 [kernel.kallsyms] [k] _copy_to_iter\n 40.00%% 40 [kernel.kallsyms] [k] filemap_get_pages\n'; return 0; fi
    local mode=$1 out= rc=0; shift
    while (( $# )); do
        case $1 in
            -o) out=$2; shift 2 ;;
            --) shift; break ;;
            *) shift ;;
        esac
    done
    "$@" || rc=$?
    if [[ $mode == stat ]]; then
        if [[ $out == */background.stat ]]; then
            if [[ ${MOCK_FAIL:-} == background ]]; then printf '100000.0,msec,task-clock\n' >"$out"
            else printf '0.0,msec,task-clock\n' >"$out"; fi
        elif [[ ${MOCK_FAIL:-} == counter ]]; then printf '<not counted>,msec,task-clock\n' >"$out"
        else printf '12.500,msec,task-clock\n' >"$out"; fi
    else : >"$out"; fi
    return "$rc"
}
export -f aenv perf pgrep
export SBX=mock-sandbox SETTLE_SEC=0 SCOPE=system BG_SEC=0.01 REPEAT_A=2 REPEAT_B=1 DIRTY_MB_LIST='0 64'
export WINDOWS='return touch' DIRTY_B=64
OUT_DIR="$test_dir/success" bash measure_resume_baseline.sh >"$test_dir/success.log" 2>&1
python3 - "$test_dir/success" <<'PY'
import csv,pathlib,sys
p=pathlib.Path(sys.argv[1]); rows=list(csv.DictReader((p/'events.csv').open()))
assert len(rows)==10 and all(r['valid']=='1' for r in rows), rows
assert all(r['scope']=='system' for r in rows), rows
assert sum(r['phase']=='stat' for r in rows)==8
assert sum(r['phase']=='record' for r in rows)==2
assert all(r['taskclk_ms']=='12.500000' for r in rows if r['phase']=='stat')
assert 'NOT full restore' in (p/'summary.txt').read_text()
assert len(list(p.glob('*/symbols.txt')))==2
assert (p/'composition.csv').exists()
import json
inputs=json.loads((p/'inference_inputs.json').read_text())
assert len(inputs)==2 and all('system_scope' in r['gate_failures'] for r in inputs)
PY
# Reusing a run directory must fail, never truncate prior events.
if OUT_DIR="$test_dir/success" bash measure_resume_baseline.sh >"$test_dir/overwrite.log" 2>&1; then echo 'FAIL: overwrote run'; exit 1; fi
for failure in resume dirty http; do
    if MOCK_FAIL=$failure OUT_DIR="$test_dir/$failure" bash measure_resume_baseline.sh >"$test_dir/$failure.log" 2>&1; then
        echo "FAIL: masked $failure failure"; exit 1
    fi
done
MOCK_FAIL=counter REPEAT_A=1 REPEAT_B=0 OUT_DIR="$test_dir/counter" bash measure_resume_baseline.sh >"$test_dir/counter.log" 2>&1
python3 - "$test_dir" <<'PY'
import csv,pathlib,sys
p=pathlib.Path(sys.argv[1])
for case in ('resume','http'):
    rows=list(csv.DictReader((p/case/'events.csv').open()))
    assert len(rows)==1 and rows[0]['valid']=='0', rows
assert list((p/'dirty').glob('*/dirty.rc'))
rows=list(csv.DictReader((p/'counter'/'events.csv').open()))
assert rows and all(r['valid']=='0' and r['reason']=='missing_counter' for r in rows)
PY
# Missing FC/server/ublk in explicit process mode must fail before measuring.
if SCOPE=process OUT_DIR="$test_dir/process" bash measure_resume_baseline.sh >"$test_dir/process.log" 2>&1; then
    echo 'FAIL: accepted incomplete process scope'; exit 1
fi
echo 'PASS: windows/stat-vs-record/diagnostic-scope/status/failures/missing-counter/no-overwrite/coverage/composition'
