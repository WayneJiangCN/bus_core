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


def parse_outstanding_sweep_lines(lines):
    return parse_perf_sweep_lines(lines, "outstanding", "osd_cfg")


def load_outstanding_records(input_path):
    with Path(input_path).open(encoding="utf-8-sig") as input_file:
        return parse_outstanding_sweep_lines(input_file)


def validate_outstanding_records(records):
    validate_records(
        records,
        "outstanding",
        "osd_cfg",
        fixed_fields=("request_bytes", "burst_len"),
    )


def configure_osd_axis(axis, records):
    osd_values = [record["osd_cfg"] for record in records]
    try:
        axis.set_xscale("log", base=2)
    except TypeError:
        axis.set_xscale("log", basex=2)
    axis.set_xticks(osd_values)
    axis.set_xticklabels([str(value) for value in osd_values])
    axis.set_xlabel("Configured outstanding per master")
    configure_grid(axis)


def plot_bandwidth(records, output_path):
    figure, axis = plt.subplots(figsize=(8.5, 5.2), dpi=150)
    x_values = [record["osd_cfg"] for record in records]
    axis.plot(x_values, [record["e2e_bpc"] for record in records],
              marker="o", linewidth=2.0, label="E2E effective")
    axis.plot(x_values, [record["hbm_bpc"] for record in records],
              marker="s", linewidth=2.0, label="HBM physical")
    configure_osd_axis(axis, records)
    axis.set_ylabel("Bandwidth (B/cycle)")
    axis.set_title("Outstanding vs Achieved Bandwidth")
    set_nonnegative_y(axis, [
        value
        for record in records
        for value in (record["e2e_bpc"], record["hbm_bpc"])
    ])
    axis.legend()
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def plot_p99(records, output_path):
    figure, axis = plt.subplots(figsize=(8.5, 5.2), dpi=150)
    axis.plot(
        [record["osd_cfg"] for record in records],
        [record["p99"] for record in records],
        marker="o",
        linewidth=2.0,
        color="tab:orange",
    )
    configure_osd_axis(axis, records)
    axis.set_ylabel("P99 latency (cycles)")
    axis.set_title("Outstanding vs P99 Latency")
    axis.set_ylim(bottom=0)
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def plot_resource_pressure(records, output_path):
    figure, stalls_axis = plt.subplots(figsize=(8.5, 5.2), dpi=150)
    osd_values = [record["osd_cfg"] for record in records]
    stalls_line = stalls_axis.plot(
        osd_values,
        [record["credit_stalls"] for record in records],
        marker="o",
        linewidth=2.0,
        color="tab:red",
        label="Credit stalls",
    )
    configure_osd_axis(stalls_axis, records)
    stalls_axis.set_ylabel("Credit stall events", color="tab:red")
    stalls_axis.tick_params(axis="y", labelcolor="tab:red")
    try:
        stalls_axis.set_yscale("symlog", linthreshy=100)
    except TypeError:
        stalls_axis.set_yscale("symlog", linthresh=100)
    max_credit_stalls = max(record["credit_stalls"] for record in records)
    stalls_axis.set_ylim(bottom=0, top=max(1, max_credit_stalls * 1.25))

    hbm_axis = stalls_axis.twinx()
    hbm_line = hbm_axis.plot(
        osd_values,
        [record["hbm_osd_peak"] for record in records],
        marker="s",
        linewidth=2.0,
        color="tab:blue",
        label="HBM outstanding peak",
    )
    hbm_axis.set_ylabel("HBM outstanding peak", color="tab:blue")
    hbm_axis.tick_params(axis="y", labelcolor="tab:blue")
    hbm_axis.set_ylim(bottom=0)
    stalls_axis.set_title("Outstanding vs Resource Pressure")
    lines = stalls_line + hbm_line
    stalls_axis.legend(lines, [line.get_label() for line in lines],
                       loc="upper left")
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def generate_artifacts(records, output_dir):
    validate_outstanding_records(records)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    csv_path = output_dir / "outstanding_sweep_summary.csv"
    bandwidth_path = output_dir / "outstanding_vs_bandwidth.png"
    p99_path = output_dir / "outstanding_vs_p99.png"
    pressure_path = output_dir / "outstanding_vs_resource_pressure.png"
    write_summary_csv(records, csv_path)
    plot_bandwidth(records, bandwidth_path)
    plot_p99(records, p99_path)
    plot_resource_pressure(records, pressure_path)
    return [csv_path, bandwidth_path, p99_path, pressure_path]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate CSV and charts from outstanding PERF_SWEEP output.")
    parser.add_argument("input", type=Path, help="gtest full or filtered log")
    parser.add_argument(
        "-o", "--output-dir", type=Path,
        help="output directory (default: <input-stem>_outstanding_charts)")
    args = parser.parse_args(argv)
    output_dir = args.output_dir or (
        args.input.parent / (args.input.stem + "_outstanding_charts"))

    try:
        records = load_outstanding_records(args.input)
        outputs = generate_artifacts(records, output_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print("Parsed {} outstanding PERF_SWEEP records.".format(len(records)))
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
