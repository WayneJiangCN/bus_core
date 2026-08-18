#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


FIELDS = (
    "case",
    "pattern",
    "request_bytes",
    "ha_entries",
    "l2_latency",
    "hit_cfg_pct",
    "hit_obs_pct",
    "e2e_bpc",
    "peak_pct",
    "dat_bpc",
    "hbm_bpc",
    "p99",
    "status",
)
OPTIONAL_FIELDS = {"ha_entries", "l2_latency"}
INTEGER_FIELDS = {
    "request_bytes", "ha_entries", "l2_latency", "hit_cfg_pct", "p99"
}
FLOAT_FIELDS = {"hit_obs_pct", "e2e_bpc", "peak_pct", "dat_bpc", "hbm_bpc"}
PATTERN_ORDER = ("nomerge", "private", "scatter", "shared")
PATTERN_STYLES = {
    "nomerge": {"label": "NoMerge", "linestyle": "--", "marker": "o"},
    "private": {"label": "Private", "linestyle": "-", "marker": "s"},
    "scatter": {"label": "Scatter", "linestyle": "-.", "marker": "^"},
    "shared": {"label": "Shared", "linestyle": ":", "marker": "D"},
}


def parse_l2_sweep_lines(lines):
    records = []
    for line_number, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not stripped.startswith("L2_SWEEP "):
            continue

        values = {}
        for token in stripped.split()[1:]:
            if "=" not in token:
                raise ValueError(
                    "line {} contains an invalid token: {}".format(
                        line_number, token))
            key, value = token.split("=", 1)
            values[key] = value

        missing = [field for field in FIELDS
                   if field not in OPTIONAL_FIELDS and field not in values]
        if missing:
            raise ValueError(
                "line {} is missing fields: {}".format(
                    line_number, ", ".join(missing)))

        record = {field: values.get(field) for field in FIELDS}
        try:
            for field in INTEGER_FIELDS:
                if record[field] is not None:
                    record[field] = int(record[field])
            for field in FLOAT_FIELDS:
                record[field] = float(record[field])
        except ValueError as error:
            raise ValueError(
                "line {} contains a non-numeric value".format(line_number)
            ) from error
        records.append(record)

    pattern_rank = {name: index for index, name in enumerate(PATTERN_ORDER)}
    records.sort(
        key=lambda record: (
            pattern_rank.get(record["pattern"], len(pattern_rank)),
            record["pattern"],
            record["hit_cfg_pct"],
        )
    )
    return records


def load_records(input_path):
    with Path(input_path).open(encoding="utf-8-sig") as input_file:
        return parse_l2_sweep_lines(input_file)


def validate_records(records):
    if not records:
        raise ValueError("input contains no L2_SWEEP records")

    request_sizes = {record["request_bytes"] for record in records}
    if len(request_sizes) != 1:
        raise ValueError("all records must use the same request_bytes")

    seen = set()
    for record in records:
        key = (record["pattern"], record["hit_cfg_pct"])
        if key in seen:
            raise ValueError(
                "duplicate pattern/hit-rate point: {} {}".format(*key))
        seen.add(key)
        if record["status"] != "PASS":
            raise ValueError(
                "case {} did not pass".format(record["case"]))


def write_summary_csv(records, output_path):
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(records)


def ordered_patterns(records):
    present = {record["pattern"] for record in records}
    known = [pattern for pattern in PATTERN_ORDER if pattern in present]
    unknown = sorted(present.difference(PATTERN_ORDER))
    return known + unknown


def plot_metric(records, metric, ylabel, title, output_path):
    figure, axis = plt.subplots(figsize=(8.5, 5.2), dpi=150)
    metric_values = [record[metric] for record in records]
    for pattern in ordered_patterns(records):
        points = [record for record in records
                  if record["pattern"] == pattern]
        points.sort(key=lambda record: record["hit_cfg_pct"])
        style = PATTERN_STYLES.get(
            pattern,
            {"label": pattern, "linestyle": "-", "marker": "o"},
        )
        axis.plot(
            [record["hit_cfg_pct"] for record in points],
            [record[metric] for record in points],
            label=style["label"],
            linestyle=style["linestyle"],
            marker=style["marker"],
            linewidth=2.0,
            markersize=6,
        )

    axis.set_xlabel("Configured L2 hit rate (%)")
    axis.set_ylabel(ylabel)
    axis.set_title(title)
    axis.set_xticks(sorted({record["hit_cfg_pct"] for record in records}))
    if min(metric_values) == 0 and max(metric_values) > 0:
        axis.set_ylim(bottom=-max(metric_values) * 0.03)
        axis.set_yticks([tick for tick in axis.get_yticks() if tick >= 0])
    else:
        axis.set_ylim(bottom=0)
    axis.grid(True, linestyle="--", alpha=0.35)
    axis.legend()
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def generate_artifacts(records, output_dir):
    validate_records(records)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    csv_path = output_dir / "l2_sweep_summary.csv"
    e2e_path = output_dir / "l2_hit_rate_vs_e2e.png"
    hbm_path = output_dir / "l2_hit_rate_vs_hbm.png"
    p99_path = output_dir / "l2_hit_rate_vs_p99.png"

    write_summary_csv(records, csv_path)
    plot_metric(
        records,
        "e2e_bpc",
        "Effective bandwidth (B/cycle)",
        "L2 Hit Rate vs End-to-End Effective Bandwidth",
        e2e_path,
    )
    plot_metric(
        records,
        "hbm_bpc",
        "HBM physical bandwidth (B/cycle)",
        "L2 Hit Rate vs HBM Physical Bandwidth",
        hbm_path,
    )
    plot_metric(
        records,
        "p99",
        "P99 latency (cycles)",
        "L2 Hit Rate vs P99 Latency",
        p99_path,
    )
    return [csv_path, e2e_path, hbm_path, p99_path]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate CSV and charts from L2_SWEEP gtest output.")
    parser.add_argument("input", type=Path, help="gtest full or filtered log")
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        help="output directory (default: <input-stem>_charts beside input)",
    )
    args = parser.parse_args(argv)
    output_dir = args.output_dir
    if output_dir is None:
        output_dir = args.input.parent / (args.input.stem + "_charts")

    try:
        records = load_records(args.input)
        outputs = generate_artifacts(records, output_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print("Parsed {} L2_SWEEP records.".format(len(records)))
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
