import csv
from pathlib import Path
import tempfile
import unittest

from analyze_stutters import COLUMNS, TraceFormatError, analyze, trace_state

HEADER = ",".join(COLUMNS) + "\n"


class AnalyzeStuttersTests(unittest.TestCase):
    def analyze_rows(self, rows, fps=120, warmup=0, tail="", **options):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            with path.open("w", newline="") as stream:
                writer = csv.writer(stream)
                writer.writerow(["event", "timestamp_ns", "frame_id", "slot", "duration_ns",
                                 "deadline_ns", "interval_ns", "detail"])
                writer.writerows(rows)
                stream.write(tail)
            return analyze(path, warmup=warmup, fps=fps, **options)

    def analyze_text(self, text, fps=60, warmup=0):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            path.write_bytes(text.encode("utf-8"))
            return analyze(path, warmup=warmup, fps=fps)

    @staticmethod
    def present(ms, frame, real=True):
        at = round(ms * 1_000_000)
        return ["present_real" if real else "present_interp", at, frame, 0, 0, at, 8_333_333, 0]

    @staticmethod
    def limited(ms, frame):
        return ["display_limited", round(ms * 1_000_000), frame, -1, 0, 0, 16_666_667, 1]

    @staticmethod
    def seal(ms, frame, period_ns):
        return ["seal", round(ms * 1_000_000), frame, -1, 1_000_000, 0, period_ns, 0]

    @staticmethod
    def ready(ms):
        return ["benchmark_ready", round(ms * 1_000_000), 0, -1, 0, 0, 0, 0]

    def pal_rows(self, gaps_ms, start=100):
        rows, at = [], start
        for frame, gap in enumerate([0, *gaps_ms], 1):
            at += gap
            rows += [self.present(at, frame), self.seal(at - 1, frame, 20_000_000)]
        return rows

    def display_limited_rows(self, frames=40):
        rows = []
        for i in range(frames):
            rows += [self.present(100 + i * 1000 / 60, i + 1), self.limited(100 + i * 1000 / 60, i + 1)]
        return rows

    def test_regular_interpolation_has_no_stutters(self):
        rows = [self.present(100 + i * 1000 / 120, i // 2, i % 2 == 1) for i in range(40)]
        result = self.analyze_rows(rows)
        self.assertEqual(result["real"]["stutters"], 0)
        self.assertEqual(result["all_presentations"]["stutters"], 0)
        self.assertAlmostEqual(result["all_presentations"]["p99_ms"], 1000 / 120, places=5)

    def test_missing_interpolation_slot_does_not_mean_real_frame_stutter(self):
        rows = [self.present(100, 1), self.present(108.333333, 2, False),
                self.present(116.666667, 2), self.present(133.333333, 3)]
        rows.append(["seal", 130_000_000, 3, -1, 4_000_000, 0, 0, 0])
        rows.append(["drop_late", 131_000_000, 3, 0, 0, 125_000_000, 8_333_333, 0])
        result = self.analyze_rows(rows)
        self.assertEqual(result["real"]["stutters"], 0)
        self.assertEqual(result["all_presentations"]["stutters"], 1)
        self.assertEqual(result["all_presentations"]["effective_stutters"], 1)
        self.assertIn("interpolation_seal", result["all_presentations"]["signals"])
        self.assertIn("late_slot_dropped", result["all_presentations"]["effective_signals"])
        self.assertEqual(result["events"]["drop_late"], 1)
        self.assertEqual(result["isolated_drops"], 0)

    def test_real_frame_gap_and_driver_wait(self):
        rows = [self.present(100, 1), self.present(140, 2),
                ["surface_wait", 140_000_001, 2, 0, 20_000_000, 0, 0, 0]]
        result = self.analyze_rows(rows, fps=60)
        self.assertEqual(result["real"]["stutters"], 1)
        self.assertIn("surface_or_driver_wait", result["real"]["signals"])
        self.assertEqual(result["interpolation"]["status"], "not_requested")
        self.assertTrue(result["interpolation"]["target_valid"])

    def test_game_work_is_distinct_from_gx_producer_wall_time(self):
        rows = [self.present(100, 1), self.present(140, 2),
                ["game_busy", 139_000_000, 2, -1, 30_000_000, 0, 40_000_000, 10_000_000],
                ["produce", 138_000_000, 2, -1, 5_000_000, 0, 0, 0]]
        result = self.analyze_rows(rows, fps=60)
        self.assertIn("game_thread_busy_over_budget", result["real"]["signals"])
        self.assertNotIn("producer_wall_time", result["real"]["signals"])
        self.assertEqual(result["unassociated_game_timing_samples"], 0)

    def test_warmup_excludes_cross_boundary_gap(self):
        rows = [self.present(100, 1), self.present(2100, 2), self.present(2116.666667, 3)]
        result = self.analyze_rows(rows, fps=60, warmup=1)
        self.assertEqual(result["real"]["count"], 1)
        self.assertEqual(result["real"]["stutters"], 0)

    def test_loss_and_incomplete_final_write_are_visible(self):
        rows = [self.present(100, 1), self.present(116.666667, 2),
                ["trace_lost", 0, 0, -1, 0, 0, 0, 3]]
        result = self.analyze_rows(rows, fps=60, tail="present_real,123")
        self.assertEqual(result["lost_trace_samples"], 3)
        self.assertTrue(result["truncated_final_row"])

    def test_display_limit_is_reported_separately_from_cpu_signals(self):
        rows = [self.present(100, 1), self.present(116.666667, 2),
                ["display_limited", 116_000_000, 2, -1, 0, 0, 16_666_667, 1]]
        result = self.analyze_rows(rows)
        self.assertIn("display_refresh_limited", result["all_presentations"]["signals"])

    def test_average_fps_does_not_hide_alternating_intervals(self):
        rows = [self.present(100, 1), self.present(115, 2, False),
                self.present(116.666667, 2), self.present(131.666667, 3, False),
                self.present(133.333333, 3)]
        result = self.analyze_rows(rows)
        self.assertEqual(result["real"]["stutters"], 0)
        self.assertEqual(result["all_presentations"]["stutters"], 2)
        transitions = result["all_presentations"]["transitions"]
        self.assertAlmostEqual(transitions["present_real->present_interp"]["median_ms"], 15)
        self.assertAlmostEqual(transitions["present_interp->present_real"]["median_ms"], 1000 / 600, places=5)

    def test_low_transform_coverage_is_visible_even_with_regular_presents(self):
        rows = [self.present(100, 1), self.present(108.333333, 2, False),
                self.present(116.666667, 2),
                ["interp_matches", 116_000_000, 2, -1, 0, 0, 0, (100 << 32) | 20]]
        result = self.analyze_rows(rows)
        self.assertEqual(result["all_presentations"]["stutters"], 0)
        self.assertEqual(result["transform_matches"]["low_match_frames"], 1)
        self.assertEqual(result["transform_matches"]["median_percent"], 20)

    def test_healthy_60hz_display_requested_at_120_is_not_a_stutter_regression(self):
        result = self.analyze_rows(self.display_limited_rows())
        self.assertEqual(result["real"]["stutters"], 0)
        self.assertGreater(result["all_presentations"]["stutters"], 0)
        self.assertEqual(result["all_presentations"]["effective_stutters"], 0)
        self.assertEqual(result["all_presentations"]["effective_signals"], {})
        self.assertGreater(result["all_presentations"]["display_limited_gaps"], 0)
        self.assertEqual(result["interpolation"]["status"], "display_limited")
        self.assertTrue(result["interpolation"]["display_limited"])
        self.assertFalse(result["interpolation"]["target_valid"])
        self.assertEqual(result["isolated_drops"], 0)

    def test_real_gap_on_display_limited_stream_is_still_a_stutter(self):
        rows = self.display_limited_rows(10)
        rows += [self.present(100 + 9 * 1000 / 60 + 50, 11), self.limited(100 + 9 * 1000 / 60 + 50, 11)]
        result = self.analyze_rows(rows)
        self.assertEqual(result["all_presentations"]["effective_stutters"], 1)
        self.assertEqual(result["real"]["stutters"], 1)

    def test_isolated_drop_late_survives_healthy_cadence(self):
        rows = [self.present(100 + i * 1000 / 120, i // 2, i % 2 == 1) for i in range(40)]
        rows.append(["drop_late", 150_000_000, 5, 0, 0, 149_000_000, 8_333_333, 0])
        result = self.analyze_rows(rows)
        self.assertEqual(result["all_presentations"]["effective_stutters"], 0)
        self.assertEqual(result["isolated_drops"], 1)
        self.assertEqual(result["isolated_drop_examples"][0]["event"], "drop_late")

    def test_display_and_present_failures_map_to_signals_in_the_gap_window(self):
        rows = [self.present(100, 1), self.present(150, 4),
                ["drop_display", 110_000_000, 2, 0, 0, 0, 0, 0],
                ["present_unavailable", 120_000_000, 3, -1, 0, 0, 0, 0],
                ["present_failed", 130_000_000, 3, -1, 0, 0, 0, 0]]
        result = self.analyze_rows(rows, fps=60)
        signals = result["all_presentations"]["effective_signals"]
        for name in ("display_slot_dropped", "present_unavailable", "present_failed"):
            self.assertIn(name, signals)
        self.assertEqual(result["isolated_drops"], 0)

    def test_first_transform_task_anchors_warmup(self):
        rows = [self.present(100 + i * 1000 / 60, i) for i in range(600)]
        rows += [["interp_uniforms", 200_000_000, 0, -1, 1000, 0, 0, 0],
                 ["interp_uniforms", 5_000_000_000, 0, -1, 1000, 0, 0, 12]]
        result = self.analyze_rows(rows, warmup=1, anchor="first_transform")
        self.assertEqual(result["timing_anchor"]["used"], "first_transform")
        self.assertEqual(result["timing_anchor"]["start_ns"], 5_000_000_000)
        self.assertLess(result["measured_seconds"], 4.2)

    def test_missing_transform_falls_back_and_is_flagged(self):
        rows = [self.present(100 + i * 1000 / 120, i // 2, i % 2 == 1) for i in range(40)]
        result = self.analyze_rows(rows)
        self.assertEqual(result["timing_anchor"]["used"], "first_real_present")
        self.assertFalse(result["timing_anchor"]["found"])
        self.assertEqual(result["interpolation"]["status"], "no_transform_work")
        self.assertTrue(self.analyze_rows(rows, anchor="first_row")["timing_anchor"]["found"])

    def test_benchmark_ready_anchors_auto_for_every_fps(self):
        rows = [self.present(100 + i * 1000 / 60, i) for i in range(600)]
        rows += [self.ready(3000), ["interp_uniforms", 5_000_000_000, 0, -1, 1000, 0, 0, 12]]
        for fps in (60, 120, 180):
            with self.subTest(fps=fps):
                timing = self.analyze_rows(rows, fps=fps, warmup=1)["timing_anchor"]
                self.assertEqual((timing["used"], timing["start_ns"], timing["found"]),
                                 ("benchmark_ready", 3_000_000_000, True))

    def test_explicit_anchors_still_select_their_event(self):
        rows = [self.present(100 + i * 1000 / 60, i) for i in range(600)]
        rows += [self.ready(3000), ["interp_uniforms", 5_000_000_000, 0, -1, 1000, 0, 0, 12]]
        for anchor, start in (("first_transform", 5_000_000_000), ("first_row", 100_000_000),
                              ("first_real_present", 100_000_000), ("benchmark_ready", 3_000_000_000)):
            with self.subTest(anchor=anchor):
                timing = self.analyze_rows(rows, warmup=1, anchor=anchor)["timing_anchor"]
                self.assertEqual((timing["used"], timing["start_ns"], timing["found"]), (anchor, start, True))

    def test_missing_benchmark_ready_falls_back_for_legacy_traces(self):
        rows = [self.present(100 + i * 1000 / 60, i) for i in range(20)]
        for anchor in ("auto", "benchmark_ready"):
            with self.subTest(anchor=anchor):
                timing = self.analyze_rows(rows, fps=60, anchor=anchor)["timing_anchor"]
                self.assertEqual((timing["used"], timing["found"]), ("first_real_present", False))

    def test_transform_work_status_does_not_depend_on_the_anchor(self):
        rows = [self.ready(90), *[self.present(100 + i * 1000 / 120, i // 2, i % 2 == 1) for i in range(40)]]
        self.assertEqual(self.analyze_rows(rows)["interpolation"]["status"], "no_transform_work")
        rows.append(["interp_uniforms", 150_000_000, 0, -1, 1000, 0, 0, 12])
        result = self.analyze_rows(rows)
        self.assertEqual(result["timing_anchor"]["used"], "benchmark_ready")
        self.assertEqual(result["interpolation"]["status"], "ok")

    def test_native_period_comes_from_seal_intervals_not_present_gaps(self):
        result = self.analyze_rows(self.pal_rows([17, 23, 19, 22, 18, 21]), fps=60)
        self.assertEqual(result["native_period_ms"], 20)
        self.assertEqual(result["native_period_source"], "seal")
        self.assertEqual(result["real"]["budget_ms"], 20)
        self.assertEqual(self.analyze_rows([self.present(100, 1)], fps=60)["native_period_source"], "default_60hz")

    def test_pal_cadence_is_healthy_and_a_late_frame_is_not(self):
        result = self.analyze_rows(self.pal_rows([20] * 20), fps=60)
        self.assertEqual(result["real"]["stutters"], 0)
        rows = self.pal_rows([20] * 10 + [45] + [20] * 10)
        rows.append(["game_busy", 330_000_000, 12, -1, 19_000_000, 0, 20_000_000, 1_000_000])
        late = self.analyze_rows(rows, fps=60)["real"]
        self.assertEqual(late["stutters"], 1)
        self.assertEqual(late["worst"][0]["gap_ms"], 45)
        self.assertEqual(late["signals"], {"unattributed": 1})

    def test_pal_budgets_scale_stage_thresholds(self):
        rows = self.pal_rows([20, 45])
        rows += [["game_busy", 164_000_000, 3, -1, 30_000_000, 0, 0, 0],
                 ["produce", 160_000_000, 3, -1, 22_000_000, 0, 0, 0]]
        signals = self.analyze_rows(rows, fps=60)["real"]["signals"]
        self.assertIn("game_thread_busy_over_budget", signals)
        self.assertNotIn("producer_wall_time", signals)

    def test_pal_display_limit_without_interval_uses_the_native_period(self):
        rows = []
        for i in range(40):
            at = 100 + i * 20
            rows += [self.present(at, i + 1), self.seal(at - 1, i + 1, 20_000_000),
                     ["display_limited", at * 1_000_000, i + 1, -1, 0, 0, 0, 1]]
        result = self.analyze_rows(rows, fps=120)
        self.assertEqual(result["all_presentations"]["budget_ms"], 1000 / 120)
        self.assertEqual(result["all_presentations"]["effective_budget_ms"], 20)
        self.assertEqual(result["all_presentations"]["effective_stutters"], 0)
        self.assertEqual(result["interpolation"]["status"], "display_limited")

    def test_oversized_csv_field_is_a_line_numbered_format_error(self):
        text = HEADER + "present_real,1,1,0,0,0,0,0\n" + "present_real,2,2,0,0,0,0," + "9" * 200_000 + "\n"
        with self.assertRaisesRegex(TraceFormatError, r"line 3.*field larger"):
            self.analyze_text(text)

    def test_overlong_integer_is_a_line_numbered_format_error(self):
        with self.assertRaisesRegex(TraceFormatError, r"line 2.*'detail' is not an integer"):
            self.analyze_text(HEADER + "present_real,1,1,0,0,0,0," + "9" * 5000 + "\n")

    def test_invalid_arguments_are_rejected(self):
        rows = [self.present(100, 1)]
        for options in ({"warmup": float("nan")}, {"warmup": -1}, {"fps": 0}, {"fps": 60.5}):
            with self.assertRaises(ValueError, msg=str(options)):
                self.analyze_rows(rows, **{"fps": 60, **options})
        with self.assertRaises(ValueError):
            self.analyze_rows(rows, anchor="bogus")

    def test_header_problems_name_line_one(self):
        cases = {
            "duplicate": (HEADER.strip() + ",detail\n", r"line 1.*duplicate columns \['detail'\]"),
            "missing": (HEADER.replace(",detail", ""), r"line 1.*missing columns \['detail'\]"),
            "foreign": ("a,b,c\n1,2,3\n", r"line 1.*missing columns.*unexpected columns \['a', 'b', 'c'\]"),
        }
        for name, (text, pattern) in cases.items():
            with self.subTest(name), self.assertRaisesRegex(TraceFormatError, pattern):
                self.analyze_text(text)

    def test_row_problems_name_the_physical_line(self):
        cases = {
            "non-integer": ("present_real,1,1,0,0,0,0,0\npresent_real,x,2,0,0,0,0,0\npresent_real,3,3,0,0,0,0,0\n",
                            r"line 3.*'timestamp_ns'.*'x'"),
            "short": ("present_real,1,1\npresent_real,2,2,0,0,0,0,0\n", r"line 2.*expected 8 fields, got 3"),
            "negative": ("present_real,1,1,0,-5,0,0,0\npresent_real,2,2,0,0,0,0,0\n",
                         r"line 2.*'duration_ns'.*negative"),
            "blank": ("present_real,1,1,0,0,0,0,0\n\npresent_real,2,2,0,0,0,0,0\n", r"line 3.*got 0"),
            "empty event": (",1,1,0,0,0,0,0\n", r"line 2.*empty event"),
        }
        for name, (rows, pattern) in cases.items():
            with self.subTest(name), self.assertRaisesRegex(TraceFormatError, pattern):
                self.analyze_text(HEADER + rows)

    def test_trace_without_timing_samples_is_rejected(self):
        for name, text in {"lost only": HEADER + "trace_lost,0,0,-1,0,0,0,3\n", "header only": HEADER}.items():
            with self.subTest(name), self.assertRaisesRegex(TraceFormatError, "no timing samples"):
                self.analyze_text(text)
        for name, text in {"empty": "", "partial header": "event,timestamp_ns"}.items():
            with self.subTest(name), self.assertRaisesRegex(TraceFormatError, "no complete header"):
                self.analyze_text(text)

    def test_truncated_final_row_is_ignored_even_when_malformed(self):
        result = self.analyze_text(HEADER + "present_real,100,1,0,0,0,0,0\npresent_real,2x")
        self.assertTrue(result["truncated_final_row"])
        self.assertEqual(result["events"]["present_real"], 1)

    def test_trace_state_reports_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.csv"
            path.write_bytes(b"event,timestamp_ns")
            self.assertIsNone(trace_state(path))
            path.write_bytes((HEADER + "present_real,5,1,0,0,0,0,0\ninterp_uniforms,9,0,-1,1,0,0,0\n"
                              "interp_uniforms,11,0,-1,1,0,0,4\ndisplay_limited,12,1,-1,0,0,0,1\n"
                              "present_real,13").encode())
            self.assertEqual(trace_state(path), {"benchmark_ready_ns": None, "real_present_ns": 5,
                                                 "transform_ns": 11, "display_limited": True})
            path.write_bytes((HEADER + "present_real,5,1,0,0,0,0,0\nbenchmark_ready,7,0,-1,0,0,0,0\n"
                              "benchmark_ready,8,0,-1,0,0,0,0\n").encode())
            self.assertEqual(trace_state(path)["benchmark_ready_ns"], 7)


if __name__ == "__main__":
    unittest.main()
