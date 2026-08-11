import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))

from generate_ring_demo_report import generate_report, parse_result, render_html


SAMPLE_RESULT = """\
TEST_COUNTS read_requests=10 read_responses=10
TEST_HA_WAITER_DIST buckets=1:2,2:1,64:3
TEST_L2_RECIPIENT_DIST buckets=1:4,8:2
TEST_L2_CARRIER_DIST bytes_128=4 bytes_256=1 bytes_512=1 other=0
TEST_STALLS read_send=0 dominant=ring_conn_serialization_busy
TEST_RESULT case=sample status=PASS failures=0
"""


class RingDemoReportTest(unittest.TestCase):
    def test_parses_distribution_buckets_and_renders_offline_report(self):
        parsed = parse_result(SAMPLE_RESULT)

        self.assertEqual(parsed["ha_waiter_buckets"], {1: 2, 2: 1, 64: 3})
        self.assertEqual(parsed["l2_recipient_buckets"], {1: 4, 8: 2})
        self.assertEqual(parsed["carrier_counts"]["bytes_512"], 1)

        html = render_html(parsed, "sample.txt")
        self.assertIn("HA transaction 聚合分布", html)
        self.assertIn("64+", html)
        self.assertIn("Ring 物理包接收者分布", html)
        self.assertIn("512B", html)
        self.assertNotIn("http://", html)
        self.assertNotIn("https://", html)
        self.assertNotIn("<script", html.lower())

    def test_old_result_without_distribution_is_still_readable(self):
        parsed = parse_result("TEST_RESULT case=old status=PASS failures=0\n")
        html = render_html(parsed, "old.txt")

        self.assertEqual(parsed["ha_waiter_buckets"], {})
        self.assertIn("无分布数据", html)

    def test_generate_report_writes_one_self_contained_file(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "result.txt"
            output_path = Path(directory) / "result.html"
            input_path.write_text(SAMPLE_RESULT, encoding="utf-8")

            generated = generate_report(input_path, output_path)

            self.assertEqual(generated, output_path)
            self.assertTrue(output_path.is_file())
            self.assertGreater(output_path.stat().st_size, 0)


if __name__ == "__main__":
    unittest.main()
