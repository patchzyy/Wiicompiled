import tempfile
import unittest
from pathlib import Path

from classify_changes import classify, write_github_output


class ClassifyChangesTests(unittest.TestCase):
    def test_documentation_only_skips_expensive_jobs(self) -> None:
        self.assertEqual(classify(["README.md", "docs/building-macos.md"]), {
            "native": False,
            "aurora": False,
        })

    def test_translator_change_runs_synthetic_native_build(self) -> None:
        self.assertEqual(classify(["translator/src/Translator.Core/Foo.cs"]), {
            "native": True,
            "aurora": False,
        })

    def test_runtime_change_runs_native_without_standalone_aurora(self) -> None:
        self.assertEqual(classify(["runtime/src/foo.cpp"]), {
            "native": True,
            "aurora": False,
        })

    def test_aurora_change_runs_both_native_suites(self) -> None:
        self.assertEqual(classify(["aurora-main/lib/gx/foo.cpp"]), {
            "native": True,
            "aurora": True,
        })

    def test_workflow_and_project_changes_fail_safe(self) -> None:
        for path in (".github/workflows/build.yml", "global.json", "new.csproj"):
            with self.subTest(path=path):
                self.assertEqual(classify([path]), {"native": True, "aurora": True})

    def test_windows_paths_are_normalized(self) -> None:
        self.assertTrue(classify([r"runtime\tests\new_tests.cpp"])["native"])

    def test_manual_run_forces_every_category(self) -> None:
        self.assertEqual(classify([], force_all=True), {"native": True, "aurora": True})

    def test_github_output_uses_lowercase_booleans(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            write_github_output({"native": True, "aurora": False}, str(output))
            self.assertEqual(output.read_text(), "native=true\naurora=false\n")


if __name__ == "__main__":
    unittest.main()
