import csv
import tempfile
import unittest
from pathlib import Path

from plot_l2_sweep import generate_artifacts, parse_l2_sweep_lines


def sweep_line(pattern, hit_rate, e2e, hbm, p99):
    return (
        "L2_SWEEP "
        f"case=l2_{pattern}_256b_hit{hit_rate} "
        f"pattern={pattern} request_bytes=256 hit_cfg_pct={hit_rate} "
        f"hit_obs_pct={float(hit_rate):.6f} e2e_bpc={e2e:.6f} "
        f"peak_pct={e2e / 512.0 * 100.0:.6f} dat_bpc={e2e:.6f} "
        f"hbm_bpc={hbm:.6f} p99={p99} status=PASS\n"
    )


class L2SweepPlotTest(unittest.TestCase):
    def test_parser_ignores_noise_and_returns_typed_sorted_rows(self):
        lines = [
            "[ RUN      ] RingL2HitRateSweep.PrivateSequentialRead256B\n",
            sweep_line("private", 50, 473.5, 274.0, 8751),
            sweep_line("nomerge", 0, 156.25, 312.5, 48890),
        ]

        records = parse_l2_sweep_lines(lines)

        self.assertEqual(["nomerge", "private"],
                         [record["pattern"] for record in records])
        self.assertEqual(256, records[0]["request_bytes"])
        self.assertEqual(0, records[0]["hit_cfg_pct"])
        self.assertEqual(156.25, records[0]["e2e_bpc"])
        self.assertEqual(48890, records[0]["p99"])

    def test_generator_writes_csv_and_three_png_charts(self):
        records = parse_l2_sweep_lines(
            sweep_line(pattern, hit_rate,
                       100.0 + hit_rate, 300.0 - hit_rate, 50000 - hit_rate)
            for pattern in ("nomerge", "private", "scatter", "shared")
            for hit_rate in (0, 100)
        )

        with tempfile.TemporaryDirectory() as directory:
            output_dir = Path(directory)
            outputs = generate_artifacts(records, output_dir)

            self.assertEqual(
                {
                    "l2_sweep_summary.csv",
                    "l2_hit_rate_vs_e2e.png",
                    "l2_hit_rate_vs_hbm.png",
                    "l2_hit_rate_vs_p99.png",
                },
                {path.name for path in outputs},
            )
            for output in outputs:
                self.assertTrue(output.is_file())
                self.assertGreater(output.stat().st_size, 0)

            with (output_dir / "l2_sweep_summary.csv").open(
                    newline="", encoding="utf-8") as csv_file:
                rows = list(csv.DictReader(csv_file))
            self.assertEqual(8, len(rows))
            self.assertEqual("nomerge", rows[0]["pattern"])
            self.assertEqual("0", rows[0]["hit_cfg_pct"])

            png = output_dir / "l2_hit_rate_vs_e2e.png"
            self.assertEqual(b"\x89PNG\r\n\x1a\n", png.read_bytes()[:8])


if __name__ == "__main__":
    unittest.main()
