"""Windows hidden-race benchmark with inherited affinity and isolated portable settings.

Every case gets its own portable directory, seeded from one pristine snapshot of the NAND and
(optionally) the cache, so a case never inherits another case's changed cache or saves.
"""
import argparse
import ctypes
from ctypes import wintypes
import itertools
import json
import math
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
import tomllib

from analyze_stutters import analyze, trace_state

SUPPORTED_RESOLUTIONS = (0, 1, 1.5, 2, 3, 4, 6, 8)
GRAPHICS_APIS = ("auto", "d3d12", "vulkan")
FPS_TARGETS = (0, 120, 180)
HELPER_COUNTS = ("auto", *map(str, range(7)))
STARTUP_TIMEOUT = 60


class ConfigError(ValueError):
    pass


def cpu_topology():
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t),
                                              ctypes.POINTER(ctypes.c_size_t)]
    kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    process, system = ctypes.c_size_t(), ctypes.c_size_t()
    handle = kernel.GetCurrentProcess()
    if not kernel.GetProcessAffinityMask(handle, ctypes.byref(process), ctypes.byref(system)):
        raise ctypes.WinError(ctypes.get_last_error())

    # SYSTEM_LOGICAL_PROCESSOR_INFORMATION has an affinity mask, relationship enum,
    # and a 16-byte union. RelationProcessorCore (0) gives each core's SMT mask.
    class ProcessorInfo(ctypes.Structure):
        _fields_ = [("mask", ctypes.c_size_t), ("relationship", ctypes.c_int),
                    ("data", ctypes.c_ulonglong * 2)]
    size = wintypes.DWORD()
    kernel.GetLogicalProcessorInformation(None, ctypes.byref(size))
    buffer = ctypes.create_string_buffer(size.value)
    if not kernel.GetLogicalProcessorInformation(buffer, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    cores = []
    for offset in range(0, size.value, ctypes.sizeof(ProcessorInfo)):
        info = ProcessorInfo.from_buffer(buffer, offset)
        available = info.mask & process.value
        if info.relationship == 0 and available:
            cores.append(available & -available)  # one logical processor per physical core
    return kernel, handle, process.value, cores


def quoted(value):
    # JSON basic strings are valid TOML except for raw DEL.
    return json.dumps(str(value), ensure_ascii=False).replace("\x7f", "\\u007f")


def interpolation_helpers(core_count, requested):
    if requested != "auto":
        return int(requested)
    if not core_count:
        return None
    return min(core_count - 1, 6) if core_count > 2 else 0


def parse_list(text, name, parse, allowed=None):
    """Comma-separated matrix axis; rejects malformed, unsupported and duplicate entries."""
    values = []
    for item in text.split(","):
        try:
            values.append(parse(item.strip()))
        except ValueError:
            raise ValueError(f"--{name}: invalid entry {item!r}") from None
    if allowed is not None and (bad := [value for value in values if value not in allowed]):
        raise ValueError(f"--{name}: unsupported {bad}; allowed {list(allowed)}")
    if len(set(values)) != len(values):
        raise ValueError(f"--{name}: duplicate entries in {text!r}")
    return values


def parse_helper(text):
    return text if text == "auto" else str(int(text))


class SourceConfig:
    def __init__(self, dvd_root, retro_rewind_root, nand_root, graphics_api):
        self.dvd_root, self.retro_rewind_root = dvd_root, retro_rewind_root
        self.nand_root, self.graphics_api = nand_root, graphics_api


def load_source_config(config_path):
    """Reads and validates the user's Config.toml before anything is staged."""
    config_path = Path(config_path)
    try:
        with config_path.open("rb") as source:
            config = tomllib.load(source)
    except OSError as error:
        raise ConfigError(f"cannot read config {config_path}: {error}") from None
    except (tomllib.TOMLDecodeError, UnicodeDecodeError) as error:
        raise ConfigError(f"{config_path}: invalid TOML: {error}") from None

    def table(name):
        value = config.get(name, {})
        if not isinstance(value, dict):
            raise ConfigError(f"{config_path}: [{name}] must be a table, got {type(value).__name__}")
        return value

    paths, video = table("paths"), table("video")
    base = config_path.resolve().parent

    def source_path(key, directory=False):
        value = paths.get(key)
        if value is None or value == "":
            return None
        if not isinstance(value, str):
            raise ConfigError(f"{config_path}: paths.{key} must be a string, got {type(value).__name__}")
        path = Path(value)
        path = path if path.is_absolute() else (base / path).resolve()
        if not (path.is_dir() if directory else path.exists()):
            raise ConfigError(f"{config_path}: paths.{key} {'is not a directory' if directory else 'does not exist'}: {path}")
        return path

    api = video.get("graphics_api", "auto")
    if not isinstance(api, str) or api not in GRAPHICS_APIS:
        raise ConfigError(f"{config_path}: video.graphics_api must be one of {list(GRAPHICS_APIS)}, got {api!r}")
    return SourceConfig(source_path("dvd_root"), source_path("retro_rewind_root"),
                        source_path("nand_root", directory=True), api)


def isolated_config_text(source, nand_root, fps, resolution, vsync):
    lines = ["[paths]"]
    for key in ("dvd_root", "retro_rewind_root"):
        if path := getattr(source, key):
            lines.append(f"{key} = {quoted(path)}")
    lines.append(f"nand_root = {quoted(nand_root)}")
    # No physical controller scans or presence/network traffic during measurements.
    # Keep the audio mixer running; --hidden retains normal audio behavior.
    lines += ["[controller]", "wii_remotes = false", "[network]", "enabled = false",
              "[discord]", "enabled = false", "[audio]", "volume = 0.0",
              "attenuate_music_when_media_plays = false",
              "[video]", "window_width = 854", "window_height = 480", 'display_mode = "windowed"',
              f"graphics_api = {quoted(source.graphics_api)}",
              f"resolution_multiplier = {float(resolution)}", f"frame_interpolation_fps = {fps}",
              f"vsync = {'true' if vsync else 'false'}", "show_fps = false"]
    return "\n".join(lines) + "\n"


def wait_ready(process, trace, label):
    """Waits for the renderer's benchmark_ready marker (first CPU racer initialized)."""
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while True:
        if process.poll() is not None:
            raise RuntimeError(f"{label}: game exited during startup ({process.returncode}); see log")
        state = trace_state(trace) if trace.exists() else None
        if state and state["benchmark_ready_ns"] is not None:
            return state
        if time.monotonic() >= deadline:
            raise RuntimeError(f"{label}: no benchmark_ready trace event within {STARTUP_TIMEOUT}s of "
                               "startup; the race did not start or the executable predates the marker; see log")
        time.sleep(.25)


def stop_process(process, label):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        process.kill()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"{label}: game did not exit after terminate and kill") from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--config", type=Path,
                        default=Path(os.environ.get("LOCALAPPDATA", ".")) / "WiiCompiled/Config.toml")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpus", default="0,4", help="physical cores without SMT; 0 keeps full inherited affinity")
    parser.add_argument("--fps", default="0,120")
    parser.add_argument("--helpers", default="auto", help="auto or comma-separated counts 0..6")
    parser.add_argument("--seconds", type=float, default=60, help="measurement time, excluding warmup")
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--course", type=lambda value: int(value, 0), default=8)
    parser.add_argument("--resolution", type=float, default=1)
    parser.add_argument("--vsync", action="store_true")
    parser.add_argument("--no-shared-cache-affinity", action="store_true",
                        help="keep all threads within the process mask, without additional cache-domain pinning")
    parser.add_argument("--cache-from", type=Path,
                        help="snapshot this Cache directory once and seed every case from the snapshot; "
                             "without it every case starts cold")
    parser.add_argument("--allow-display-limited", action="store_true",
                        help="exit 0 even when a requested interpolation target was suppressed by the display")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("The runner requires Windows; the trace analyzer is cross-platform.")
    try:
        cpus = parse_list(args.cpus, "cpus", int)
        fps_targets = parse_list(args.fps, "fps", int, FPS_TARGETS)
        helpers = parse_list(args.helpers, "helpers", parse_helper, HELPER_COUNTS)
    except ValueError as error:
        parser.error(str(error))
    if not (math.isfinite(args.seconds) and args.seconds > 0 and math.isfinite(args.warmup) and args.warmup >= 0):
        parser.error("seconds must be finite and > 0; warmup must be finite and >= 0")
    if args.repeats < 1 or not 0 <= args.course <= 31:
        parser.error("invalid repeats or course")
    if not math.isfinite(args.resolution) or args.resolution not in SUPPORTED_RESOLUTIONS:
        parser.error(f"resolution must be one of {list(SUPPORTED_RESOLUTIONS)}")
    kernel, handle, original_mask, cores = cpu_topology()
    if any(value < 0 or value > len(cores) for value in cpus):
        parser.error(f"cpus must be between 0 and {len(cores)}")
    try:
        exe = args.exe.resolve(strict=True)
        source = load_source_config(args.config)
    except (OSError, ConfigError) as error:
        parser.error(str(error))
    output = args.output.resolve()
    if source.nand_root and output.is_relative_to(source.nand_root.resolve()):
        parser.error("--output must not be inside the source nand_root; the NAND snapshot would copy itself")
    cache_from = args.cache_from.resolve() if args.cache_from else None
    if cache_from and (not cache_from.is_dir() or output.is_relative_to(cache_from)
                       or cache_from.is_relative_to(output)):
        parser.error("--cache-from must be an existing directory outside the output directory")
    if (output / "portable").exists() or (output / "seeds").exists():
        parser.error("output already contains portable/ or seeds/; use a fresh output directory")

    output.mkdir(parents=True, exist_ok=True)
    seeds = output / "seeds"
    nand_seed, cache_seed = seeds / "NAND", seeds / "Cache"
    if source.nand_root:
        shutil.copytree(source.nand_root, nand_seed, ignore=shutil.ignore_patterns("*.nandsafe.tmp"))
    else:
        nand_seed.mkdir(parents=True)
    if cache_from:
        shutil.copytree(cache_from, cache_seed)
    cache_mode = "seeded" if cache_from else "cold"
    print(f"cache: {cache_mode}" + (f" (every case copies the snapshot of {cache_from})" if cache_from
          else " (every case starts with an empty cache; results include shader/pipeline compilation)"),
          flush=True)
    runtime_files = [exe, *sorted(itertools.chain(exe.parent.glob("*.dll"), exe.parent.glob("*.bin"),
                                                  exe.parent.glob("initial_pipeline_cache.db")))]

    summaries, invalid = [], []
    for repeat, core_count, fps, helper in itertools.product(range(args.repeats), cpus, fps_targets, helpers):
        effective_helpers = interpolation_helpers(core_count, helper)
        label = f"r{repeat}-cpu{core_count}-fps{fps}-helpers{helper}"
        mask = original_mask if core_count == 0 else sum(cores[:core_count])
        trace = output / f"{label}.csv"
        stage = output / "portable" / label
        user = stage / "UserData"
        user.mkdir(parents=True)
        (stage / "portable.txt").write_text("stutter benchmark\n")
        for runtime_file in dict.fromkeys(runtime_files):
            shutil.copy2(runtime_file, stage / runtime_file.name)
        shutil.copytree(nand_seed, user / "NAND")
        if cache_from:
            shutil.copytree(cache_seed, user / "Cache")
        (user / "Config.toml").write_text(
            isolated_config_text(source, user / "NAND", fps, args.resolution, args.vsync), encoding="utf-8")
        print(f"running {label}, affinity=0x{mask:x}, cache={cache_mode}", flush=True)
        env = os.environ.copy()
        env["MKW_FRAME_TRACE"] = str(trace)
        env["MKW_PERF_STATS"] = "1"
        env.pop("MKW_INTERPOLATION_HELPERS", None)
        env.pop("MKW_SHARED_CACHE_AFFINITY", None)
        if args.no_shared_cache_affinity:
            env["MKW_SHARED_CACHE_AFFINITY"] = "0"
        if effective_helpers is not None:
            env["MKW_INTERPOLATION_HELPERS"] = str(effective_helpers)
        with (output / f"{label}.log").open("wb") as log:
            try:
                if not kernel.SetProcessAffinityMask(handle, mask):
                    raise ctypes.WinError(ctypes.get_last_error())
                # The child inherits affinity at creation, before any worker pool sizes itself.
                process = subprocess.Popen([str(stage / exe.name), "--hidden", "--race-cpu",
                                            f"--race={args.course}"], cwd=stage, env=env,
                                           stdout=log, stderr=subprocess.STDOUT,
                                           creationflags=subprocess.CREATE_NO_WINDOW)
            finally:
                if not kernel.SetProcessAffinityMask(handle, original_mask):
                    raise ctypes.WinError(ctypes.get_last_error())
            forced_stop = False
            try:
                # The analyzer anchors warmup at the same event for every fps target, so
                # boot and loading never consume the warmup/measurement interval.
                started = time.monotonic()
                ready = wait_ready(process, trace, label)
                startup_seconds = time.monotonic() - started
                process.wait(timeout=args.warmup + args.seconds + 1)
                raise RuntimeError(f"{label}: game exited early ({process.returncode}); see log")
            except subprocess.TimeoutExpired:
                forced_stop = True
            finally:
                stop_process(process, label)
        result = analyze(trace, args.warmup, fps or 60)
        if result["lost_trace_samples"] or result["truncated_final_row"]:
            raise RuntimeError(f"{label}: incomplete trace; see log")
        log_text = (output / f"{label}.log").read_text(encoding="utf-8", errors="replace")
        affinity_counts = {name: int(count) for name, count in re.findall(
            r"\[aurora::thread\] (.+?): (\d+) available CPUs after affinity", log_text)}
        if result["real"]["count"] == 0:
            raise RuntimeError(f"{label}: no real frame presentations after warmup; see log")
        interpolation = result["interpolation"]
        if fps and interpolation["status"] == "no_transform_work":
            raise RuntimeError(f"{label}: no transform interpolation work after warmup; verify the race loaded")
        if result["measured_seconds"] < args.seconds * .9:
            raise RuntimeError(f"{label}: measured only {result['measured_seconds']:.1f}s of {args.seconds}s; see log")
        if fps and not interpolation["target_valid"]:
            invalid.append(label)
            print(f"  WARNING {label}: requested {fps} fps target is {interpolation['status']} "
                  f"({interpolation['display_limited_fraction']:.0%} of frames display-limited); "
                  "use effective_* statistics and do not compare it with a real 120/180 fps run", flush=True)
        result.update(label=label, affinity_mask=hex(mask), physical_core_limit=core_count,
                      helpers=helper, repeat=repeat, forced_stop=forced_stop,
                      effective_helpers=effective_helpers, host_logical_processors=os.cpu_count(),
                      resolution=args.resolution, vsync=args.vsync, course=args.course,
                      shared_cache_affinity=not args.no_shared_cache_affinity,
                      cache_mode=cache_mode, cache_seed=str(cache_from) if cache_from else None,
                      startup_seconds=startup_seconds, startup_ready=ready,
                      thread_affinity_cpu_counts=affinity_counts,
                      affinity_narrowed=bool(core_count and any(
                          count < core_count for count in affinity_counts.values())),
                      executable=str(exe), available_core_masks=[hex(core) for core in cores])
        (output / f"{label}.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        summaries.append(result)
        (output / "summary.json").write_text(json.dumps(summaries, indent=2), encoding="utf-8")
        for kind in ("real", "all_presentations"):
            entry = result[kind]
            print(f"  {kind}: p99={entry['p99_ms']:.2f} max={entry['max_ms']:.2f} ms "
                  f"stutters={entry['stutters']} effective={entry['effective_stutters']} "
                  f"signals={entry['effective_signals']}", flush=True)
        print(f"  interpolation={interpolation['status']} isolated_drops={result['isolated_drops']}", flush=True)
    if invalid and not args.allow_display_limited:
        raise SystemExit(f"display-limited or invalid interpolation targets: {', '.join(invalid)}")


if __name__ == "__main__":
    main()
