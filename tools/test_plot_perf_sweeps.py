import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from plot_burst_sweep import (  # noqa: E402
    generate_artifacts as generate_burst_artifacts,
    parse_burst_sweep_lines,
)
from plot_outstanding_sweep import (  # noqa: E402
    generate_artifacts as generate_outstanding_artifacts,
    parse_outstanding_sweep_lines,
)


def perf_line(kind, case, osd_cfg, burst_len, request_bytes, e2e_bpc,
              hbm_bpc, credit_stalls, hbm_osd_peak, p99,
              status="PASS", pattern=None):
    pattern_field = "" if pattern is None else f"pattern={pattern} "
    return (
        "PERF_SWEEP "
        f"kind={kind} case={case} {pattern_field}osd_cfg={osd_cfg} "
        f"osd_peak_min={osd_cfg} osd_peak_max={osd_cfg} "
        f"burst_len={burst_len} request_bytes={request_bytes} "
        f"e2e_bpc={e2e_bpc:.6f} dat_bpc={e2e_bpc:.6f} "
        f"hbm_bpc={hbm_bpc:.6f} req_packets=1000 dat_packets=900 "
        "dat_ser_eff_pct=100.000000 "
        f"credit_stalls={credit_stalls} queue_full_stalls=0 "
        f"hbm_osd_peak={hbm_osd_peak} p99={p99} status={status}\n"
    )


class OutstandingSweepPlotTest(unittest.TestCase):
    def test_parser_filters_kind_and_keeps_osd_512(self):
        lines = [
            "[ RUN      ] RingOutstandingSweep.PrivateNoMergeRead256B\n",
            perf_line("outstanding", "osd512", 512, 2, 256,
                      478.801826, 957.603653, 63882, 181, 3428),
            perf_line("burst", "burst128", 64, 1, 128,
                      215.462667, 0.0, 0, 0, 341),
            perf_line("outstanding", "osd16", 16, 2, 256,
                      50.793257, 101.586514, 43, 32, 655),
        ]

        records = parse_outstanding_sweep_lines(lines)

        self.assertEqual([16, 512],
                         [record["osd_cfg"] for record in records])
        self.assertEqual(478.801826, records[1]["e2e_bpc"])
        self.assertEqual(3428, records[1]["p99"])

    def test_generator_writes_csv_and_three_png_charts(self):
        records = parse_outstanding_sweep_lines(
            perf_line("outstanding", f"osd{osd}", osd, 2, 256,
                      e2e, e2e * 2.0, stalls, hbm_osd, p99)
            for osd, e2e, stalls, hbm_osd, p99 in (
                (16, 50.8, 43, 32, 655),
                (256, 488.9, 63922, 181, 1346),
                (512, 478.8, 63882, 181, 3428),
            )
        )

        with tempfile.TemporaryDirectory() as directory:
            outputs = generate_outstanding_artifacts(records, directory)

            self.assertEqual(
                {
                    "outstanding_sweep_summary.csv",
                    "outstanding_vs_bandwidth.png",
                    "outstanding_vs_p99.png",
                    "outstanding_vs_resource_pressure.png",
                },
                {path.name for path in outputs},
            )
            self._assert_artifacts(outputs, 3, "osd_cfg", "16")

    def _assert_artifacts(self, outputs, expected_rows, first_key,
                          first_value):
        for output in outputs:
            self.assertTrue(output.is_file())
            self.assertGreater(output.stat().st_size, 0)
        csv_path = next(path for path in outputs
                        if path.suffix == ".csv")
        with csv_path.open(newline="", encoding="utf-8") as csv_file:
            rows = list(csv.DictReader(csv_file))
        self.assertEqual(expected_rows, len(rows))
        self.assertEqual(first_value, rows[0][first_key])
        png_path = next(path for path in outputs
                        if path.suffix == ".png")
        self.assertEqual(b"\x89PNG\r\n\x1a\n", png_path.read_bytes()[:8])


class BurstSweepPlotTest(unittest.TestCase):
    def test_parser_keeps_pattern_and_sorts_pattern_size_matrix(self):
        lines = [
            perf_line("burst", "shared512", 64, 4, 512,
                      900.0, 0.0, 0, 0, 300,
                      pattern="shared"),
            perf_line("burst", "nomerge256", 64, 2, 256,
                      400.0, 0.0, 0, 0, 200,
                      pattern="nomerge"),
            perf_line("burst", "shared128", 64, 1, 128,
                      700.0, 0.0, 0, 0, 100,
                      pattern="shared"),
            perf_line("burst", "nomerge128", 64, 1, 128,
                      200.0, 0.0, 0, 0, 100,
                      pattern="nomerge"),
        ]

        records = parse_burst_sweep_lines(lines)

        self.assertEqual(
            [
                ("nomerge", 128),
                ("nomerge", 256),
                ("shared", 128),
                ("shared", 512),
            ],
            [(record.get("pattern"), record["request_bytes"])
             for record in records],
        )

    def test_parser_filters_kind_and_sorts_by_request_bytes(self):
        lines = [
            perf_line("burst", "burst512", 64, 4, 512,
                      502.823713, 0.0, 0, 0, 598),
            perf_line("outstanding", "osd16", 16, 2, 256,
                      50.793257, 101.586514, 43, 32, 655),
            perf_line("burst", "burst128", 64, 1, 128,
                      215.462667, 0.0, 0, 0, 341),
        ]

        records = parse_burst_sweep_lines(lines)

        self.assertEqual([128, 512],
                         [record["request_bytes"] for record in records])
        self.assertEqual(["private", "private"],
                         [record.get("pattern") for record in records])
        self.assertEqual(4, records[1]["burst_len"])

    def test_non_passing_case_is_rejected(self):
        records = parse_burst_sweep_lines([
            perf_line("burst", "burst128", 64, 1, 128,
                      215.0, 0.0, 0, 0, 341, status="FAIL"),
        ])

        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "did not pass"):
                generate_burst_artifacts(records, directory)

    def test_generator_writes_csv_and_three_png_charts(self):
        records = parse_burst_sweep_lines(
            perf_line(
                "burst", f"burst_{pattern}_{size}b", 64,
                size // 128, size, e2e, 0.0, 0, 0, p99,
                pattern=pattern,
            )
            for pattern, scale in (
                ("nomerge", 1.0),
                ("private", 1.1),
                ("scatter", 1.3),
                ("shared", 1.6),
            )
            for size, e2e, p99 in (
                (128, 215.5 * scale, 341),
                (256, 425.4 * scale, 358),
                (512, 502.8 * scale, 598),
            )
        )

        with tempfile.TemporaryDirectory() as directory:
            try:
                outputs = generate_burst_artifacts(records, directory)
            except ValueError as error:
                self.fail("pattern/size matrix was rejected: {}".format(error))

            self.assertEqual(
                {
                    "burst_sweep_summary.csv",
                    "request_bytes_vs_bandwidth.png",
                    "request_bytes_vs_p99.png",
                    "request_bytes_vs_packet_counts.png",
                },
                {path.name for path in outputs},
            )
            OutstandingSweepPlotTest._assert_artifacts(
                self, outputs, 12, "pattern", "nomerge")


if __name__ == "__main__":
    unittest.main()
