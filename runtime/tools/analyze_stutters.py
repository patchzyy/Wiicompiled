"""Analyze MKW_FRAME_TRACE CSV files. No third-party packages required."""
import argparse
import bisect
import collections
import csv
import json
import math
import io
import re
import statistics
from pathlib import Path

NS_MS = 1_000_000
NATIVE_FPS = 60
COLUMNS = ("event", "timestamp_ns", "frame_id", "slot", "duration_ns", "deadline_ns",
           "interval_ns", "detail")
INTEGER_COLUMNS = COLUMNS[1:]
SIGNED_COLUMNS = {"slot"}
DROP_SIGNALS = {"drop_late": "late_slot_dropped", "drop_display": "display_slot_dropped",
                "present_unavailable": "present_unavailable", "present_failed": "present_failed"}
ANCHORS = ("auto", "first_row", "first_real_present", "first_transform", "benchmark_ready")
DISPLAY_LIMITED_INVALID_FRACTION = .01


class TraceFormatError(ValueError):
    pass


def percentile(values, fraction):
    if not values:
        return 0.0
    values = sorted(values)
    return values[max(0, math.ceil(len(values) * fraction) - 1)]


def distribution(values):
    return {"count": len(values), "median_ms": percentile(values, .5),
            "p95_ms": percentile(values, .95), "p99_ms": percentile(values, .99),
            "max_ms": max(values, default=0)}


def parse_trace(path):
    """Returns (rows, lost_samples, truncated_tail); rows exclude trace_lost markers."""
    path = Path(path)
    try:
        content = path.read_bytes().decode("utf-8-sig")
    except UnicodeDecodeError as error:
        raise TraceFormatError(f"{path}: not UTF-8 text ({error})") from None
    truncated_tail = bool(content) and not content.endswith("\n")
    if truncated_tail:
        content = content[:content.rfind("\n") + 1]
    reader = csv.reader(io.StringIO(content, newline=""))
    try:
        return _parse_rows(path, reader, truncated_tail)
    except csv.Error as error:
        raise TraceFormatError(f"{path}: line {reader.line_num}: {error}") from None


def _parse_rows(path, reader, truncated_tail):
    header = next(reader, None)
    if header is None:
        raise TraceFormatError(f"{path}: no complete header line")
    duplicate = sorted({name for name in header if header.count(name) > 1})
    missing = [name for name in COLUMNS if name not in header]
    foreign = sorted({name for name in header if name not in COLUMNS})
    if duplicate or missing or foreign:
        problems = [f"{label} {names}" for label, names in
                    (("duplicate columns", duplicate), ("missing columns", missing),
                     ("unexpected columns", foreign)) if names]
        raise TraceFormatError(f"{path}: line 1: not a frame trace header: {'; '.join(problems)}")
    index = {name: header.index(name) for name in COLUMNS}
    rows, lost = [], 0
    for fields in reader:
        line = reader.line_num
        if len(fields) != len(header):
            raise TraceFormatError(f"{path}: line {line}: expected {len(header)} fields, got {len(fields)}")
        row = {"event": fields[index["event"]], "line": line}
        if not row["event"]:
            raise TraceFormatError(f"{path}: line {line}: empty event name")
        for name in INTEGER_COLUMNS:
            text = fields[index[name]]
            if not re.fullmatch(r"-?[0-9]{1,20}", text):
                raise TraceFormatError(f"{path}: line {line}: column '{name}' is not an integer: {text!r}")
            row[name] = int(text)
            if row[name] < 0 and name not in SIGNED_COLUMNS:
                raise TraceFormatError(f"{path}: line {line}: column '{name}' must not be negative: {text}")
        if row["event"] == "trace_lost":
            lost += row["detail"]
        else:
            rows.append(row)
    return rows, lost, truncated_tail


def trace_state(path):
    """Cheap readiness probe for a trace still being written; None until the header is complete."""
    if b"\n" not in Path(path).read_bytes():
        return None
    rows = parse_trace(path)[0]
    state = {"benchmark_ready_ns": None, "real_present_ns": None, "transform_ns": None,
             "display_limited": False}
    for row in rows:
        if row["event"] == "benchmark_ready" and state["benchmark_ready_ns"] is None:
            state["benchmark_ready_ns"] = row["timestamp_ns"]
        elif row["event"] == "present_real" and state["real_present_ns"] is None:
            state["real_present_ns"] = row["timestamp_ns"]
        elif row["event"] == "interp_uniforms" and row["detail"] and state["transform_ns"] is None:
            state["transform_ns"] = row["timestamp_ns"]
        elif row["event"] == "display_limited":
            state["display_limited"] = True
    return state


def native_period_ms(rows):
    """Scheduled native VI period from the seal events; 60 Hz only when the trace has none."""
    intervals = [row["interval_ns"] for row in rows if row["event"] == "seal" and row["interval_ns"]]
    return statistics.median(intervals) / NS_MS if intervals else 1000 / NATIVE_FPS


def timing_anchor(rows, requested):
    if requested == "auto":
        requested = "benchmark_ready"
    matches = {"first_row": lambda row: True,
               "benchmark_ready": lambda row: row["event"] == "benchmark_ready",
               "first_real_present": lambda row: row["event"] == "present_real",
               "first_transform": lambda row: row["event"] == "interp_uniforms" and row["detail"] > 0}
    for used in dict.fromkeys((requested, "first_real_present", "first_row")):
        row = next((row for row in rows if matches[used](row)), None)
        if row:
            return {"requested": requested, "used": used, "start_ns": row["timestamp_ns"],
                    "found": used == requested}
    raise AssertionError("first_row always matches")


def analyze(path, warmup=15, fps=60, anchor="auto"):
    if not math.isfinite(warmup) or warmup < 0:
        raise ValueError(f"warmup must be a finite number of seconds >= 0, got {warmup}")
    if isinstance(fps, bool) or not isinstance(fps, int) or fps <= 0:
        raise ValueError(f"fps must be a positive integer, got {fps!r}")
    if anchor not in ANCHORS:
        raise ValueError(f"anchor must be one of {ANCHORS}, got {anchor!r}")
    rows, lost, truncated_tail = parse_trace(path)
    if not rows:
        raise TraceFormatError(f"{path}: no timing samples ({lost} trace_lost samples, no events)")
    rows.sort(key=lambda row: row["timestamp_ns"])
    start = rows[0]["timestamp_ns"]
    timing = timing_anchor(rows, anchor)
    native_ms = native_period_ms(rows)
    cutoff = timing["start_ns"] + warmup * 1_000_000_000
    measured = [row for row in rows if row["timestamp_ns"] >= cutoff]
    frames = collections.defaultdict(list)
    for row in rows:
        frames[row["frame_id"]].append(row)
    counts = collections.Counter(row["event"] for row in measured)
    stages = collections.defaultdict(list)
    for row in measured:
        if row["duration_ns"]:
            stages[row["event"]].append(row["duration_ns"] / NS_MS)
    drops = [row for row in measured if row["event"] in DROP_SIGNALS]
    drop_times = [row["timestamp_ns"] for row in drops]
    limited_ms = {}
    for row in rows:
        if row["event"] == "display_limited" and row["detail"]:
            limited_ms[row["frame_id"]] = max(limited_ms.get(row["frame_id"], 0),
                                              row["interval_ns"] / NS_MS or native_ms)

    def evidence(frame_ids, window, attributed):
        timings = collections.defaultdict(float)
        pending = 0
        wake_late = 0
        signals = []
        window_drops = drops[bisect.bisect_left(drop_times, window[0]):bisect.bisect_right(drop_times, window[1])]
        for row in window_drops:
            attributed.add(id(row))
            signals.append(DROP_SIGNALS[row["event"]])
        for frame_id in frame_ids:
            for row in frames[frame_id]:
                timings[row["event"]] = max(timings[row["event"]], row["duration_ns"] / NS_MS)
                if row["event"] == "pipelines":
                    pending = max(pending, row["detail"])
                if row["event"] == "pacer_wait" and row["duration_ns"] > NS_MS and row["deadline_ns"]:
                    wake_late = max(wake_late, (row["timestamp_ns"] - row["deadline_ns"]) / NS_MS)
        for stage, threshold, label in [
            ("surface_wait", 2, "surface_or_driver_wait"),
            ("seal", 2, "interpolation_seal"),
            ("pipeline_wait", 1, "pipeline_compilation_wait"),
            ("worker_queue", 2, "render_worker_queue_wait"),
            ("resource_wait", 2, "frame_resources_wait"),
            ("replay_encode", 2, "interpolation_replay_encode"),
            ("present_work", 2, "submit_or_present_cpu_work"),
            ("produce", native_ms * 1.25, "producer_wall_time"),
            ("game_busy", native_ms, "game_thread_busy_over_budget"),
        ]:
            if timings[stage] > threshold:
                signals.append(label)
        if pending:
            signals.append("pipelines_pending")
        if wake_late > 1:
            signals.append("pacer_woke_late")
        for frame_id in frame_ids:
            for row in frames[frame_id]:
                if row["event"] in DROP_SIGNALS:
                    attributed.add(id(row))
                    signals.append(DROP_SIGNALS[row["event"]])
                if row["event"] == "display_limited":
                    signals.append("display_refresh_limited")
                if row["event"] == "interp_health" and fps > NATIVE_FPS:
                    health = row["detail"]
                    if not health & (1 << 18):
                        signals.append("producer_missed_retrace")
                    if not health & (1 << 17):
                        signals.append("interpolation_replay_unsafe")
                    if not health & (1 << 16):
                        signals.append("interpolation_ineligible")
                if row["event"] == "interp_matches" and fps > NATIVE_FPS:
                    matchable, matches = row["detail"] >> 32, row["detail"] & 0xffffffff
                    if matchable and matches * 100 < matchable * 55:
                        signals.append("low_transform_match_coverage")
        signals = sorted(set(signals))
        return {"signals": signals or ["unattributed"], "stage_ms": dict(timings),
                "queued_pipelines": pending, "pacer_wake_late_ms": wake_late}

    def cadence(real_only):
        presents = [row for row in rows if row["event"] in (
            ("present_real",) if real_only else ("present_real", "present_interp"))]
        budget = native_ms if real_only or fps <= NATIVE_FPS else min(native_ms, 1000 / fps)
        tolerance = max(1, budget * .25)
        intervals, spikes, effective_spikes, lateness = [], [], [], []
        transitions = collections.defaultdict(list)
        attributed = set()
        limited_gaps = 0
        effective_budget = budget
        for previous, row in zip(presents, presents[1:]):
            if previous["timestamp_ns"] < cutoff:
                continue
            gap = (row["timestamp_ns"] - previous["timestamp_ns"]) / NS_MS
            intervals.append(gap)
            transitions[previous["event"] + "->" + row["event"]].append(gap)
            if row["deadline_ns"]:
                lateness.append(max(0, row["timestamp_ns"] - row["deadline_ns"]) / NS_MS)
            limit = max(limited_ms.get(previous["frame_id"], 0), limited_ms.get(row["frame_id"], 0))
            expected = max(budget, limit)
            limited_gaps += expected > budget
            effective_budget = max(effective_budget, expected)
            if gap > budget + tolerance:
                nearby = set()
                spike = {"seconds": (row["timestamp_ns"] - start) / 1e9,
                         "frame_id": row["frame_id"], "kind": row["event"],
                         "gap_ms": gap, "excess_ms": gap - budget,
                         **evidence({previous["frame_id"], row["frame_id"]},
                                    (previous["timestamp_ns"], row["timestamp_ns"]), nearby)}
                spikes.append(spike)
                if gap > expected + max(1, expected * .25):
                    effective_spikes.append({**spike, "excess_ms": gap - expected})
                    attributed |= nearby
        worst = lambda found: sorted(found, key=lambda item: item["gap_ms"], reverse=True)[:20]
        counted = lambda found: dict(collections.Counter(
            signal for spike in found for signal in spike["signals"]))
        return {**distribution(intervals), "budget_ms": budget, "tolerance_ms": tolerance,
                "stutters": len(spikes), "signals": counted(spikes),
                "effective_budget_ms": effective_budget, "display_limited_gaps": limited_gaps,
                "effective_stutters": len(effective_spikes),
                "effective_signals": counted(effective_spikes),
                "transitions": {key: distribution(values) for key, values in transitions.items()},
                "deadline_lateness": distribution(lateness),
                "worst": worst(spikes), "effective_worst": worst(effective_spikes)}, attributed

    real, _ = cadence(True)
    all_presentations, attributed = cadence(False)
    isolated = [row for row in drops if id(row) not in attributed]
    match_ratios = [100 * (row["detail"] & 0xffffffff) / (row["detail"] >> 32)
                    for row in measured if row["event"] == "interp_matches" and row["detail"] >> 32]
    real_presents = counts["present_real"]
    limited_frames = len({row["frame_id"] for row in measured
                          if row["event"] == "display_limited" and row["detail"]})
    limited_fraction = limited_frames / real_presents if real_presents else 0.0
    if fps <= NATIVE_FPS:
        status = "not_requested"
    elif not real_presents:
        status = "no_real_presents"
    elif limited_fraction >= DISPLAY_LIMITED_INVALID_FRACTION:
        status = "display_limited"
    elif not any(row["event"] == "interp_uniforms" and row["detail"] > 0 for row in measured):
        status = "no_transform_work"
    elif not counts["present_interp"]:
        status = "no_interpolated_presents"
    else:
        status = "ok"
    return {"trace": str(path), "warmup_seconds": warmup, "target_fps": fps,
            "timing_anchor": timing, "native_period_ms": native_ms,
            "native_period_source": "seal" if any(row["event"] == "seal" and row["interval_ns"]
                                                  for row in rows) else "default_60hz",
            "interpolation": {"status": status, "target_valid": status in ("ok", "not_requested"),
                              "display_limited": limited_fraction >= DISPLAY_LIMITED_INVALID_FRACTION,
                              "display_limited_frames": limited_frames,
                              "display_limited_fraction": limited_fraction,
                              "interpolated_presents": counts["present_interp"]},
            "measured_seconds": max(0, (rows[-1]["timestamp_ns"] - cutoff) / 1e9),
            "lost_trace_samples": lost, "truncated_final_row": truncated_tail, "events": dict(counts),
            "isolated_drops": len(isolated),
            "isolated_drop_examples": [{"seconds": (row["timestamp_ns"] - start) / 1e9, "event": row["event"],
                                        "frame_id": row["frame_id"], "slot": row["slot"]}
                                       for row in isolated[:5]],
            "unassociated_game_timing_samples": sum(row["event"] == "game_busy" and row["frame_id"] == 0
                                                     for row in measured),
            "real": real, "all_presentations": all_presentations,
            "stages": {key: distribution(values) for key, values in stages.items()},
            "transform_matches": {"frames": len(match_ratios),
                                  "low_match_frames": sum(value < 55 for value in match_ratios),
                                  "median_percent": percentile(match_ratios, .5),
                                  "p05_percent": percentile(match_ratios, .05)},
            "limitations": ["Present timestamps measure CPU surface submission, not physical scanout or GPU execution.",
                            "Signals correlate slow stages with gaps; they do not prove causation.",
                            "Producer wall time includes guest waits. Hidden-window results can differ from visible VSync.",
                            "A forced stop can discard the final 100 ms of trace samples.",
                            "stutters/signals use the requested fps budget; effective_* also accept deliberate "
                            "display-limited suppression. Check interpolation.target_valid before comparing targets."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--anchor", choices=ANCHORS, default="auto",
                        help="warmup start: auto uses the renderer's benchmark_ready event (first CPU racer "
                             "initialized, independent of --fps), else the first real present")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = analyze(args.trace, args.warmup, args.fps, args.anchor)
    except (OSError, ValueError) as error:
        raise SystemExit(f"error: {error}")
    if args.output:
        args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    for key in ("real", "all_presentations"):
        entry = result[key]
        print(f"{key}: {entry['count']} intervals, p99 {entry['p99_ms']:.3f} ms, "
              f"max {entry['max_ms']:.3f} ms, {entry['stutters']} requested / "
              f"{entry['effective_stutters']} effective stutters; {entry['effective_signals']}")
    interpolation = result["interpolation"]
    print(f"interpolation: {interpolation['status']}, anchor {result['timing_anchor']['used']}")
    print(f"late slots: {result['events'].get('drop_late', 0)}, "
          f"display drops: {result['events'].get('drop_display', 0)}, "
          f"isolated drops: {result['isolated_drops']}, trace loss: {result['lost_trace_samples']}")


if __name__ == "__main__":
    main()
