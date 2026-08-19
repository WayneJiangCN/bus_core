#!/usr/bin/env python3

import argparse
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

from perf_sweep_plot_common import (
    configure_grid,
    parse_perf_sweep_lines,
    set_nonnegative_y,
    validate_records,
    write_summary_csv,
)

PATTERN_ORDER = ("nomerge", "private", "scatter", "shared")
PATTERN_STYLES = {
    "nomerge": {"label": "NoMerge", "linestyle": "--", "marker": "o"},
    "private": {"label": "Private", "linestyle": "-", "marker": "s"},
    "scatter": {"label": "Scatter", "linestyle": "-.", "marker": "^"},
    "shared": {"label": "Shared", "linestyle": ":", "marker": "D"},
}


def parse_burst_sweep_lines(lines):
    records = parse_perf_sweep_lines(lines, "burst", "request_bytes")
    pattern_rank = {name: index for index, name in enumerate(PATTERN_ORDER)}
    for record in records:
        if not record["pattern"]:
            record["pattern"] = "private"
    records.sort(
        key=lambda record: (
            pattern_rank.get(record["pattern"], len(pattern_rank)),
            record["pattern"],
            record["request_bytes"],
        )
    )
    return records


def load_burst_records(input_path):
    with Path(input_path).open(encoding="utf-8-sig") as input_file:
        return parse_burst_sweep_lines(input_file)


def validate_burst_records(records):
    validate_records(
        records,
        "burst",
        ("pattern", "request_bytes"),
        fixed_fields=("osd_cfg",),
    )


def configure_request_axis(axis, records):
    request_sizes = sorted({record["request_bytes"] for record in records})
    axis.set_xticks(request_sizes)
    axis.set_xlabel("Request size (B)")
    configure_grid(axis)


def ordered_patterns(records):
    present = {record["pattern"] for record in records}
    known = [pattern for pattern in PATTERN_ORDER if pattern in present]
    return known + sorted(present.difference(PATTERN_ORDER))


def plot_pattern_metric(axis, records, metric, show_legend=True):
    for pattern in ordered_patterns(records):
        points = [record for record in records
                  if record["pattern"] == pattern]
        points.sort(key=lambda record: record["request_bytes"])
        style = PATTERN_STYLES.get(
            pattern,
            {"label": pattern, "linestyle": "-", "marker": "o"},
        )
        axis.plot(
            [record["request_bytes"] for record in points],
            [record[metric] for record in points],
            marker=style["marker"],
            linestyle=style["linestyle"],
            linewidth=2.0,
            label=style["label"],
        )
    if show_legend:
        axis.legend()


def plot_bandwidth(records, output_path):
    figure, axis = plt.subplots(figsize=(8.5, 5.2), dpi=150)
    plot_pattern_metric(axis, records, "e2e_bpc")
    configure_request_axis(axis, records)
    axis.set_ylabel("Effective bandwidth (B/cycle)")
    axis.set_title("Request Size / Burst vs Effective Bandwidth")
    set_nonnegative_y(axis, [record["e2e_bpc"] for record in records])
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def plot_p99(records, output_path):
    figure, axis = plt.subplots(figsize=(8.5, 5.2), dpi=150)
    plot_pattern_metric(axis, records, "p99")
    configure_request_axis(axis, records)
    axis.set_ylabel("P99 latency (cycles)")
    axis.set_title("Request Size / Burst vs P99 Latency")
    axis.set_ylim(bottom=0)
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def plot_packet_counts(records, output_path):
    figure, axes = plt.subplots(3, 1, figsize=(8.5, 10.0), dpi=150,
                               sharex=True)
    metrics = (
        ("req_packets", "REQ packet count"),
        ("dat_packets", "DAT packet count"),
        ("dat_ser_eff_pct", "DAT serialization efficiency (%)"),
    )
    for index, (metric, ylabel) in enumerate(metrics):
        axis = axes[index]
        plot_pattern_metric(axis, records, metric, show_legend=index == 0)
        configure_request_axis(axis, records)
        if index != len(metrics) - 1:
            axis.set_xlabel("")
        axis.set_ylabel(ylabel)
        axis.set_ylim(bottom=0)
    axes[0].set_title("Request Size / Burst vs Packet Traffic")
    axes[-1].set_ylim(0, 110)
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def generate_artifacts(records, output_dir):
    validate_burst_records(records)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    csv_path = output_dir / "burst_sweep_summary.csv"
    bandwidth_path = output_dir / "request_bytes_vs_bandwidth.png"
    p99_path = output_dir / "request_bytes_vs_p99.png"
    packet_path = output_dir / "request_bytes_vs_packet_counts.png"
    write_summary_csv(records, csv_path)
    plot_bandwidth(records, bandwidth_path)
    plot_p99(records, p99_path)
    plot_packet_counts(records, packet_path)
    return [csv_path, bandwidth_path, p99_path, packet_path]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate CSV and charts from burst PERF_SWEEP output.")
    parser.add_argument("input", type=Path, help="gtest full or filtered log")
    parser.add_argument(
        "-o", "--output-dir", type=Path,
        help="output directory (default: <input-stem>_burst_charts)")
    args = parser.parse_args(argv)
    output_dir = args.output_dir or (
        args.input.parent / (args.input.stem + "_burst_charts"))

    try:
        records = load_burst_records(args.input)
        outputs = generate_artifacts(records, output_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print("Parsed {} burst PERF_SWEEP records.".format(len(records)))
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
