import csv
from pathlib import Path


FIELDS = (
    "kind",
    "case",
    "pattern",
    "osd_cfg",
    "osd_peak_min",
    "osd_peak_max",
    "burst_len",
    "request_bytes",
    "e2e_bpc",
    "dat_bpc",
    "hbm_bpc",
    "req_packets",
    "dat_packets",
    "dat_ser_eff_pct",
    "credit_stalls",
    "queue_full_stalls",
    "hbm_osd_peak",
    "p99",
    "status",
)
OPTIONAL_FIELDS = {"pattern"}
INTEGER_FIELDS = {
    "osd_cfg",
    "osd_peak_min",
    "osd_peak_max",
    "burst_len",
    "request_bytes",
    "req_packets",
    "dat_packets",
    "credit_stalls",
    "queue_full_stalls",
    "hbm_osd_peak",
    "p99",
}
FLOAT_FIELDS = {"e2e_bpc", "dat_bpc", "hbm_bpc", "dat_ser_eff_pct"}


def parse_perf_sweep_lines(lines, expected_kind, sort_field):
    records = []
    for line_number, line in enumerate(lines, start=1):
        stripped = line.strip()
        if not stripped.startswith("PERF_SWEEP "):
            continue

        values = {}
        for token in stripped.split()[1:]:
            if "=" not in token:
                raise ValueError(
                    "line {} contains an invalid token: {}".format(
                        line_number, token))
            key, value = token.split("=", 1)
            values[key] = value

        if values.get("kind") != expected_kind:
            continue

        missing = [field for field in FIELDS
                   if field not in OPTIONAL_FIELDS and field not in values]
        if missing:
            raise ValueError(
                "line {} is missing fields: {}".format(
                    line_number, ", ".join(missing)))

        record = {field: values.get(field) for field in FIELDS}
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

    records.sort(key=lambda record: record[sort_field])
    return records


def validate_records(records, expected_kind, unique_field,
                     fixed_fields=()):
    if not records:
        raise ValueError(
            "input contains no PERF_SWEEP kind={} records".format(
                expected_kind))

    unique_fields = ((unique_field,) if isinstance(unique_field, str)
                     else tuple(unique_field))
    seen = set()
    for record in records:
        if record["kind"] != expected_kind:
            raise ValueError("unexpected sweep kind: {}".format(
                record["kind"]))
        if record["status"] != "PASS":
            raise ValueError("case {} did not pass".format(record["case"]))
        point = tuple(record[field] for field in unique_fields)
        if point in seen:
            raise ValueError("duplicate {} point: {}".format(
                "/".join(unique_fields), point))
        seen.add(point)

    for field in fixed_fields:
        values = {record[field] for record in records}
        if len(values) != 1:
            raise ValueError("all records must use the same {}".format(field))


def write_summary_csv(records, output_path):
    with Path(output_path).open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(records)


def set_nonnegative_y(axis, values):
    maximum = max(values)
    minimum = min(values)
    if minimum > 0:
        axis.set_ylim(bottom=0)
    elif maximum > 0:
        axis.set_ylim(bottom=-maximum * 0.03)
        axis.set_yticks([tick for tick in axis.get_yticks() if tick >= 0])
    else:
        axis.set_ylim(-0.05, 1.0)


def configure_grid(axis):
    axis.grid(True, linestyle="--", alpha=0.35)
