import unittest

from check_required import validate


class CheckRequiredTests(unittest.TestCase):
    def test_accepts_successful_heavy_run(self) -> None:
        self.assertEqual(validate("success", "success", "success", True), [])

    def test_accepts_classifier_authorized_skip(self) -> None:
        self.assertEqual(validate("success", "success", "skipped", False), [])

    def test_rejects_missing_or_failed_work(self) -> None:
        self.assertEqual(len(validate("failure", "cancelled", "skipped", True)), 3)


if __name__ == "__main__":
    unittest.main()
