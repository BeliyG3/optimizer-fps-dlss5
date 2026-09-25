"""Regression checks for the deliberately narrow Off dimension exception."""
import tempfile
import unittest
from pathlib import Path

from compare_ngx_audit import compare, equivalent, read_blocks


class AuditComparisonTests(unittest.TestCase):
    def test_equal_off_dimensions(self):
        for key in ("DLSSNR.Width", "DLSSNR.Height"):
            self.assertTrue(equivalent(key, ("uint32", "1920"), ("int32", "1920"), True))

    def test_changed_dimensions_block(self):
        self.assertFalse(equivalent("DLSSNR.Width", ("uint32", "1920"), ("int32", "1919"), True))

    def test_warp_types_stay_strict(self):
        self.assertFalse(equivalent("DLSSNR.Width", ("uint32", "1920"), ("int32", "1920")))

    def test_other_keys_types_and_invalid_sizes_block(self):
        for key, left, right in [
            ("Other", ("uint32", "1"), ("int32", "1")),
            ("DLSSNR.Width", ("float", "1"), ("int32", "1")),
            ("DLSSNR.Width", ("uint32", "-1"), ("int32", "-1")),
            ("DLSSNR.Width", ("uint32", "2147483648"), ("int32", "2147483648")),
        ]:
            self.assertFalse(equivalent(key, left, right, True))

    def test_resource_descriptor_change_blocks(self):
        self.assertTrue(equivalent("DLSSNR.Color", ("pointer", "resource width=1"),
                                   ("d3d12", "resource width=1")))
        self.assertFalse(equivalent("DLSSNR.Color", ("pointer", "resource width=1"),
                                    ("d3d12", "resource width=2")))

    def test_blocks_and_missing_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            a, b = Path(directory) / "a.log", Path(directory) / "b.log"
            text = "[ngx audit] begin addon create\n[ngx audit] DLSSNR.Width uint32 1920\n[ngx audit] end\n"
            a.write_text(text)
            b.write_text(text.replace("uint32", "int32"))
            self.assertEqual(compare(a, b, True)["off_dimension_type_exceptions"], 1)
            b.write_text(text.replace("uint32 1920", "int32 1919"))
            with self.assertRaises(ValueError):
                compare(a, b, True)
            b.write_text(text.replace("[ngx audit] DLSSNR.Width uint32 1920\n", ""))
            with self.assertRaises(ValueError):
                compare(a, b, True)
            b.write_text(text.replace("[ngx audit] end\n", ""))
            with self.assertRaises(ValueError):
                read_blocks(b)


if __name__ == "__main__":
    unittest.main()
