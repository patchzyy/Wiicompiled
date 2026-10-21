"""Binary-free regression tests: python3 -m unittest discover -s tools/region."""
import unittest
import contextlib
import io
import tempfile
from pathlib import Path
from unittest.mock import patch

import port_data_addresses as porter


class Words:
    def __init__(self, words):
        self.words = words

    def word(self, address):
        return self.words.get(address, 0x60000000)  # nop


class AddressProofTests(unittest.TestCase):
    def test_missing_pal_proof_inputs_fail_before_loading_or_overwriting(self):
        with tempfile.TemporaryDirectory() as directory:
            table = Path(directory, "source.txt")
            table.write_text("802574A0 80257420 fixture\n")
            output = Path(directory, "output.txt")
            output.write_text("existing table\n")
            errors = io.StringIO()
            args = ["porter", "--region", "J", "--disc-root", directory,
                    "--source-table", str(table), "--out", str(output)]
            with patch("sys.argv", args), patch.object(porter, "Image") as image, \
                    contextlib.redirect_stderr(errors), self.assertRaises(SystemExit) as failure:
                porter.main()
            self.assertEqual(2, failure.exception.code)
            self.assertIn("FIELD/RECORD proofs require clean PAL inputs", errors.getvalue())
            image.assert_not_called()
            self.assertEqual("existing table\n", output.read_text())

    def test_lis_low_half_can_be_delayed_beyond_eight_instructions(self):
        image = Words({0x1000: 0x3CC08012, 0x1028: 0x38C66954})
        self.assertEqual(0x80126954, porter.formed_address(image, 0x1000, 0, 0))

    def test_register_clobber_ends_lis_sequence(self):
        image = Words({0x1000: 0x3CC08012, 0x1004: 0x38C00000, 0x1028: 0x38C66954})
        self.assertIsNone(porter.formed_address(image, 0x1000, 0, 0))

    def test_signed_low_half_and_lha_address(self):
        image = Words({0x1000: 0x3C60809B, 0x1004: 0xA863D140})
        self.assertEqual(0x809AD140, porter.formed_address(image, 0x1000, 0, 0))

    def test_branch_signatures_retain_link_and_absolute_flags(self):
        self.assertNotEqual(porter._norm(0x48000001), porter._norm(0x48000000))
        self.assertNotEqual(porter._norm(0x48000002), porter._norm(0x48000000))

    def field_images(self):
        source = Words({0x8021B310: 0x3FE08025, 0x8021B314: 0x3BFF7480,
                        0x8021B378: 0x389F0020})
        target = Words({0x8021C310: 0x3FE08025, 0x8021C314: 0x3BFF7240,
                        0x8021C378: 0x389F0020})
        return source, target

    def prove_field(self, source, target):
        with patch.object(porter, "matched_site", side_effect=lambda _s, a, _t: (a + 0x1000, 10)):
            return porter.verified_field(0x802574A0, source, target, "J")

    def test_field_uses_target_base_and_actual_field_instruction(self):
        value, evidence = self.prove_field(*self.field_images())
        self.assertEqual(0x80257260, value)
        self.assertIn("389F0020", evidence)

    def test_field_rejects_changed_displacement(self):
        source, target = self.field_images()
        target.words[0x8021C378] = 0x389F0040
        self.assertIsNone(self.prove_field(source, target)[0])

    def test_field_rejects_intervening_register_clobber(self):
        source, target = self.field_images()
        target.words[0x8021C320] = 0x3BE00000  # li r31,0
        self.assertIsNone(self.prove_field(source, target)[0])

    def test_field_rejects_non_unique_signature(self):
        with patch.object(porter, "matched_site", return_value=(None, None)):
            self.assertIsNone(porter.verified_field(0x802574A0, *self.field_images(), "J")[0])

    def test_indexed_update_store_clobbers_its_base(self):
        stwux = (31 << 26) | (4 << 21) | (31 << 16) | (3 << 11) | (183 << 1)
        self.assertEqual({31}, porter.gpr_writes(stwux))

    def test_unknown_field_has_no_nearest_neighbour_fallback(self):
        self.assertEqual((None, None), porter.verified_field(0x802574A4, *self.field_images(), "J"))

    def test_record_requires_target_stride_and_record_bytes(self):
        source = Words({0x8023E574: 0x3CA0802A, 0x8023E580: 0x38A53EE8,
                        0x8023E56C: 0x1C80000C})
        target = Words({0x8023F574: 0x3CA08029, 0x8023F580: 0x38A51EE8,
                        0x8023F56C: 0x1C80000C})
        records = (0x026001E0, 0x3F86BCA2, 0x3F800000, 0x034001E0, 0x3F44EC4F, 0x3F800000)
        for i, word in enumerate(records):
            source.words[0x802A3EE8 + i * 4] = word
            target.words[0x80291EE8 + i * 4] = word
        with patch.object(porter, "matched_site", side_effect=lambda _s, a, _t: (a + 0x1000, 10)):
            self.assertEqual(0x80291EF4, porter.verified_record(0x802A3EF4, source, target, "J")[0])
            target.words[0x80291EF4] ^= 1
            self.assertIsNone(porter.verified_record(0x802A3EF4, source, target, "J")[0])
            target.words[0x80291EF4] ^= 1
            target.words[0x8023F56C] = 0x1C800018  # changed record stride
            self.assertIsNone(porter.verified_record(0x802A3EF4, source, target, "J")[0])


if __name__ == "__main__":
    unittest.main()
