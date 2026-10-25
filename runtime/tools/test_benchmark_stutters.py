from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest
from unittest import mock

import benchmark_stutters
from benchmark_stutters import (ConfigError, SourceConfig, isolated_config_text, load_source_config,
                                interpolation_helpers, parse_helper, parse_list, quoted, stop_process,
                                wait_ready)
from analyze_stutters import COLUMNS

HEADER = ",".join(COLUMNS) + "\n"


class QuotedTests(unittest.TestCase):
    def test_unicode_and_control_characters_roundtrip_through_toml(self):
        for value in ["C:\\Users\\Jürgen\\日本語\\Wii", 'a"b', "tab\there", "del\x7fchar", "\u2028 end", ""]:
            with self.subTest(value=value):
                self.assertEqual(tomllib.loads(f"x = {quoted(value)}")["x"], value)

    def test_isolated_config_roundtrips_paths_and_video_settings(self):
        source = SourceConfig(Path("C:/Spiele/Mario Kart é"), Path("C:/RR/日本"), None, "vulkan")
        text = isolated_config_text(source, Path("C:/Out/ünï/NAND"), 120, 1.5, True)
        config = tomllib.loads(text)
        self.assertEqual(config["paths"]["dvd_root"], str(source.dvd_root))
        self.assertEqual(config["paths"]["retro_rewind_root"], str(source.retro_rewind_root))
        self.assertEqual(config["paths"]["nand_root"], str(Path("C:/Out/ünï/NAND")))
        self.assertEqual(config["video"]["frame_interpolation_fps"], 120)
        self.assertEqual(config["video"]["resolution_multiplier"], 1.5)
        self.assertEqual(config["video"]["graphics_api"], "vulkan")
        self.assertTrue(config["video"]["vsync"])
        self.assertEqual(config["network"], {"enabled": False})

    def test_optional_paths_are_omitted(self):
        config = tomllib.loads(isolated_config_text(SourceConfig(None, None, None, "auto"), Path("N"), 0, 1, False))
        self.assertEqual(config["paths"], {"nand_root": "N"})


class ParseListTests(unittest.TestCase):
    def test_valid_lists_keep_order(self):
        self.assertEqual(parse_list("4, 0,2", "cpus", int), [4, 0, 2])
        self.assertEqual(parse_list("auto,2", "helpers", parse_helper, ("auto", "2")), ["auto", "2"])

    def test_rejects_duplicates_including_equivalent_spellings(self):
        for text, parse in [("0,0", int), ("1,01", int)]:
            with self.subTest(text), self.assertRaisesRegex(ValueError, "duplicate"):
                parse_list(text, "cpus", parse)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            parse_list("auto,0,00", "helpers", parse_helper, ("auto", "0"))

    def test_rejects_malformed_and_unsupported_entries(self):
        for text in ("1,", "", "a"):
            with self.subTest(text), self.assertRaisesRegex(ValueError, "invalid entry"):
                parse_list(text, "cpus", int)
        with self.assertRaisesRegex(ValueError, "unsupported"):
            parse_list("0,60", "fps", int, (0, 120, 180))
        with self.assertRaisesRegex(ValueError, "unsupported"):
            parse_list("7", "helpers", parse_helper, ("auto", "0"))


class SourceConfigTests(unittest.TestCase):
    def load(self, text, create=()):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in create:
                (root / name).mkdir()
            path = root / "Config.toml"
            path.write_text(text.replace("{root}", quoted(root)[1:-1]), encoding="utf-8")
            return load_source_config(path), root

    def test_relative_paths_resolve_against_the_config_directory(self):
        source, root = self.load('[paths]\ndvd_root = "game"\nnand_root = "nand"\n', create=("game", "nand"))
        self.assertEqual(source.dvd_root, (root / "game").resolve())
        self.assertEqual(source.nand_root, (root / "nand").resolve())
        self.assertEqual(source.graphics_api, "auto")

    def test_unicode_directory_names_roundtrip(self):
        name = "Wii é日本"
        source, root = self.load('[paths]\ndvd_root = "{root}\\\\' + name + '"\n', create=(name,))
        self.assertEqual(source.dvd_root, root / name)

    def test_empty_and_missing_values_are_unset(self):
        source, _ = self.load('[paths]\ndvd_root = ""\n')
        self.assertIsNone(source.dvd_root)
        self.assertIsNone(source.nand_root)

    def test_odd_config_types_and_values_are_rejected(self):
        cases = {
            "paths not table": ('paths = "x"\n', "\\[paths\\] must be a table"),
            "video not table": ("video = 3\n", "\\[video\\] must be a table"),
            "path not string": ("[paths]\ndvd_root = 5\n", "paths.dvd_root must be a string"),
            "path list": ('[paths]\nnand_root = ["a"]\n', "paths.nand_root must be a string"),
            "missing path": ('[paths]\ndvd_root = "nope"\n', "does not exist"),
            "api not string": ("[video]\ngraphics_api = 3\n", "graphics_api"),
            "api unsupported": ('[video]\ngraphics_api = "dx11"\n', "graphics_api"),
            "invalid toml": ("[paths\n", "invalid TOML"),
        }
        for name, (text, pattern) in cases.items():
            with self.subTest(name), self.assertRaisesRegex(ConfigError, pattern):
                self.load(text)

    def test_nand_root_must_be_a_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "nand").write_text("file")
            (root / "Config.toml").write_text('[paths]\nnand_root = "nand"\n')
            with self.assertRaisesRegex(ConfigError, "not a directory"):
                load_source_config(root / "Config.toml")

    def test_missing_config_file_is_a_config_error(self):
        with tempfile.TemporaryDirectory() as directory, self.assertRaisesRegex(ConfigError, "cannot read config"):
            load_source_config(Path(directory) / "missing.toml")


class HelperCountTests(unittest.TestCase):
    def test_limited_runs_use_the_requested_cpu_capacity(self):
        for count, expected in ((1, 0), (2, 0), (3, 2), (4, 3), (12, 6)):
            with self.subTest(count=count):
                self.assertEqual(interpolation_helpers(count, "auto"), expected)

    def test_full_cpu_runs_keep_the_runtime_default(self):
        self.assertIsNone(interpolation_helpers(0, "auto"))

    def test_explicit_helper_controls_are_preserved(self):
        self.assertEqual(interpolation_helpers(4, "6"), 6)
        self.assertEqual(interpolation_helpers(4, "0"), 0)


class WaitReadyTests(unittest.TestCase):
    @staticmethod
    def running():
        return mock.Mock(**{"poll.return_value": None})

    def test_requires_benchmark_ready_regardless_of_other_events(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / "trace.csv"
            trace.write_text(HEADER + "present_real,5,1,0,0,0,0,0\ninterp_uniforms,9,0,-1,1,0,0,4\n")
            with mock.patch.object(benchmark_stutters, "STARTUP_TIMEOUT", 0), \
                    self.assertRaisesRegex(RuntimeError, "no benchmark_ready"):
                wait_ready(self.running(), trace, "case")
            trace.write_text(HEADER + "benchmark_ready,7,0,-1,0,0,0,0\n")
            self.assertEqual(wait_ready(self.running(), trace, "case")["benchmark_ready_ns"], 7)


class StopProcessTests(unittest.TestCase):
    def test_kills_after_terminate_times_out(self):
        process = mock.Mock(**{"poll.return_value": None})
        process.wait.side_effect = [subprocess.TimeoutExpired("game", 10), 0]
        stop_process(process, "case")
        process.terminate.assert_called_once()
        process.kill.assert_called_once()

    def test_unkillable_process_is_a_clear_failure(self):
        process = mock.Mock(**{"poll.return_value": None})
        process.wait.side_effect = subprocess.TimeoutExpired("game", 10)
        with self.assertRaisesRegex(RuntimeError, "did not exit after terminate and kill"):
            stop_process(process, "case")


if __name__ == "__main__":
    unittest.main()
