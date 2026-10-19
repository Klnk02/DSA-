#!/usr/bin/env python3
"""Exclusive self-symbol buckets for measure_resume_baseline.sh (stdlib only).

Sample share is NOT exact operation timing, blocking time, or a proven DSA share.
Unknown symbols and unmatched rows never silently become zero-copy evidence.
"""
import argparse
import collections
import csv
import json
from pathlib import Path
import random
import re
import statistics

CATEGORIES = [
    "copy_to_iter", "copy_from_iter", "copy_user_boundary", "copy_memory",
    "zero_fill", "page_fault", "page_table", "page_cache",
    "memcg_lru", "filesystem", "block_io", "virtualization", "lock_rcu", "scheduler",
    "other_kernel", "user_runtime", "other_user", "unresolved",
]
# These are self-instruction symbol families, not inclusive call stacks.
# First matching rule wins; keep rules auditable in symbols_aggregated.csv.
RULES = [
    ("copy_to_iter", r"(?:^|_)copy_(?:page_)?to_iter"),
    ("copy_from_iter", r"(?:^|_)copy_(?:page_)?from_iter"),
    ("copy_user_boundary", r"(?:^|_)(?:copy_(?:user|to_user|from_user)|raw_copy|copyin|copyout)"),
    ("copy_memory", r"(?:^|[_.])(?:memcpy|memmove|copy_page)|^rep_movs"),
    ("zero_fill", r"memset|clear_page|clear_user|zero_(?:page|user)|^rep_stos"),
    ("page_fault", r"(?:^|_)(?:handle_mm_fault|do_(?:user_addr|anonymous|wp|swap|huge_pmd|fault)|filemap_fault|wp_page_copy|hugetlb_fault)|^exc_page_fault$"),
    ("page_table", r"follow_page|(?:^|_)(?:pte|pmd|pud|pgd)_|(?:^|_)alloc_pages|free_(?:one|unref)_page|(?:^|_)get_user_pages|(?:^|_)pin_user_pages|walk_page|zap_|tlb_|rmap|map_pages"),
    ("page_cache", r"filemap_|folio_|folios_|xas_|xa_|page_cache|read_cache|readahead"),
    ("memcg_lru", r"memcg|mem_cgroup|try_charge|page_counter|lru_|lruvec|mod_node_page_state|mod_memcg|charge_memcg"),
    ("filesystem", r"^(?:ext4|jbd2|xfs|btrfs|overlay|ovl|fuse|vfs|iomap|generic_file|file_write|file_read|shmem)_"),
    ("block_io", r"^(?:blk|bio|submit_bio|io_uring|io_wq|uring|nvme|scsi|ublk|loop|dm)_|^io_(?:submit|issue|queue)|^submit_bio$"),
    ("virtualization", r"^(?:kvm|vcpu|vmx|svm|tdp|ept|handle_ept)_"),
    ("lock_rcu", r"spin_lock|spin_unlock|mutex_|rwsem_|read_lock|read_unlock|write_lock|write_unlock|rcu_|futex|queued_spin|osq_lock"),
    ("scheduler", r"^sched|^schedule|^__schedule|^pick_next|^enqueue_|^dequeue_|^try_to_wake|^finish_task_switch|^context_switch|^update_(?:curr|load)|^cpu_idle"),
]
COMPILED = [(cat, re.compile(pattern)) for cat, pattern in RULES]
ROW = re.compile(r"^\s*[0-9.]+%\s+(\d+)\s+(\S+)\s+(.+?)\s*$")


def category(dso, symbol):
    if "[unknown]" in (dso, symbol) or re.fullmatch(r"(?:0x)?[0-9a-fA-F]{8,}", symbol):
        return "unresolved"
    for cat, regex in COMPILED:
        if regex.search(symbol):
            return cat
    if "kernel" in dso or symbol.startswith("__x64_sys_"):
        return "other_kernel"
    if symbol.startswith(("runtime.", "syscall.", "internal/")):
        return "user_runtime"
    return "other_user"


def parse_report(path):
    counts = collections.Counter()
    symbols = collections.Counter()
    unparsed = []
    for line in path.read_text(errors="replace").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = ROW.match(line)
        if not match:
            if re.match(r"^\s*[0-9.]+%", line):
                unparsed.append(line)
            continue
        n, dso, symbol = match.groups()
        symbol = re.sub(r"^\[.\]\s+", "", symbol)
        cat = category(dso, symbol)
        counts[cat] += int(n)
        symbols[(cat, dso, symbol)] += int(n)
    return counts, symbols, unparsed


def interval(values):
    values = sorted(values)
    return [values[int((len(values)-1)*.025)], values[int((len(values)-1)*.975)]]


def write_csv(path, headers, rows):
    with path.open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(headers)
        writer.writerows(rows)


def analyze(root, min_samples=20000, min_events=20, max_unresolved=.05):
    with (root / "events.csv").open(encoding="utf-8-sig", newline="") as f:
        events = list(csv.DictReader(f))
    grouped = collections.defaultdict(list)
    symbol_rows, event_rows = [], []
    for event in events:
        if event["phase"] != "record" or event["valid"] != "1":
            continue
        dirs = list(root.glob(f'{int(event["event"]):04d}_record_*'))
        if len(dirs) != 1 or not (dirs[0] / "symbols.txt").exists():
            continue
        counts, symbols, unparsed = parse_report(dirs[0] / "symbols.txt")
        total = sum(counts.values())
        log = (dirs[0] / "perf.stderr").read_text(errors="replace")
        # perf can exit zero despite losing samples. Treat overload/loss diagnostics as a quality failure.
        loss = bool(re.search(r"(?:lost\s+[1-9][0-9]*|[1-9][0-9]*\s+(?:chunks?\s+)?lost|check io/cpu overload)", log, re.I))
        quality = not unparsed and total > 0 and not loss
        key = (event["window"], event["scope"], event["dirty_mib"], event["pids"])
        grouped[key].append((event, counts, total, quality))
        for cat in CATEGORIES:
            event_rows.append([event["event"], *key[:3], cat, counts[cat], total,
                               100*counts[cat]/total if total else "", int(quality)])
        for (cat, dso, symbol), n in symbols.items():
            symbol_rows.append([event["event"], *key, cat, dso, symbol, n])
        (dirs[0] / "parse_quality.json").write_text(json.dumps({
            "parsed_samples": total, "unparsed_rows": unparsed,
            "loss_warning": loss, "parse_quality_ok": quality,
        }, indent=2))
    composition, inputs = [], []
    lines = ["# Resume CPU 符号构成", "",
             "基于cpu-clock的self符号采样；类别互斥、长尾保留，非精确操作计时或阻塞时间。",
             "CPU与DSA收益不可直接等同；共享server内部其他请求仍可能混入。",
             "系统口径只作诊断，不进入推演。CI按事件重采样，未覆盖分类错误或沙箱长期相关性。", ""]
    rng = random.Random(20261019)
    for key, group in sorted(grouped.items()):
        win, scope, dirty, pids = key
        pooled = collections.Counter()
        for _, counts, _, _ in group:
            pooled.update(counts)
        total = sum(pooled.values())
        kernel_named = 0
        # Symbol-level kernel parse quality, separate from functional classification.
        for event, _, _, _ in group:
            dirs = list(root.glob(f'{int(event["event"]):04d}_record_*'))
            _, syms, _ = parse_report(dirs[0] / "symbols.txt")
            kernel_named += sum(n for (cat, dso, _), n in syms.items() if "kernel" in dso and cat != "unresolved")
        unresolved = pooled["unresolved"]/total if total else 1
        reasons = []
        if scope != "process": reasons.append("system_scope")
        if len(group) < min_events: reasons.append("too_few_events")
        if total < min_samples: reasons.append("too_few_samples")
        if unresolved > max_unresolved: reasons.append("unresolved_symbols")
        if kernel_named == 0: reasons.append("no_named_kernel_samples")
        if not all(g[3] for g in group): reasons.append("report_parse_or_loss")
        ref = [r for r in events if r["phase"]=="stat" and r["valid"]=="1"
               and (r["window"],r["scope"],r["dirty_mib"],r["pids"]) == key]
        if len(ref) < min_events: reasons.append("too_few_matched_stat_events")
        lines += [f"## {win} / {scope} / {dirty} MiB", "",
                  f"PID集合：{pids}；事件{len(group)}，样本{total}；采样质量门禁：{'通过（推演仍需确认无并发请求）' if not reasons else ', '.join(reasons)}。", "",
                  "| 符号类别 | 样本数 | 合并占比 | 事件均值 | 事件重采样95%区间 |", "|---|---:|---:|---:|---:|"]
        cat_inputs = {}
        boot = [rng.choices(range(len(group)), k=len(group)) for _ in range(2000)] if len(group)>1 else []
        for cat in CATEGORIES:
            fraction = pooled[cat]/total if total else 0
            vals = [counts[cat]/n for _, counts, n, _ in group if n]
            ci = interval([sum(group[i][1][cat] for i in ix)/sum(group[i][2] for i in ix)
                           for ix in boot if sum(group[i][2] for i in ix)]) if boot and total else [fraction, fraction]
            mean = statistics.mean(vals) if vals else 0
            composition.append([win,scope,dirty,pids,cat,pooled[cat],total,100*fraction,
                                100*mean,100*ci[0],100*ci[1],len(group),int(not reasons)])
            lines.append(f"| {cat} | {pooled[cat]} | {100*fraction:.2f}% | {100*mean:.2f}% | {100*ci[0]:.2f}～{100*ci[1]:.2f}% |")
            cat_inputs[cat] = {"sample_share": fraction, "cluster95": ci}
        inputs.append({"window":win,"scope":scope,"dirty_mib":int(dirty),"pids":pids,
                       "eligible_for_conditional_inference":not reasons,"gate_failures":reasons,
                       "manual_gate":"confirm PID ownership and no concurrent work in shared server/ublk; boundary checks cannot prove absence",
                       "baseline_mean_cpu_ms":statistics.mean(float(r["taskclk_ms"]) for r in ref) if ref else None,
                       "profiled_mean_cpu_ms":statistics.mean(float(r["taskclk_ms"]) for r,_,_,_ in group),
                       "categories":cat_inputs,
                       "copy_total_sample_share":sum(v["sample_share"] for k,v in cat_inputs.items() if k.startswith("copy_")),
                       "copy_warning":"to_iter/from_iter labels follow symbol names, not universally read/write direction; copy_total is NOT automatically DSA-replaceable",
                       "use":"category share times SAME window/dirty/PID mean CPU is a conditional allocation, not measured category time; do not multiply by dirty-regression slope"})
        lines.append("")
    write_csv(root/"composition.csv", ["window","scope","dirty_mib","pids","category","samples","total_samples","pooled_pct","event_mean_pct","cluster95_lo_pct","cluster95_hi_pct","events","quality_gate_ok"], composition)
    write_csv(root/"composition_events.csv", ["event","window","scope","dirty_mib","category","samples","total_samples","pct","parse_quality_ok"], event_rows)
    write_csv(root/"symbols_per_event.csv", ["event","window","scope","dirty_mib","pids","category","dso","symbol","samples"], symbol_rows)
    aggregate = collections.Counter()
    for _, window, scope, dirty, pids, cat, dso, symbol, n in symbol_rows:
        aggregate[(window,scope,dirty,pids,cat,dso,symbol)] += n
    write_csv(root/"symbols_aggregated.csv", ["window","scope","dirty_mib","pids","category","dso","symbol","samples"],
              [[*key,n] for key,n in sorted(aggregate.items(), key=lambda item: -item[1])])
    (root/"inference_inputs.json").write_text(json.dumps(inputs, ensure_ascii=False, indent=2), encoding="utf-8")
    if not grouped:
        lines.append("无有效符号事件；不得解释为copy占比为零。")
    (root/"composition.md").write_text("\n".join(lines)+"\n", encoding="utf-8")
    print("Composition: " + str(root/"composition.md"))
    return inputs


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--min-samples", type=int, default=20000)
    parser.add_argument("--min-events", type=int, default=20)
    args = parser.parse_args()
    analyze(args.run_dir, args.min_samples, args.min_events)
