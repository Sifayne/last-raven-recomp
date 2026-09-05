"""No game data or GPU needed: validate the comparison and baseline guards."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("render_check", Path(__file__).with_name("11-render-check.py"))
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class RenderCheckTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "image.ppm"

    def test_ppm_preserves_whitespace_pixels(self):
        self.path.write_bytes(b"P6\n# dimensions\n1 1\n255\n\n\r ")
        self.assertEqual(check.read_ppm(self.path), (1, 1, b"\n\r "))

    def test_ppm_crlf(self):
        self.path.write_bytes(b"P6\r\n1 1\r\n255\r\n\x00\x00\x00")
        self.assertEqual(check.read_ppm(self.path), (1, 1, bytes(3)))

    def test_invalid_ppm(self):
        for data in (b"P6\n1 1\n255\n\x00", b"P6\n1 1\n255\n1234",
                     b"P3\n1 1\n255\n123", b"P6\n-1 1\n255\n",
                     b"P6\n#unterminated", b"P6\n1 1\n65535\n123456"):
            with self.subTest(data=data):
                self.path.write_bytes(data)
                with self.assertRaises(ValueError):
                    check.read_ppm(self.path)

    def test_exact_metrics(self):
        image = (2, 1, bytes([20, 40, 60, 80, 100, 120]))
        result = check.metrics(image, image)
        self.assertEqual(result["exact_pixels"], 2)
        self.assertEqual(result["rmse"], 0)
        self.assertIsNone(result["error_bbox"])
        self.assertEqual(result["software_mean"], 70)

    def test_pixel_threshold_uses_worst_channel(self):
        result = check.metrics((2, 1, bytes(6)), (2, 1, bytes([2, 0, 0, 0, 3, 4])))
        self.assertEqual(result["pixels_over_2"], 1)
        self.assertEqual(result["max_channel_error"], 4)
        self.assertEqual(result["error_bbox"], [1, 0, 1, 0])
        self.assertAlmostEqual(result["rmse"], (29 / 6) ** 0.5 / 255)

    def test_region_is_independent(self):
        a, b = (2, 2, bytes(12)), (2, 2, bytes(6) + bytes([9, 8, 7, 0, 0, 0]))
        self.assertEqual(check.metrics(a, b, (0, 0, 2, 1))["rmse"], 0)
        self.assertEqual(check.metrics(a, b, (0, 1, 1, 1))["max_channel_error"], 9)
        with self.assertRaises(ValueError):
            check.metrics(a, b, (1, 1, 2, 1))

    def test_size_mismatch(self):
        with self.assertRaises(ValueError):
            check.metrics((1, 1, bytes(3)), (2, 1, bytes(6)))

    def test_coverage_is_separate_from_pixel_parity(self):
        text = ("25 point/line draw(s) skipped, blend not represented: 2 factor, "
                "3 equation, 912 draw(s) need alpha-backed stencil")
        self.assertEqual(check.coverage(text), {"point_line_draws_skipped": 25,
                         "blend_factors_unsupported": 2, "blend_equations_unsupported": 3,
                         "stencil_draws_unsupported": 912})
        self.assertEqual(sum(check.coverage("").values()), 0)

    def test_capture_size_and_multilist_guard(self):
        # Header + synthetic state/list/RAM, no game-derived bytes.
        data = struct.pack("<10I", 0x50414347, 2, 4, 1, 0, 4, 0, 0, 0, 0) + bytes(20)
        self.path.write_bytes(data)
        self.assertEqual(check.capture_header(self.path)["lists"], 1)
        self.path.write_bytes(data[:-1])
        with self.assertRaises(ValueError):
            check.capture_header(self.path)
        self.path.write_bytes(struct.pack("<10I", 0x50414347, 2, 4, 2, 0, 4, 0, 0, 0, 0) + bytes(32))
        with self.assertRaisesRegex(ValueError, "multi-list"):
            check.capture_header(self.path)

    def baseline(self):
        return {"cases": [{"name": "scene", "capture_sha256": "capture", "software_sha256": "oracle",
                           "region_rects": {}, "coverage": check.coverage(""),
                           "regions": {"full": {"rmse": 0.01, "pixels_over_2": 3, "max_channel_error": 8}}}]}

    def test_baseline_same_and_improved(self):
        baseline = self.baseline()
        self.assertEqual(check.regressions(baseline, baseline), [])
        newer = copy.deepcopy(baseline)
        newer["cases"][0]["regions"]["full"]["rmse"] = 0.005
        self.assertEqual(check.regressions(newer, baseline), [])

    def test_baseline_regression(self):
        baseline = self.baseline()
        newer = copy.deepcopy(baseline)
        newer["cases"][0]["regions"]["full"]["pixels_over_2"] = 4
        newer["cases"][0]["coverage"]["point_line_draws_skipped"] = 1
        self.assertEqual(len(check.regressions(newer, baseline)), 2)

    def test_baseline_identity_guards(self):
        baseline = self.baseline()
        for field, value in (("capture_sha256", "other"), ("software_sha256", "other"),
                             ("name", "other"), ("region_rects", {"lower-left": [0, 1, 2, 1]})):
            with self.subTest(field=field):
                newer = copy.deepcopy(baseline)
                newer["cases"][0][field] = value
                with self.assertRaises(ValueError):
                    check.regressions(newer, baseline)

    def test_suite_selector_guards(self):
        suite = {"version": 1, "stop_poll": 10, "cases": [{"name": "a", "poll": 2}, {"name": "b", "poll": 5}]}
        self.path.write_text(json.dumps(suite))
        self.assertEqual(len(check.load_suite(self.path)["cases"]), 2)
        for case in ({"name": "a", "poll": 5}, {"name": "../bad", "poll": 5},
                     {"name": "b", "poll": 2}, {"name": "b", "poll": 10}):
            suite["cases"][1] = case
            self.path.write_text(json.dumps(suite))
            with self.assertRaises(ValueError):
                check.load_suite(self.path)


if __name__ == "__main__":
    unittest.main()
