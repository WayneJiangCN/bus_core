#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


FIELDS = (
    "scenario",
    "e2e_bpc",
    "p99",
    "backend_reads",
    "backend_read_saved",
    "write_hazard_stalls",
    "h_carriers",
    "h_carrier_recipients",
    "status",
)

INTEGER_FIELDS = {
    "p99",
    "backend_reads",
    "backend_read_saved",
    "write_hazard_stalls",
    "h_carriers",
    "h_carrier_recipients",
}
FLOAT_FIELDS = {"e2e_bpc"}
SCENARIO_ORDER = (
    "nomerge_read",
    "shared_read",
    "scatter_read",
    "private_read_write",
    "same_address_read_write",
)
SCENARIO_LABELS = {
    "nomerge_read": "No-merge\nread",
    "shared_read": "Shared\nread",
    "scatter_read": "Scatter\nread",
    "private_read_write": "Private\nread/write",
    "same_address_read_write": "Same-address\nread/write",
}


def parse_same_address_sweep_lines(lines):
    records = []
    for line_number, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not stripped.startswith("SAME_ADDRESS_CORE "):
            continue

        values = {}
        for token in stripped.split()[1:]:
            if "=" not in token:
                raise ValueError(
                    "line {} contains an invalid token: {}".format(
                        line_number, token))
            key, value = token.split("=", 1)
            values[key] = value

        missing = [field for field in FIELDS if field not in values]
        if missing:
            raise ValueError(
                "line {} is missing fields: {}".format(
                    line_number, ", ".join(missing)))

        record = {field: values[field] for field in FIELDS}
        try:
            for field in INTEGER_FIELDS:
                record[field] = int(record[field])
            for field in FLOAT_FIELDS:
                record[field] = float(record[field])
        except ValueError as error:
            raise ValueError(
                "line {} contains a non-numeric value".format(line_number)
            ) from error
        records.append(record)

    rank = {name: index for index, name in enumerate(SCENARIO_ORDER)}
    records.sort(key=lambda record: (
        rank.get(record["scenario"], len(rank)), record["scenario"]))
    return records


def load_records(input_path):
    with Path(input_path).open(encoding="utf-8-sig") as input_file:
        return parse_same_address_sweep_lines(input_file)


def validate_records(records):
    if not records:
        raise ValueError("input contains no SAME_ADDRESS_CORE records")

    seen = set()
    for record in records:
        if record["status"] != "PASS":
            raise ValueError(
                "scenario {} did not pass".format(record["scenario"]))
        scenario = record["scenario"]
        if scenario in seen:
            raise ValueError("duplicate scenario: {}".format(scenario))
        seen.add(scenario)

def _labels(records):
    return [SCENARIO_LABELS.get(record["scenario"], record["scenario"])
            for record in records]


def _configure_bar_axis(axis, labels, ylabel):
    axis.set_xticks(range(len(labels)))
    axis.set_xticklabels(labels)
    axis.set_ylabel(ylabel)
    axis.set_ylim(bottom=0)
    axis.grid(True, axis="y", linestyle="--", alpha=0.35)


def plot_bandwidth_latency(records, output_path):
    labels = _labels(records)
    figure, axes = plt.subplots(2, 1, figsize=(9.0, 8.0), dpi=150)
    axes[0].bar(range(len(records)),
                [record["e2e_bpc"] for record in records], color="#4472C4")
    _configure_bar_axis(axes[0], labels, "Effective bandwidth (B/cycle)")
    axes[0].set_title("Same-address Access: Bandwidth and Tail Latency")
    axes[1].bar(range(len(records)),
                [record["p99"] for record in records], color="#ED7D31")
    _configure_bar_axis(axes[1], labels, "P99 latency (cycles)")
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def plot_backend_traffic(records, output_path):
    labels = _labels(records)
    positions = list(range(len(records)))
    width = 0.38
    figure, axis = plt.subplots(figsize=(9.0, 5.4), dpi=150)
    axis.bar([position - width / 2 for position in positions],
             [record["backend_reads"] for record in records], width,
             label="Backend reads", color="#4472C4")
    axis.bar([position + width / 2 for position in positions],
             [record["backend_read_saved"] for record in records], width,
             label="Backend reads saved", color="#70AD47")
    _configure_bar_axis(axis, labels, "Transaction count")
    axis.set_title("Same-address Access: Backend Read Traffic")
    axis.legend()
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def plot_fanout_conflict(records, output_path):
    labels = _labels(records)
    reuse = [
        (record["h_carrier_recipients"] / record["h_carriers"]
         if record["h_carriers"] else 0.0)
        for record in records
    ]
    figure, axes = plt.subplots(2, 1, figsize=(9.0, 8.0), dpi=150)
    axes[0].bar(range(len(records)), reuse, color="#70AD47")
    _configure_bar_axis(axes[0], labels, "Recipients per H carrier")
    axes[0].set_title("Same-address Access: Fanout and Write Conflict")
    axes[1].bar(range(len(records)),
                [record["write_hazard_stalls"] for record in records],
                color="#C55A11")
    _configure_bar_axis(axes[1], labels, "Write-hazard stall cycles")
    figure.tight_layout()
    figure.savefig(str(output_path))
    plt.close(figure)


def generate_artifacts(records, output_dir):
    validate_records(records)
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    csv_path = output_dir / "same_address_core_summary.csv"
    bandwidth_path = output_dir / "same_address_bandwidth_latency.png"
    backend_path = output_dir / "same_address_backend_traffic.png"
    conflict_path = output_dir / "same_address_fanout_conflict.png"
    with csv_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(records)
    plot_bandwidth_latency(records, bandwidth_path)
    plot_backend_traffic(records, backend_path)
    plot_fanout_conflict(records, conflict_path)
    return [csv_path, bandwidth_path, backend_path, conflict_path]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate CSV and charts from SAME_ADDRESS_CORE output.")
    parser.add_argument("input", type=Path, help="core result text")
    parser.add_argument(
        "-o", "--output-dir", type=Path,
        help="output directory (default: <input-stem>_same_address_charts)")
    args = parser.parse_args(argv)
    output_dir = args.output_dir or (
        args.input.parent / (args.input.stem + "_same_address_charts"))

    try:
        records = load_records(args.input)
        outputs = generate_artifacts(records, output_dir)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print("Parsed {} SAME_ADDRESS_CORE records.".format(len(records)))
    for output in outputs:
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
