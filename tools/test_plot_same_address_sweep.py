import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from plot_same_address_sweep import (  # noqa: E402
    generate_artifacts,
    parse_same_address_sweep_lines,
)


def same_address_line(scenario, e2e_bpc, p99, backend_reads,
                      backend_read_saved, write_hazard_stalls,
                      h_carriers, h_carrier_recipients,
                      status="PASS"):
    return (
        "SAME_ADDRESS_CORE "
        f"scenario={scenario} e2e_bpc={e2e_bpc:.6f} "
        f"p99={p99} backend_reads={backend_reads} "
        f"backend_read_saved={backend_read_saved} "
        f"write_hazard_stalls={write_hazard_stalls} "
        f"h_carriers={h_carriers} "
        f"h_carrier_recipients={h_carrier_recipients} "
        f"status={status}\n"
    )


class SameAddressSweepPlotTest(unittest.TestCase):
    def test_parser_filters_prefix_and_uses_stable_scenario_order(self):
        lines = [
            "unrelated output\n",
            same_address_line(
                "same_address_read_write", 210.0, 1800, 64, 32, 900,
                64, 64),
            same_address_line(
                "shared_read", 420.0, 700, 16, 112, 0, 32, 128),
            same_address_line(
                "nomerge_read", 250.0, 600, 128, 0, 0, 128, 128),
        ]

        records = parse_same_address_sweep_lines(lines)

        self.assertEqual(
            ["nomerge_read", "shared_read", "same_address_read_write"],
            [record["scenario"] for record in records],
        )
        self.assertEqual(210.0, records[2]["e2e_bpc"])
        self.assertEqual(900, records[2]["write_hazard_stalls"])

    def test_generator_rejects_non_passing_or_duplicate_scenarios(self):
        failed = parse_same_address_sweep_lines([
            same_address_line(
                "nomerge_read", 250.0, 600, 128, 0, 0, 128, 128,
                status="INCOMPLETE"),
        ])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "did not pass"):
                generate_artifacts(failed, directory)

        duplicate = parse_same_address_sweep_lines([
            same_address_line(
                "shared_read", 420.0, 700, 16, 112, 0, 32, 128),
            same_address_line(
                "shared_read", 421.0, 701, 16, 112, 0, 32, 128),
        ])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError, "duplicate scenario"):
                generate_artifacts(duplicate, directory)

    def test_generator_writes_csv_and_three_png_charts(self):
        samples = (
            ("nomerge_read", 250.0, 600, 128, 0, 0, 128, 128),
            ("shared_read", 420.0, 700, 16, 112, 0, 32, 128),
            ("scatter_read", 390.0, 750, 32, 96, 0, 32, 128),
            ("private_read_write", 230.0, 900,
             128, 0, 0, 128, 128),
            ("same_address_read_write", 180.0, 1800,
             64, 64, 900, 64, 128),
        )
        records = parse_same_address_sweep_lines(
            same_address_line(
                scenario, e2e, p99, backend_reads, saved, hazard,
                carriers, recipients)
            for (scenario, e2e, p99, backend_reads, saved, hazard,
                 carriers, recipients) in samples
        )

        with tempfile.TemporaryDirectory() as directory:
            outputs = generate_artifacts(records, directory)

            self.assertEqual(
                {
                    "same_address_core_summary.csv",
                    "same_address_bandwidth_latency.png",
                    "same_address_backend_traffic.png",
                    "same_address_fanout_conflict.png",
                },
                {path.name for path in outputs},
            )
            for output in outputs:
                self.assertTrue(output.is_file())
                self.assertGreater(output.stat().st_size, 0)
            csv_path = next(path for path in outputs
                            if path.suffix == ".csv")
            with csv_path.open(newline="", encoding="utf-8") as csv_file:
                rows = list(csv.DictReader(csv_file))
            self.assertEqual(5, len(rows))
            self.assertEqual(
                [
                    "scenario", "e2e_bpc", "p99", "backend_reads",
                    "backend_read_saved", "write_hazard_stalls",
                    "h_carriers", "h_carrier_recipients", "status",
                ],
                list(rows[0]),
            )
            self.assertEqual("nomerge_read", rows[0]["scenario"])
            png_path = next(path for path in outputs
                            if path.suffix == ".png")
            self.assertEqual(b"\x89PNG\r\n\x1a\n",
                             png_path.read_bytes()[:8])


if __name__ == "__main__":
    unittest.main()
