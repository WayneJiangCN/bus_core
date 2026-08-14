#!/usr/bin/env python3
"""Generate one offline HTML comparison report from PERF_* result blocks."""

import argparse
import html as html_lib
import re
import sys
from pathlib import Path


_KEY_VALUE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
_REQUIRED_SECTIONS = (
    "CONFIG",
    "COUNTS",
    "BANDWIDTH",
    "LATENCY",
    "RING",
    "HOME_AGENT",
    "L2_BUFFER",
    "MEMORY",
    "THEORY",
    "RESULT",
)
_REQUIRED_STRING_FIELDS = {
    "CONFIG": ("case", "op", "pattern"),
    "THEORY": ("assumption",),
    "RESULT": ("status",),
}
_REQUIRED_NUMERIC_FIELDS = {
    "CONFIG": ("active_masters", "bytes_per_master", "burst_len"),
    "COUNTS": (
        "completed_packets",
        "completed_bytes",
        "protocol_errors",
        "drained",
    ),
    "BANDWIDTH": (
        "first_request",
        "first_response",
        "last_response",
        "transfer_cycles",
        "end_to_end_bpc",
        "steady_response_bpc",
        "scaling_efficiency",
        "jain_fairness",
    ),
    "LATENCY": ("p50", "p95", "p99", "max"),
    "RING": (
        "req_packets",
        "req_bytes",
        "req_busy_cycles",
        "req_stalls",
        "rsp_packets",
        "rsp_bytes",
        "rsp_busy_cycles",
        "rsp_stalls",
        "dat_packets",
        "dat_bytes",
        "dat_busy_cycles",
        "dat_stalls",
        "cross_station_injected",
        "cross_station_ejected",
    ),
    "HOME_AGENT": (
        "rd_requests",
        "backend_reads",
        "backend_read_saved",
        "l2_hits",
        "l2_misses",
        "write_hazard_stalls",
    ),
    "L2_BUFFER": (
        "responses_accepted",
        "h_carriers",
        "h_unicast_carriers",
        "h_multicast_carriers",
        "h_scatter_carriers",
        "h_carrier_recipients",
        "dat_bytes",
        "occupancy_peak",
        "buffer_full_stalls",
        "issue_interval_stalls",
        "dat_inject_stalls",
        "carrier_128b",
        "carrier_256b",
        "carrier_512b",
    ),
    "MEMORY": (
        "accepted_read_bytes",
        "accepted_write_bytes",
        "credit_stalls",
        "queue_full_stalls",
        "outstanding_peak",
    ),
    "THEORY": (
        "total_useful_bytes",
        "physical_packets",
        "fabric_min_cycles",
        "fabric_ceiling_bpc",
        "measured_over_fabric_ceiling",
    ),
    "RESULT": ("protocol_errors",),
}
_REPEATED_SECTIONS = (
    "HA_SOURCE",
    "RING_DOMAIN",
    "RBRG",
    "RING_BUFFER",
    "DEFLECTION",
    "HW_CHANNEL",
    "RING_EDGE",
    "RBRG_CHANNEL",
)


class PerfReportError(ValueError):
    """Raised when PERF_* input does not satisfy the report contract."""


def _parse_key_values(line):
    return {
        match.group(1): match.group(2) for match in _KEY_VALUE.finditer(line)
    }


def _parse_number(value):
    try:
        if any(marker in value.lower() for marker in (".", "e")):
            return float(value)
        return int(value)
    except (AttributeError, TypeError, ValueError):
        raise PerfReportError("invalid numeric value: {}".format(value))


def _validate_required_fields(sections, case_name):
    for section_name, keys in _REQUIRED_STRING_FIELDS.items():
        for key in keys:
            if not sections[section_name].get(key):
                raise PerfReportError(
                    "case {} is missing {}.{}".format(
                        case_name, section_name, key
                    )
                )

    for section_name, keys in _REQUIRED_NUMERIC_FIELDS.items():
        for key in keys:
            value = sections[section_name].get(key)
            if value is None:
                raise PerfReportError(
                    "case {} is missing {}.{}".format(
                        case_name, section_name, key
                    )
                )
            try:
                _parse_number(value)
            except PerfReportError:
                raise PerfReportError(
                    "case {} has invalid numeric field {}.{}: {}".format(
                        case_name, section_name, key, value
                    )
                )


class PerfScenario(object):
    def __init__(self, order, sections, raw_lines):
        self.order = order
        self.sections = sections
        self.raw_lines = list(raw_lines)
        self.case_name = sections["CONFIG"].get("case", "")
        if not self.case_name:
            raise PerfReportError("PERF_CONFIG is missing case")

    @property
    def raw_text(self):
        return "\n".join(self.raw_lines)

    def value(self, section, key, default=None):
        return self.sections.get(section, {}).get(key, default)

    def number(self, section, key):
        value = self.value(section, key)
        if value is None:
            raise PerfReportError(
                "case {} is missing {}.{}".format(
                    self.case_name, section, key
                )
            )
        return _parse_number(value)

    def records(self, section):
        return list(self.sections.get(section, ()))


def parse_perf_results(text):
    """Parse complete PERF_CONFIG ... PERF_RESULT scenarios from text."""
    scenarios = []
    seen_cases = set()
    sections = None
    raw_lines = None

    for source_line in text.splitlines():
        line = source_line.strip()
        if not line.startswith("PERF_"):
            continue

        record_name = line.split(None, 1)[0]
        section_name = record_name[len("PERF_") :]

        if section_name == "CONFIG":
            if sections is not None:
                active_case = sections.get("CONFIG", {}).get("case", "unknown")
                raise PerfReportError(
                    "unclosed scenario before new PERF_CONFIG: {}".format(
                        active_case
                    )
                )
            sections = {}
            raw_lines = []
        elif sections is None:
            raise PerfReportError(
                "{} appears outside a scenario".format(record_name)
            )

        if section_name in sections and section_name not in _REPEATED_SECTIONS:
            active_case = sections.get("CONFIG", {}).get("case", "unknown")
            raise PerfReportError(
                "duplicate section {} in case {}".format(
                    section_name, active_case
                )
            )

        record = _parse_key_values(line)
        if section_name in _REPEATED_SECTIONS:
            sections.setdefault(section_name, []).append(record)
        else:
            sections[section_name] = record
        raw_lines.append(line)

        if section_name != "RESULT":
            continue

        missing = [
            name for name in _REQUIRED_SECTIONS if name not in sections
        ]
        case_name = sections["CONFIG"].get("case", "")
        if missing:
            raise PerfReportError(
                "case {} is missing required sections: {}".format(
                    case_name or "unknown", ", ".join(missing)
                )
            )
        if case_name in seen_cases:
            raise PerfReportError("duplicate case name: {}".format(case_name))

        _validate_required_fields(sections, case_name or "unknown")

        scenario = PerfScenario(len(scenarios), sections, raw_lines)
        scenarios.append(scenario)
        seen_cases.add(scenario.case_name)
        sections = None
        raw_lines = None

    if sections is not None:
        active_case = sections.get("CONFIG", {}).get("case", "unknown")
        raise PerfReportError("unclosed scenario: {}".format(active_case))
    if not scenarios:
        raise PerfReportError("input contains no complete PERF scenarios")
    return scenarios


def _format_number(value):
    if isinstance(value, float):
        return "{:.3f}".format(value)
    return "{:,}".format(value)


def _scenario_passes(scenario):
    return (
        scenario.value("RESULT", "status") == "PASS"
        and scenario.number("COUNTS", "drained") == 1
        and scenario.number("COUNTS", "protocol_errors") == 0
        and scenario.number("RESULT", "protocol_errors") == 0
    )


def _fabric_efficiency(scenario):
    ceiling = scenario.number("THEORY", "fabric_ceiling_bpc")
    if ceiling <= 0:
        return 0.0
    return scenario.number("BANDWIDTH", "end_to_end_bpc") / ceiling


def _bar_width(value, maximum):
    if maximum <= 0:
        return 0.0
    return max(0.0, min(100.0, 100.0 * value / maximum))


def _metric_card(label, value, note=""):
    return (
        '<div class="metric">'
        '<div class="metric-label">{}</div>'
        '<div class="metric-value">{}</div>'
        '<div class="metric-note">{}</div>'
        '</div>'
    ).format(
        html_lib.escape(str(label)),
        html_lib.escape(str(value)),
        html_lib.escape(str(note)),
    )


def _bar(label, value, maximum, css_class):
    return (
        '<div class="bar-line">'
        '<span class="bar-name">{}</span>'
        '<span class="bar-track"><span class="bar-fill {}" '
        'style="width:{:.2f}%"></span></span>'
        '<span class="bar-value">{}</span>'
        '</div>'
    ).format(
        html_lib.escape(str(label)),
        css_class,
        _bar_width(value, maximum),
        html_lib.escape(_format_number(value)),
    )


def _optional_number(scenario, section, key, default=0):
    value = scenario.value(section, key)
    return default if value is None else _parse_number(value)


def _run_mode(scenario):
    return scenario.value("CONFIG", "run_mode", "free_running")


def _theory_number(scenario, section, key):
    value = scenario.value(section, key)
    if value is None:
        return scenario.number("THEORY", key)
    return _parse_number(value)


def _scaling_efficiency_text(scenario):
    available = scenario.value("BANDWIDTH", "scaling_efficiency_available")
    if available is not None and _parse_number(available) == 0:
        return "N/A"
    return _format_number(scenario.number("BANDWIDTH", "scaling_efficiency"))


def _direction_bar(cw_cycles, ccw_cycles):
    total = cw_cycles + ccw_cycles
    cw_pct = 0.0 if total == 0 else 100.0 * cw_cycles / total
    ccw_pct = 0.0 if total == 0 else 100.0 * ccw_cycles / total
    return (
        '<div class="direction-load">'
        '<div class="direction-track" aria-label="CW {:.1f}%, CCW {:.1f}%">'
        '<span class="direction-cw" style="width:{:.2f}%"></span>'
        '<span class="direction-ccw" style="width:{:.2f}%"></span></div>'
        '<div class="direction-values"><span>CW {} ({:.1f}%)</span>'
        '<span>CCW {} ({:.1f}%)</span></div></div>'
    ).format(
        cw_pct,
        ccw_pct,
        cw_pct,
        ccw_pct,
        _format_number(cw_cycles),
        cw_pct,
        _format_number(ccw_cycles),
        ccw_pct,
    )


def _profile_stat(label, value, note):
    return (
        '<div class="profile-stat"><span>{}</span><strong>{}</strong>'
        '<small>{}</small></div>'
    ).format(
        html_lib.escape(str(label)),
        html_lib.escape(str(value)),
        html_lib.escape(str(note)),
    )


def _record_number(record, key):
    return _parse_number(record[key])


def _multiring_profile(scenario):
    domains = scenario.records("RING_DOMAIN")
    rbrgs = scenario.records("RBRG")
    fanout = scenario.sections.get("FANOUT_CROSS_RING")
    if not domains or not rbrgs or fanout is None:
        return '<div class="empty">此日志中不可用。</div>'

    busy_keys = (
        "req_cw_busy_cycles",
        "req_ccw_busy_cycles",
        "rsp_cw_busy_cycles",
        "rsp_ccw_busy_cycles",
        "dat_cw_busy_cycles",
        "dat_ccw_busy_cycles",
    )
    maximum_busy = max(
        _record_number(domain, key) for domain in domains for key in busy_keys
    )
    domain_rows = []
    hottest_rows = []
    for domain in domains:
        label = "{}{}".format(domain["type"].upper(), domain["id"])
        domain_rows.append(
            '<article class="chart-group"><div class="group-heading"><strong>{}</strong>'
            '<span class="subtle">REQ / RSP / DAT</span></div>{}</article>'.format(
                html_lib.escape(label),
                "".join(
                    _bar(
                        key.replace("_busy_cycles", "").upper(),
                        _record_number(domain, key),
                        maximum_busy,
                        "bar-teal" if "cw_" in key else "bar-blue",
                    )
                    for key in busy_keys
                ),
            )
        )
        hottest_rows.append(
            '<div class="profile-kv"><span>{} 最热连接</span><strong>{} {} -&gt; {} {} / {} busy cycles / {} stalls</strong></div>'.format(
                html_lib.escape(label),
                html_lib.escape(domain["hottest_subnet"]),
                html_lib.escape(domain["hottest_src_station"]),
                html_lib.escape(domain["hottest_dst_station"]),
                html_lib.escape(domain["hottest_dst_direction"]),
                _format_number(_record_number(domain, "hottest_busy_cycles")),
                _format_number(_record_number(domain, "hottest_stalls")),
            )
        )

    path_names = ("v_to_h_req", "v_to_h_dat", "h_to_v_rsp", "h_to_v_dat")
    rbrg_rows = []
    for rbrg in rbrgs:
        path_rows = "".join(
            '<div class="profile-kv"><span>{}</span><strong>{} bytes / peak {} / full {} / inject {}</strong></div>'.format(
                path,
                _format_number(_record_number(rbrg, path + "_bytes")),
                _format_number(_record_number(rbrg, path + "_queue_peak")),
                _format_number(_record_number(rbrg, path + "_queue_full_stalls")),
                _format_number(_record_number(rbrg, path + "_inject_stalls")),
            )
            for path in path_names
        )
        rbrg_rows.append(
            '<article class="chart-group"><div class="group-heading"><strong>RBRG {} 四路径</strong></div>'
            '<div class="profile-kv-list">{}</div></article>'.format(
                html_lib.escape(rbrg["id"]), path_rows
            )
        )

    fanout_rows = (
        ("逻辑接收者", "logical_recipients"),
        ("H-Ring 载体（L2）", "h_ring_carriers"),
        ("V-Ring 载体（RBRG H_TO_V_DAT）", "v_ring_carriers"),
        ("H-Ring 段包节省", "h_ring_saved_packets"),
        ("V-Ring 段包节省", "v_ring_saved_packets"),
        ("段包节省", "total_segment_packets_saved"),
    )
    fanout_body = "".join(
        '<div class="profile-kv"><span>{}</span><strong>{}</strong></div>'.format(
            html_lib.escape(label),
            _format_number(_record_number(fanout, key)),
        )
        for label, key in fanout_rows
    )
    return (
        '<div class="profile-grid">'
        '<div class="profile-pane"><h3>H-Ring 与 V-Ring 双向 busy cycles</h3>{}</div>'
        '<div class="profile-pane"><h3>每 Ring 最热连接</h3><div class="profile-kv-list">{}</div></div>'
        '<div class="profile-pane"><h3>跨 Ring 扇出</h3><div class="profile-kv-list">{}</div></div>'
        '</div><div class="profile-pane"><h3>RBRG 路径字节与 stalls</h3>{}</div>'
    ).format(
        "".join(domain_rows),
        "".join(hottest_rows),
        fanout_body,
        "".join(rbrg_rows),
    )


def _no_activity():
    return '<div class="empty">No activity（无活动）。</div>'


def _ring_groups(records):
    groups = {}
    for record in records:
        key = (record.get("domain", "?"), record.get("ring", "?"))
        groups.setdefault(key, []).append(record)
    return groups


def _utilization_bar(value, css_class):
    return _bar("", value, 100.0, css_class)


_FIXED_TOPOLOGY = {
    "h0": {
        "domain": "h",
        "ring": "0",
        "nodes": (
            ("rbrg", 0, 0, 220, 310),
            ("home_agent", 0, 1, 380, 220),
            ("l2_buffer", 0, 2, 540, 220),
            ("l2_buffer", 1, 3, 700, 220),
            ("home_agent", 1, 4, 860, 220),
            ("rbrg", 1, 5, 980, 310),
            ("home_agent", 2, 6, 860, 400),
            ("l2_buffer", 2, 7, 700, 400),
            ("l2_buffer", 3, 8, 540, 400),
            ("home_agent", 3, 9, 380, 400),
        ),
    },
    "v0": {
        "domain": "v",
        "ring": "0",
        "nodes": (
            ("rbrg", 0, 0, 220, 310),
            ("master", 0, 1, 90, 130),
            ("master", 1, 2, 45, 230),
            ("master", 2, 3, 45, 390),
            ("master", 3, 4, 90, 490),
        ),
    },
    "v1": {
        "domain": "v",
        "ring": "1",
        "nodes": (
            ("rbrg", 1, 0, 980, 310),
            ("master", 4, 1, 1110, 130),
            ("master", 5, 2, 1155, 230),
            ("master", 6, 3, 1155, 390),
            ("master", 7, 4, 1110, 490),
        ),
    },
}


def _fixed_node_label(node_type, node):
    if node_type == "master":
        name = "M{}".format(node)
    elif node_type == "home_agent":
        name = "HA{}".format(node)
    elif node_type == "l2_buffer":
        name = "L2_{}".format(node)
    else:
        name = "RBRG{}".format(node)
    return name


def _supports_fixed_topology(scenario):
    if (
        scenario.number("CONFIG", "active_masters") != 8
        or _optional_number(scenario, "CONFIG", "max_aicore_per_vring") != 4
    ):
        return False

    expected_nodes = {
        "master": set(range(8)),
        "home_agent": set(range(4)),
        "l2_buffer": set(range(4)),
    }
    observed_nodes = {node_type: set() for node_type in expected_nodes}
    for record in scenario.records("RING_BUFFER"):
        node_type = record.get("node_type")
        if node_type in observed_nodes:
            observed_nodes[node_type].add(_record_number(record, "node"))
    if observed_nodes != expected_nodes:
        return False

    expected_rings = {("h", "0"), ("v", "0"), ("v", "1")}
    observed_rings = {
        (record.get("domain"), record.get("ring"))
        for record in scenario.records("HW_CHANNEL")
    }
    if observed_rings != expected_rings:
        return False

    observed_edge_rings = {
        (record.get("domain"), record.get("ring"))
        for record in scenario.records("RING_EDGE")
    }
    if observed_edge_rings != expected_rings:
        return False

    rbrg_ids = {
        record.get("rbrg", record.get("id"))
        for record in scenario.records("RBRG_CHANNEL")
    }
    return rbrg_ids == {"0", "1"}


def _topology_segment_path(source, target, offset):
    x1, y1 = source[3], source[4]
    x2, y2 = target[3], target[4]
    delta_x = x2 - x1
    delta_y = y2 - y1
    length = max((delta_x * delta_x + delta_y * delta_y) ** 0.5, 1.0)
    normal_x = -delta_y / length
    normal_y = delta_x / length
    return "M {:.1f} {:.1f} L {:.1f} {:.1f}".format(
        x1 + normal_x * offset,
        y1 + normal_y * offset,
        x2 + normal_x * offset,
        y2 + normal_y * offset,
    )


def _topology_edge_path(ring_key, index, station_count, direction):
    ring = _FIXED_TOPOLOGY[ring_key]
    source = ring["nodes"][index]
    target_index = (
        (index + 1) % station_count
        if direction == "cw"
        else (index + station_count - 1) % station_count
    )
    target = ring["nodes"][target_index]
    return _topology_segment_path(source, target, -3)


def _topology_node_label_position(ring_key, station, x, y):
    if ring_key == "h0":
        if station in (6, 7, 8, 9):
            return x, y + 30, "middle"
        return x, y - 20, "middle"
    if ring_key == "v0":
        return x + 20, y + 4, "start"
    return x - 20, y + 4, "end"


def _topology_edge_record(scenario, ring_key, subnet, direction, source, target):
    ring = _FIXED_TOPOLOGY[ring_key]
    for record in scenario.records("RING_EDGE"):
        if (
            record.get("domain") == ring["domain"]
            and record.get("ring") == ring["ring"]
            and record.get("subnet") == subnet
            and record.get("direction") == direction
            and _record_number(record, "src_station") == source
            and _record_number(record, "dst_station") == target
        ):
            return record
    return None


def _topology_edge_classes(record):
    if record is None:
        return "topology-edge-idle topology-edge-thin"

    cycle_utilization = _record_number(record, "cycle_util_pct")
    payload_utilization = _record_number(record, "payload_util_pct")
    if cycle_utilization >= 70:
        heat = "hot"
    elif cycle_utilization >= 40:
        heat = "warm"
    elif cycle_utilization > 0:
        heat = "low"
    else:
        heat = "idle"

    if payload_utilization >= 60:
        width = "wide"
    elif payload_utilization >= 25:
        width = "medium"
    else:
        width = "thin"
    return "topology-edge-{} topology-edge-{}".format(heat, width)


def _topology_edge_detail(edge_id, ring_key, subnet, direction, source, target, record):
    if record is None:
        body = '<div class="topology-detail-empty">该物理边没有活动记录。</div>'
    else:
        rows = (
            ("Packet", _record_number(record, "packets")),
            ("Bytes", _record_number(record, "bytes")),
            ("Busy cycles", _record_number(record, "busy_cycles")),
            ("Stalls", _record_number(record, "stalls")),
            ("Cycle utilization", _record_number(record, "cycle_util_pct")),
            ("Payload utilization", _record_number(record, "payload_util_pct")),
            (
                "Serialization efficiency",
                _record_number(record, "serialization_efficiency_pct"),
            ),
        )
        body = "".join(
            '<div class="topology-detail-row"><span>{}</span><strong>{}</strong></div>'.format(
                label, _format_number(value)
            )
            for label, value in rows
        )
    return (
        '<div class="topology-detail" data-topology-detail="edge-{}" hidden>'
        '<strong>{} · {} · {} · st{} → st{}</strong>{}</div>'
    ).format(
        edge_id,
        ring_key.upper(),
        subnet.upper(),
        direction.upper(),
        source,
        target,
        body,
    )


def _topology_node_details(scenario):
    groups = {}
    for record in scenario.records("RING_BUFFER"):
        node_type = record.get("node_type")
        node = record.get("node")
        if node_type is None or node is None:
            continue
        groups.setdefault("{}-{}".format(node_type, node), []).append(record)

    sources_by_ha = {}
    for record in scenario.records("HA_SOURCE"):
        ha = record.get("ha")
        if ha is None:
            continue
        sources_by_ha.setdefault(ha, []).append(record)
        groups.setdefault("home_agent-{}".format(ha), [])

    details = []
    for node_id, records in sorted(groups.items()):
        rows = []
        for record in records:
            rows.append(
                '<tr><td>{} {}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>'.format(
                    html_lib.escape(record["subnet"].upper()),
                    html_lib.escape(record["side"]),
                    html_lib.escape(record["direction"]),
                    _format_number(_record_number(record, "peak")),
                    _format_number(_record_number(record, "full_pct")),
                    _format_number(_record_number(record, "pushes")),
                    _format_number(_record_number(record, "push_rejects")),
                )
            )
        source_table = ""
        if node_id.startswith("home_agent-"):
            ha = node_id.split("-", 1)[1]
            source_records = sources_by_ha.get(ha, [])
            source_total = sum(
                _record_number(record, "total_packets")
                for record in source_records
            )
            source_rows = []
            for record in sorted(
                source_records,
                key=lambda item: _record_number(item, "master"),
            ):
                total = _record_number(record, "total_packets")
                share = 0.0 if source_total == 0 else 100.0 * total / source_total
                source_rows.append(
                    '<tr><td>M{}</td><td>{}</td><td>{}</td><td>{}</td><td>{:.1f}</td></tr>'.format(
                        html_lib.escape(record["master"]),
                        _format_number(_record_number(record, "rd_packets")),
                        _format_number(_record_number(record, "wr_packets")),
                        _format_number(total),
                        share,
                    )
                )
            if source_rows:
                source_table = (
                    '<strong>REQ 来源 Master</strong><div class="table-wrap"><table>'
                    '<thead><tr><th>Master</th><th>RD</th><th>WR</th>'
                    '<th>Total</th><th>Share (%)</th></tr></thead><tbody>{}</tbody>'
                    '</table></div>'
                ).format("".join(source_rows))

        details.append(
            '<div class="topology-detail" data-node-detail="{}" '
            'data-topology-detail="node-{}" hidden><strong>{}</strong>'
            '<div class="table-wrap"><table><thead><tr><th>Queue</th>'
            '<th>Direction</th><th>Peak</th><th>Full (%)</th>'
            '<th>Pushes</th><th>Push rejects</th></tr></thead><tbody>{}</tbody></table></div>{}</div>'.format(
                html_lib.escape(node_id),
                html_lib.escape(node_id),
                html_lib.escape(node_id),
                "".join(rows),
                source_table,
            )
        )
    return "".join(details)


def _topology_rbrg_details(scenario):
    details = []
    for record in scenario.records("RBRG_CHANNEL"):
        rbrg_id = record.get("rbrg", record.get("id", "?"))
        rows = (
            ("Path", record.get("path", "-")),
            ("Cycle utilization", _record_number(record, "cycle_util_pct")),
            ("Payload utilization", _record_number(record, "payload_util_pct")),
            ("Queue peak", _record_number(record, "queue_peak")),
            ("Queue full stalls", _record_number(record, "queue_full_stalls")),
        )
        body = "".join(
            '<div class="topology-detail-row"><span>{}</span><strong>{}</strong></div>'.format(
                label, value if isinstance(value, str) else _format_number(value)
            )
            for label, value in rows
        )
        details.append(
            '<div class="topology-detail" data-node-detail="rbrg-{}" '
            'data-topology-detail="node-rbrg-{}" hidden><strong>RBRG{}</strong>{}</div>'.format(
                html_lib.escape(rbrg_id),
                html_lib.escape(rbrg_id),
                html_lib.escape(rbrg_id),
                body,
            )
        )
    return "".join(details)


def _topology_path_summary(scenario):
    rbrg_records = scenario.records("RBRG_CHANNEL")
    rbrg = max(
        rbrg_records,
        key=lambda record: _record_number(record, "cycle_util_pct"),
    )
    buffer = max(
        scenario.records("RING_BUFFER"),
        key=lambda record: _record_number(record, "full_pct"),
    )
    edge = max(
        scenario.records("RING_EDGE"),
        key=lambda record: _record_number(record, "cycle_util_pct"),
    )
    rbrg_id = rbrg.get("rbrg", rbrg.get("id", "?"))
    rbrg_path = rbrg["path"]
    return (
        '<div class="topology-path-summary">'
        '<div><span>最热物理边</span><strong>{}{} · {} · {}%</strong></div>'
        '<div><span>最满 endpoint Buffer</span><strong>{} {} · {}%</strong></div>'
        '<div data-rbrg-path="{}-{}"><span>最忙 RBRG</span>'
        '<strong>RBRG{} · {} · {}%</strong></div></div>'.format(
            edge["domain"].upper(),
            edge["ring"],
            edge["subnet"].upper(),
            _format_number(_record_number(edge, "cycle_util_pct")),
            html_lib.escape(buffer["node_type"]),
            html_lib.escape(buffer["node"]),
            _format_number(_record_number(buffer, "full_pct")),
            html_lib.escape(rbrg_id),
            html_lib.escape(rbrg_path),
            html_lib.escape(rbrg_id),
            html_lib.escape(rbrg_path),
            _format_number(_record_number(rbrg, "cycle_util_pct")),
        )
    )


def _render_topology_overview(scenario, profile_index):
    if not _supports_fixed_topology(scenario):
        return (
            '<div class="topology-unavailable">当前拓扑不受支持：此视图要求 '
            '8 Master、4 HA/L2、2 V-Ring 和 2 RBRG 的完整 PERF 记录。</div>'
        )

    skeletons = []
    edges = []
    edge_details = []
    nodes = []
    private_links = []
    for ring_key, ring in _FIXED_TOPOLOGY.items():
        station_count = len(ring["nodes"])
        for index, source in enumerate(ring["nodes"]):
            target = ring["nodes"][(index + 1) % station_count]
            skeletons.append(
                '<path class="topology-skeleton" data-ring="{}" d="{}" />'.format(
                    ring_key, _topology_segment_path(source, target, 0)
                )
            )
        for subnet in ("req", "rsp", "dat"):
            for direction in ("cw", "ccw"):
                for index, source in enumerate(ring["nodes"]):
                    target = ring["nodes"][(index + 1) % station_count]
                    if direction == "ccw":
                        target = ring["nodes"][(index + station_count - 1) % station_count]
                    record = _topology_edge_record(
                        scenario,
                        ring_key,
                        subnet,
                        direction,
                        source[2],
                        target[2],
                    )
                    edge_id = "{}-{}-{}-{}-{}".format(
                        ring_key, subnet, direction, source[2], target[2]
                    )
                    edges.append(
                        '<path class="topology-edge topology-edge-{} {}" '
                        'data-ring="{}" data-subnet="{}" '
                        'data-direction="{}" data-edge="{}" '
                        'data-detail-target="edge-{}" '
                        'd="{}"{} />'.format(
                            subnet,
                            _topology_edge_classes(record),
                            ring_key,
                            subnet,
                            direction,
                            edge_id,
                            edge_id,
                            _topology_edge_path(
                                ring_key, index, station_count, direction
                            ),
                            "" if record is not None else ' data-missing="true"',
                        )
                    )
                    edge_details.append(
                        _topology_edge_detail(
                            edge_id,
                            ring_key,
                            subnet,
                            direction,
                            source[2],
                            target[2],
                            record,
                        )
                    )
        for node_type, node, station, x, y in ring["nodes"]:
            if node_type == "rbrg" and ring_key != "h0":
                continue
            node_id = "{}-{}".format(node_type, node)
            label_x, label_y, text_anchor = _topology_node_label_position(
                ring_key, station, x, y
            )
            label = _fixed_node_label(node_type, node)
            nodes.append(
                '<g class="topology-node topology-node-{}" data-node="{}" '
                'data-station="{}" data-detail-target="node-{}" tabindex="0" '
                'role="button" aria-label="{} · station {}">'
                '<circle cx="{}" cy="{}" r="12" />'
                '<text x="{}" y="{}" text-anchor="{}">{}</text></g>'.format(
                    node_type,
                    node_id,
                    station,
                    node_id,
                    html_lib.escape(label),
                    station,
                    x,
                    y,
                    label_x,
                    label_y,
                    text_anchor,
                    html_lib.escape(label),
                )
            )
        if ring_key == "h0":
            for index in (1, 3, 6, 8):
                source = ring["nodes"][index]
                target = ring["nodes"][index + 1]
                offset = 16 if source[4] < 310 else -16
                private_links.append(
                    '<line class="topology-private-link" x1="{}" y1="{}" '
                    'x2="{}" y2="{}" />'.format(
                        source[3], source[4] + offset, target[3], target[4] + offset
                    )
                )

    return (
        '<div class="topology-overview" data-topology-profile="{}" '
        'data-active-subnet="dat" data-active-ring="all">'
        '<div class="topology-heading"><div><strong>固定 Ring 拓扑</strong>'
        '<span>8 Master · 4 HA/L2 · 2 V-Ring</span></div>'
        '<div class="topology-subnets">'
        '<button class="topology-subnet" data-subnet="req">REQ</button>'
        '<button class="topology-subnet" data-subnet="rsp">RSP</button>'
        '<button class="topology-subnet active" data-subnet="dat">DAT</button>'
        '</div></div><div class="topology-rings">'
        '<button class="topology-ring active" data-ring="all">All</button>'
        '<button class="topology-ring" data-ring="h0">H0</button>'
        '<button class="topology-ring" data-ring="v0">V0</button>'
        '<button class="topology-ring" data-ring="v1">V1</button>'
        '</div><div class="topology-legend">'
        '<span><i class="topology-legend-hot"></i>颜色：cycle 利用率</span>'
        '<span><i class="topology-legend-wide"></i>线宽：payload 利用率</span>'
        '<span><i class="topology-legend-private"></i>HA-L2 私有接口</span>'
        '</div><div class="topology-canvas">'
        '<svg id="ring-topology-{}" viewBox="0 0 1200 620" '
        'role="img" aria-label="H0 与 V0/V1 Ring 拓扑">{}</svg>'
        '</div>{}<div class="topology-detail-panel">'
        '<div class="topology-detail active" data-topology-detail="summary">'
        '选择物理边或节点查看详细统计。</div>{}{}{}</div></div>'
    ).format(
        profile_index,
        profile_index,
        "".join(skeletons + edges + private_links + nodes),
        _topology_path_summary(scenario),
        "".join(edge_details),
        _topology_node_details(scenario),
        _topology_rbrg_details(scenario),
    )


def _physical_channel_view(scenario):
    groups = _ring_groups(scenario.records("HW_CHANNEL"))
    if not groups:
        return _no_activity()

    bodies = []
    for (domain, ring), records in groups.items():
        by_channel = {
            (record["subnet"], record["direction"]): record
            for record in records
        }
        rows = []
        for subnet in ("req", "rsp", "dat"):
            for direction in ("cw", "ccw"):
                record = by_channel.get((subnet, direction))
                if record is None:
                    cycle = '<span class="subtle">No activity</span>'
                    payload = '<span class="subtle">No activity</span>'
                else:
                    cycle = _utilization_bar(
                        _record_number(record, "cycle_util_pct"), "bar-teal"
                    )
                    payload = _utilization_bar(
                        _record_number(record, "payload_util_pct"), "bar-blue"
                    )
                rows.append(
                    '<tr><th>{}</th><td>{}</td><td>{}</td><td>{}</td></tr>'.format(
                        subnet.upper(), direction.upper(), cycle, payload
                    )
                )
        bodies.append(
            '<article class="chart-group"><div class="group-heading"><strong>{}{}</strong>'
            '<span class="subtle">REQ / RSP / DAT x CW / CCW</span></div>'
            '<div class="table-wrap"><table><thead><tr><th>Subnet</th>'
            '<th>Direction</th><th>cycle utilization (%)</th>'
            '<th>payload utilization (%)</th></tr></thead><tbody>{}</tbody>'
            '</table></div></article>'.format(
                html_lib.escape(domain.upper()),
                html_lib.escape(ring),
                "".join(rows),
            )
        )
    return "".join(bodies)


def _edge_hotspot_view(scenario):
    groups = _ring_groups(scenario.records("RING_EDGE"))
    if not groups:
        return _no_activity()

    bodies = []
    for (domain, ring), records in groups.items():
        rows = []
        for record in records:
            rows.append(
                (
                    '<tr><th>{}</th><td>{}</td><td>{}</td><td>{}</td>'
                    '<td>{}</td><td>{}</td></tr>'
                ).format(
                    html_lib.escape(record["subnet"].upper()),
                    html_lib.escape(record["direction"].upper()),
                    html_lib.escape(record["src_station"]),
                    html_lib.escape(record["dst_station"]),
                    _utilization_bar(
                        _record_number(record, "cycle_util_pct"), "bar-teal"
                    ),
                    _utilization_bar(
                        _record_number(record, "payload_util_pct"), "bar-blue"
                    ),
                )
            )
        bodies.append(
            '<article class="chart-group"><div class="group-heading"><strong>{}{}</strong>'
            '</div><div class="table-wrap"><table><thead><tr><th>Subnet</th>'
            '<th>Direction</th><th>Source</th><th>Destination</th>'
            '<th>cycle utilization (%)</th><th>payload utilization (%)</th>'
            '</tr></thead><tbody>{}</tbody></table></div></article>'.format(
                html_lib.escape(domain.upper()),
                html_lib.escape(ring),
                "".join(rows),
            )
        )
    return "".join(bodies)


def _endpoint_buffer_view(scenario):
    records = scenario.records("RING_BUFFER")
    if not records:
        return _no_activity()

    rows = []
    for record in records:
        rows.append(
            (
                '<tr><th>{} {}</th><td>{} {}</td><td>{}</td><td>{}</td>'
                '<td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>'
            ).format(
                html_lib.escape(record["node_type"]),
                html_lib.escape(record["node"]),
                html_lib.escape(record["subnet"].upper()),
                html_lib.escape(record["side"]),
                html_lib.escape(record["direction"]),
                _format_number(_record_number(record, "depth")),
                _format_number(_record_number(record, "peak")),
                _format_number(_record_number(record, "avg_occupancy_pct")),
                _format_number(_record_number(record, "full_cycles")),
                _format_number(_record_number(record, "full_pct")),
            )
        )
    return (
        '<div class="table-wrap"><table><thead><tr><th>Node</th><th>Queue</th>'
        '<th>Direction</th><th>Depth</th><th>Peak</th>'
        '<th>Average occupancy (%)</th><th>Full cycles</th><th>Full (%)</th>'
        '</tr></thead><tbody>{}</tbody></table></div>'
    ).format("".join(rows))


def _deflection_recovery_view(scenario):
    records = scenario.records("DEFLECTION")
    if not records:
        return _no_activity()

    rows = []
    for record in records:
        rows.append(
            (
                '<tr><th>{}{}</th><td>{}</td><td>{}</td><td>{}</td><td>{}</td>'
                '<td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>'
            ).format(
                html_lib.escape(record["domain"].upper()),
                html_lib.escape(record["ring"]),
                html_lib.escape(record["subnet"].upper()),
                _format_number(_record_number(record, "events")),
                _format_number(_record_number(record, "unique_packets")),
                _format_number(_record_number(record, "avg_rounds")),
                _format_number(_record_number(record, "max_rounds")),
                _format_number(_record_number(record, "avg_delay_cycles")),
                _format_number(_record_number(record, "max_delay_cycles")),
                _format_number(
                    _record_number(record, "fanout_recipient_retry_events")
                ),
            )
        )
    return (
        '<div class="table-wrap"><table><thead><tr><th>Ring</th><th>Subnet</th>'
        '<th>Events</th><th>Unique packets</th><th>Average rounds</th>'
        '<th>Max rounds</th><th>Average recovery delay</th>'
        '<th>Max recovery delay</th><th>Fanout-recipient retry events</th>'
        '</tr></thead><tbody>{}</tbody></table></div>'
    ).format("".join(rows))


def _rbrg_channel_view(scenario):
    records = scenario.records("RBRG_CHANNEL")
    if not records:
        return _no_activity()

    rows = []
    for record in records:
        rbrg_id = record.get("rbrg", record.get("id", "?"))
        rows.append(
            (
                '<tr><th>{}</th><td>{}</td><td>{}</td><td>{}</td>'
                '<td>{}</td><td>{}</td><td>{}</td></tr>'
            ).format(
                html_lib.escape(rbrg_id),
                html_lib.escape(record["path"]),
                _utilization_bar(
                    _record_number(record, "cycle_util_pct"), "bar-teal"
                ),
                _utilization_bar(
                    _record_number(record, "payload_util_pct"), "bar-blue"
                ),
                _format_number(_record_number(record, "queue_peak")),
                _format_number(_record_number(record, "queue_full_stalls")),
                _format_number(
                    _record_number(record, "destination_inject_stalls")
                ),
            )
        )
    return (
        '<div class="table-wrap"><table><thead><tr><th>RBRG</th><th>Path</th>'
        '<th>cycle utilization (%)</th><th>payload utilization (%)</th>'
        '<th>Queue peak</th><th>Queue full stalls</th>'
        '<th>Destination inject stalls</th></tr></thead><tbody>{}</tbody>'
        '</table></div>'
    ).format("".join(rows))


def _single_scenario_metric_views(scenario):
    return (
        '<details class="topology-data"><summary>详细 Ring 数据</summary>'
        '<div class="profile-grid">'
        '<div class="profile-pane"><h3>Physical Channel Utilization（物理通道利用率）</h3>{}</div>'
        '<div class="profile-pane"><h3>Deflection Recovery（绕圈恢复）</h3>{}</div>'
        '<div class="profile-pane"><h3>RBRG Channel（RBRG 路径）</h3>{}</div>'
        '</div><div class="profile-pane"><h3>Per-Edge Hotspots（逐边热点）</h3>{}</div>'
        '<div class="profile-pane"><h3>Endpoint Buffer</h3>{}</div></details>'
    ).format(
        _physical_channel_view(scenario),
        _deflection_recovery_view(scenario),
        _rbrg_channel_view(scenario),
        _edge_hotspot_view(scenario),
        _endpoint_buffer_view(scenario),
    )


def _scenario_profile_section(scenarios):
    options = []
    profiles = []
    direction_keys = (
        "req_cw_cycles",
        "req_ccw_cycles",
        "req_imbalance_pct",
        "dat_cw_cycles",
        "dat_ccw_cycles",
        "dat_imbalance_pct",
    )

    for index, scenario in enumerate(scenarios):
        options.append(
            '<option value="{}">{}</option>'.format(
                index, html_lib.escape(scenario.case_name)
            )
        )
        end_to_end = scenario.number("BANDWIDTH", "end_to_end_bpc")
        steady = scenario.number("BANDWIDTH", "steady_response_bpc")
        ceiling = scenario.number("THEORY", "fabric_ceiling_bpc")
        efficiency = 0.0 if ceiling <= 0 else 100.0 * end_to_end / ceiling

        profile_stats = "".join(
            (
                _profile_stat("端到端带宽", _format_number(end_to_end), "B/cycle"),
                _profile_stat("稳态响应", _format_number(steady), "B/cycle"),
                _profile_stat("模型效率", "{:.1f}%".format(efficiency), "实测/上限"),
                _profile_stat(
                    "P50 / P99",
                    "{} / {}".format(
                        _format_number(scenario.number("LATENCY", "p50")),
                        _format_number(scenario.number("LATENCY", "p99")),
                    ),
                    "cycles",
                ),
                _profile_stat(
                    "Jain 公平性",
                    _format_number(scenario.number("BANDWIDTH", "jain_fairness")),
                    "1.0 为完全均衡",
                ),
            )
        )

        if all(scenario.value("RING", key) is not None for key in direction_keys):
            req_cw = scenario.number("RING", "req_cw_cycles")
            req_ccw = scenario.number("RING", "req_ccw_cycles")
            dat_cw = scenario.number("RING", "dat_cw_cycles")
            dat_ccw = scenario.number("RING", "dat_ccw_cycles")
            direction_body = (
                '<div class="direction-row"><div><strong>REQ</strong>'
                '<span>失衡率 {:.1f}%</span></div>{}</div>'
                '<div class="direction-row"><div><strong>DAT</strong>'
                '<span>失衡率 {:.1f}%</span></div>{}</div>'
            ).format(
                scenario.number("RING", "req_imbalance_pct"),
                _direction_bar(req_cw, req_ccw),
                scenario.number("RING", "dat_imbalance_pct"),
                _direction_bar(dat_cw, dat_ccw),
            )
        else:
            direction_body = (
                '<div class="empty">该日志生成于方向统计加入前；重新运行场景后显示 CW/CCW。</div>'
            )

        legacy_hottest_keys = (
            "hottest_subnet",
            "hottest_src_station",
            "hottest_direction",
            "hottest_cycles",
        )
        if all(scenario.value("RING", key) is not None for key in legacy_hottest_keys):
            hottest_cycles = _optional_number(
                scenario,
                "RING",
                "hottest_edge_cycles",
                scenario.number("RING", "hottest_cycles"),
            )
            hottest = "{} / station {} / {} / {} cycles".format(
                scenario.value("RING", "hottest_subnet"),
                scenario.value("RING", "hottest_src_station"),
                scenario.value("RING", "hottest_direction"),
                _format_number(hottest_cycles),
            )
        elif scenario.records("HW_CHANNEL") and scenario.records("RING_EDGE"):
            hottest = (
                "当前日志请查看 Physical Channel Utilization / "
                "Per-Edge Hotspots。"
            )
        else:
            hottest = "该日志未提供最热边摘要。"

        stall_values = (
            (
                "Ring link",
                sum(
                    scenario.number("RING", "{}_stalls".format(subnet))
                    for subnet in ("req", "rsp", "dat")
                ),
                "bar-teal",
            ),
            (
                "HA 写冲突",
                scenario.number("HOME_AGENT", "write_hazard_stalls"),
                "bar-gray",
            ),
            (
                "L2 Buffer",
                sum(
                    scenario.number("L2_BUFFER", key)
                    for key in (
                        "buffer_full_stalls",
                        "issue_interval_stalls",
                        "dat_inject_stalls",
                    )
                ),
                "bar-amber",
            ),
            (
                "TmMem",
                scenario.number("MEMORY", "credit_stalls")
                + scenario.number("MEMORY", "queue_full_stalls"),
                "bar-red",
            ),
        )
        maximum_stall = max(value for _, value, _ in stall_values)
        stall_body = "".join(
            _bar(label, value, maximum_stall, css_class)
            for label, value, css_class in stall_values
        )

        traffic_rows = (
            ("完成有效字节", scenario.number("COUNTS", "completed_bytes")),
            ("后端读取字节", scenario.number("MEMORY", "accepted_read_bytes")),
            ("HA 节省后端读", scenario.number("HOME_AGENT", "backend_read_saved")),
            (
                "H-Ring 多播载体",
                scenario.number("L2_BUFFER", "h_multicast_carriers"),
            ),
            (
                "H-Ring 载体接收者",
                scenario.number("L2_BUFFER", "h_carrier_recipients"),
            ),
            ("L2 Buffer 峰值", scenario.number("L2_BUFFER", "occupancy_peak")),
            (
                "L2 其他尺寸 carrier",
                _optional_number(scenario, "L2_BUFFER", "carrier_other"),
            ),
            ("TmMem OSD 峰值", scenario.number("MEMORY", "outstanding_peak")),
        )
        traffic_body = "".join(
            '<div class="profile-kv"><span>{}</span><strong>{}</strong></div>'.format(
                html_lib.escape(label), _format_number(value)
            )
            for label, value in traffic_rows
        )

        profiles.append(
            (
                '<div class="scenario-profile" data-profile="{}"{}>'
                '<div class="profile-stats">{}</div>{}'
                '<div class="profile-grid">'
                '<div class="profile-pane"><h3>Ring 双向负载</h3>{}'
                '<div class="hottest-edge"><span>最热边</span><strong>{}</strong></div></div>'
                '<div class="profile-pane"><h3>阻塞构成</h3>{}</div>'
                '<div class="profile-pane"><h3>端点与后端流量</h3>'
                '<div class="profile-kv-list">{}</div></div>'
                '</div>{}{}</div>'
            ).format(
                index,
                "" if index == 0 else " hidden",
                profile_stats,
                _render_topology_overview(scenario, index),
                direction_body,
                html_lib.escape(hottest),
                stall_body,
                traffic_body,
                _multiring_profile(scenario),
                _single_scenario_metric_views(scenario),
            )
        )

    return (
        '<section id="profile"><div class="section-heading">'
        '<h2>单场景性能画像</h2>'
        '<label class="scenario-picker">场景<select id="scenario-select">{}</select></label>'
        '</div>{}</section>'
    ).format("".join(options), "".join(profiles))


def _bandwidth_section(scenarios):
    maximum = max(
        max(
            scenario.number("BANDWIDTH", "end_to_end_bpc"),
            scenario.number("BANDWIDTH", "steady_response_bpc"),
            _theory_number(
                scenario, "THEORY_NO_MERGE", "fabric_ceiling_bpc"
            ),
            _theory_number(
                scenario, "THEORY_IDEAL_MERGE", "fabric_ceiling_bpc"
            ),
        )
        for scenario in scenarios
    )
    rows = []
    for scenario in scenarios:
        end_to_end = scenario.number("BANDWIDTH", "end_to_end_bpc")
        steady = scenario.number("BANDWIDTH", "steady_response_bpc")
        no_merge_ceiling = _theory_number(
            scenario, "THEORY_NO_MERGE", "fabric_ceiling_bpc"
        )
        ideal_ceiling = _theory_number(
            scenario, "THEORY_IDEAL_MERGE", "fabric_ceiling_bpc"
        )
        efficiency = 0.0 if ideal_ceiling <= 0 else end_to_end / ideal_ceiling
        if ideal_ceiling <= 0:
            efficiency_text = "无有效理论上限"
            state_class = "warning"
        else:
            efficiency_text = "模型效率 {:.1f}%".format(100.0 * efficiency)
            state_class = "error" if efficiency > 1.01 else "neutral"
        rows.append(
            (
                '<article class="chart-group {}">'
                '<div class="group-heading"><strong>{}</strong>'
                '<span class="state {}">{}</span></div>{}{}{}{}'
                '</article>'
            ).format(
                state_class,
                html_lib.escape(scenario.case_name),
                state_class,
                html_lib.escape(efficiency_text),
                _bar("端到端", end_to_end, maximum, "bar-teal"),
                _bar("响应稳态", steady, maximum, "bar-blue"),
                _bar("无合并上限", no_merge_ceiling, maximum, "bar-gray"),
                _bar("理想合并上限", ideal_ceiling, maximum, "bar-amber"),
            )
        )
    return (
        '<section id="bandwidth"><div class="section-heading">'
        '<h2>带宽与 Fabric 上限</h2>'
        '<p>仅比较 free-running；同时展示无合并和理想合并两条有限流量上限。</p>'
        '</div><div class="chart-list">{}</div></section>'
    ).format("".join(rows))


def _latency_section(scenarios):
    maximum = max(scenario.number("LATENCY", "max") for scenario in scenarios)
    rows = []
    for scenario in scenarios:
        rows.append(
            (
                '<article class="chart-group">'
                '<div class="group-heading"><strong>{}</strong></div>{}{}{}{}'
                '</article>'
            ).format(
                html_lib.escape(scenario.case_name),
                _bar("P50", scenario.number("LATENCY", "p50"), maximum, "bar-teal"),
                _bar("P95", scenario.number("LATENCY", "p95"), maximum, "bar-blue"),
                _bar("P99", scenario.number("LATENCY", "p99"), maximum, "bar-amber"),
                _bar("最大", scenario.number("LATENCY", "max"), maximum, "bar-red"),
            )
        )
    return (
        '<section id="latency"><div class="section-heading">'
        '<h2>延迟分布</h2><p>所有场景共享同一 cycle 数轴。</p>'
        '</div><div class="chart-list">{}</div></section>'
    ).format("".join(rows))


def _shared_section(scenarios):
    shared = [
        scenario
        for scenario in scenarios
        if scenario.value("CONFIG", "pattern") == "sequential_shared"
        and scenario.number("CONFIG", "burst_len") > 0
    ]
    shared.sort(key=lambda scenario: scenario.number("CONFIG", "burst_len"))
    if not shared:
        body = '<div class="empty">输入中没有 shared read 粒度场景。</div>'
    else:
        maximum = max(
            scenario.number("BANDWIDTH", "end_to_end_bpc")
            for scenario in shared
        )
        rows = []
        for scenario in shared:
            burst_len = scenario.number("CONFIG", "burst_len")
            carrier = "{}/{}/{}".format(
                scenario.number("L2_BUFFER", "carrier_128b"),
                scenario.number("L2_BUFFER", "carrier_256b"),
                scenario.number("L2_BUFFER", "carrier_512b"),
            )
            role = "单 beat" if burst_len == 1 else "多 beat"
            rows.append(
                (
                    '<tr><th>{} beats <span class="subtle">{}</span></th>'
                    '<td>{}</td><td>{}</td><td>{}</td><td>{}</td>'
                    '<td>{}</td><td>{}</td></tr>'
                ).format(
                    burst_len,
                    role,
                    _bar(
                        "",
                        scenario.number("BANDWIDTH", "end_to_end_bpc"),
                        maximum,
                        "bar-teal",
                    ),
                    _format_number(scenario.number("THEORY", "physical_packets")),
                    _format_number(scenario.number("HOME_AGENT", "backend_read_saved")),
                    _format_number(
                        scenario.number("L2_BUFFER", "h_multicast_carriers")
                    ),
                    _format_number(
                        scenario.number("L2_BUFFER", "h_carrier_recipients")
                    ),
                    carrier,
                )
            )
        body = (
            '<div class="table-wrap"><table><thead><tr>'
            '<th>Burst length</th><th>端到端 B/cycle</th><th>物理包</th>'
            '<th>后端读节省</th><th>H-Ring 多播载体</th>'
            '<th>H-Ring 载体接收者</th>'
            '<th>Carrier 128/256/512B</th>'
            '</tr></thead><tbody>{}</tbody></table></div>'
        ).format("".join(rows))
    return (
        '<section id="shared"><div class="section-heading">'
        '<h2>Shared read 粒度收益</h2>'
        '<p>Burst length 表示每笔请求包含的完整 beat 数。</p>'
        '</div>{}</section>'
    ).format(body)


def _bottleneck_section(scenarios):
    selected = [
        scenario
        for scenario in scenarios
        if scenario.case_name.startswith("bottleneck_")
        or "interleave" in scenario.case_name.lower()
    ]
    if not selected:
        body = '<div class="empty">输入中没有 bottleneck 或 interleave 对照场景。</div>'
    else:
        rows = []
        for scenario in selected:
            ring_stalls = sum(
                scenario.number("RING", "{}_stalls".format(subnet))
                for subnet in ("req", "rsp", "dat")
            )
            l2_stalls = sum(
                scenario.number("L2_BUFFER", key)
                for key in (
                    "buffer_full_stalls",
                    "issue_interval_stalls",
                    "dat_inject_stalls",
                )
            )
            memory_stalls = (
                scenario.number("MEMORY", "credit_stalls")
                + scenario.number("MEMORY", "queue_full_stalls")
            )
            values = (
                ring_stalls,
                l2_stalls,
                memory_stalls,
                scenario.number("HOME_AGENT", "write_hazard_stalls"),
            )
            total = sum(values)
            if total == 0:
                stall_view = '<span class="subtle">未观察到 stall</span>'
            else:
                classes = ("seg-teal", "seg-amber", "seg-red", "seg-gray")
                segments = []
                for value, css_class in zip(values, classes):
                    if value:
                        segments.append(
                            '<span class="segment {}" style="width:{:.2f}%"></span>'.format(
                                css_class, 100.0 * value / total
                            )
                        )
                stall_view = '<div class="stacked">{}</div>'.format("".join(segments))
            rows.append(
                (
                    '<tr><th>{}</th><td>{}</td><td>{}</td><td>{}</td>'
                    '<td>{}</td><td>{}</td><td>{}</td><td>{}</td>'
                    '<td>{}</td></tr>'
                ).format(
                    html_lib.escape(scenario.case_name),
                    stall_view,
                    _format_number(ring_stalls),
                    _format_number(l2_stalls),
                    _format_number(memory_stalls),
                    _format_number(scenario.number("MEMORY", "outstanding_peak")),
                    _format_number(scenario.number("HOME_AGENT", "write_hazard_stalls")),
                    _format_number(scenario.number("HOME_AGENT", "backend_read_saved")),
                    _format_number(
                        scenario.number("L2_BUFFER", "h_multicast_carriers")
                    ),
                )
            )
        body = (
            '<div class="legend"><span class="dot seg-teal"></span>Ring '
            '<span class="dot seg-amber"></span>L2 Buffer '
            '<span class="dot seg-red"></span>TmMem '
            '<span class="dot seg-gray"></span>HA</div>'
            '<div class="table-wrap"><table><thead><tr><th>场景</th><th>Stall 构成</th>'
            '<th>Ring</th><th>L2</th><th>TmMem</th>'
            '<th>Mem OSD peak</th><th>HA 写冲突</th><th>后端读节省</th>'
            '<th>H-Ring 多播载体</th></tr></thead><tbody>{}</tbody></table></div>'
        ).format("".join(rows))
    return (
        '<section id="bottleneck"><div class="section-heading">'
        '<h2>瓶颈迁移</h2>'
        '<p>只展示输入中实际存在的瓶颈与交织场景，不根据完成周期猜测瓶颈。</p>'
        '</div>{}</section>'
    ).format(body)


def _aggregation_wave_section(scenarios):
    waves = [
        scenario
        for scenario in scenarios
        if _run_mode(scenario) == "aggregation_wave"
    ]
    if not waves:
        body = '<div class="empty">输入中没有同步 wave 聚合场景。</div>'
    else:
        rows = []
        for scenario in waves:
            expected_h = _theory_number(
                scenario, "THEORY_IDEAL_MERGE", "h_carriers"
            )
            expected_v = _theory_number(
                scenario, "THEORY_IDEAL_MERGE", "v_carriers"
            )
            actual_v = scenario.value("FANOUT_CROSS_RING", "v_ring_carriers")
            actual_v_text = "N/A" if actual_v is None else _format_number(
                _parse_number(actual_v)
            )
            admission_stalls = sum(
                _optional_number(scenario, "HOME_AGENT", key)
                for key in (
                    "table_full_stalls",
                    "waiter_full_stalls",
                    "aggregation_closed_stalls",
                )
            )
            merge_phases = "{}/{}/{}".format(
                _optional_number(scenario, "HOME_AGENT", "rd_merged_pending"),
                _optional_number(scenario, "HOME_AGENT", "rd_merged_inflight"),
                _optional_number(
                    scenario, "HOME_AGENT", "rd_merged_responding"
                ),
            )
            rows.append(
                (
                    '<tr><th>{}</th><td>{}</td><td>{} / {}</td>'
                    '<td>{} / {}</td><td>{} / {}</td><td>{} / {}</td>'
                    '<td>{} / {}</td><td>{}</td><td>{}</td><td>{}</td>'
                    '</tr>'
                ).format(
                    html_lib.escape(scenario.case_name),
                    html_lib.escape(scenario.value("CONFIG", "pattern")),
                    _format_number(expected_h),
                    _format_number(scenario.number("L2_BUFFER", "h_carriers")),
                    _format_number(expected_v),
                    actual_v_text,
                    _format_number(
                        _theory_number(
                            scenario,
                            "THEORY_IDEAL_MERGE",
                            "backend_read_saved",
                        )
                    ),
                    _format_number(
                        scenario.number("HOME_AGENT", "backend_read_saved")
                    ),
                    _format_number(
                        _theory_number(
                            scenario,
                            "THEORY_IDEAL_MERGE",
                            "h_multicast_carriers",
                        )
                    ),
                    _format_number(
                        scenario.number("L2_BUFFER", "h_multicast_carriers")
                    ),
                    _format_number(
                        _theory_number(
                            scenario,
                            "THEORY_IDEAL_MERGE",
                            "h_scatter_carriers",
                        )
                    ),
                    _format_number(
                        scenario.number("L2_BUFFER", "h_scatter_carriers")
                    ),
                    merge_phases,
                    _format_number(admission_stalls),
                    _scaling_efficiency_text(scenario),
                )
            )
        body = (
            '<div class="table-wrap"><table><thead><tr>'
            '<th>场景</th><th>Pattern</th><th>预期 / 实际 H carrier</th>'
            '<th>预期 / 实际 V carrier</th><th>预期 / 实际后端读节省</th>'
            '<th>预期 / 实际 multicast</th><th>预期 / 实际 scatter</th>'
            '<th>HA merge P/I/R</th><th>HA admission stalls</th>'
            '<th>扩展效率</th>'
            '</tr></thead><tbody>{}</tbody></table></div>'
        ).format("".join(rows))
    return (
        '<section id="aggregation-wave"><div class="section-heading">'
        '<h2>同步聚合验证</h2>'
        '<p>短 trace 只验证 carrier 合并，不参与持续吞吐排名。</p>'
        '</div>{}</section>'
    ).format(body)


def _details_section(scenarios):
    rows = []
    raw = []
    for scenario in scenarios:
        efficiency = _fabric_efficiency(scenario)
        efficiency_text = (
            "无有效上限"
            if scenario.number("THEORY", "fabric_ceiling_bpc") <= 0
            else "{:.1f}%".format(100.0 * efficiency)
        )
        status = "PASS" if _scenario_passes(scenario) else "INCOMPLETE"
        rows.append(
            (
                '<tr><th>{}</th><td>{}</td><td>{}</td><td>{}</td><td>{}</td>'
                '<td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td>'
                '<td>{}</td><td>{}</td><td>{}</td><td>{}</td>'
                '<td><span class="state {}">{}</span></td></tr>'
            ).format(
                html_lib.escape(scenario.case_name),
                html_lib.escape(scenario.value("CONFIG", "op")),
                html_lib.escape(scenario.value("CONFIG", "pattern")),
                _format_number(scenario.number("CONFIG", "active_masters")),
                _format_number(scenario.number("CONFIG", "bytes_per_master")),
                _format_number(scenario.number("CONFIG", "burst_len")),
                _format_number(scenario.number("COUNTS", "completed_bytes")),
                _format_number(scenario.number("BANDWIDTH", "transfer_cycles")),
                "{}/{}".format(
                    _format_number(scenario.number("BANDWIDTH", "end_to_end_bpc")),
                    _format_number(scenario.number("BANDWIDTH", "steady_response_bpc")),
                ),
                _format_number(scenario.number("LATENCY", "p50")),
                _format_number(scenario.number("LATENCY", "p95")),
                _format_number(scenario.number("LATENCY", "p99")),
                _format_number(scenario.number("LATENCY", "max")),
                _format_number(scenario.number("BANDWIDTH", "jain_fairness")),
                "{} / {}".format(
                    _format_number(scenario.number("THEORY", "fabric_ceiling_bpc")),
                    efficiency_text,
                ),
                _scaling_efficiency_text(scenario),
                "pass" if status == "PASS" else "error",
                status,
            )
        )
        raw.append(
            '<details><summary>{}</summary><pre>{}</pre></details>'.format(
                html_lib.escape(scenario.case_name),
                html_lib.escape(scenario.raw_text),
            )
        )
    return (
        '<section id="details"><div class="section-heading"><h2>完整数据</h2>'
        '<p>带宽列为端到端/稳态，延迟列为 P50/P95/P99/max。</p></div>'
        '<div class="table-wrap"><table><thead><tr><th>Case</th><th>Op</th>'
        '<th>Pattern</th><th>Master</th><th>每 Master 字节</th><th>Burst length</th>'
        '<th>完成字节</th><th>周期</th><th>带宽</th><th colspan="4">延迟</th>'
        '<th>Jain</th><th>上限/效率</th><th>扩展效率</th><th>状态</th></tr></thead>'
        '<tbody>{}</tbody></table></div><div class="raw-data">{}</div></section>'
    ).format("".join(rows), "".join(raw))


def render_html(scenarios, source_name):
    """Render parsed scenarios as one self-contained comparison document."""
    if not scenarios:
        raise PerfReportError("cannot render an empty scenario list")

    passed = sum(1 for scenario in scenarios if _scenario_passes(scenario))
    free_running = [
        scenario for scenario in scenarios if _run_mode(scenario) == "free_running"
    ]
    best = (
        max(
            free_running,
            key=lambda scenario: scenario.number("BANDWIDTH", "end_to_end_bpc"),
        )
        if free_running
        else None
    )
    summary = "".join(
        (
            _metric_card("场景", len(scenarios), "完整 PERF block"),
            _metric_card("PASS", passed, "其余 {}".format(len(scenarios) - passed)),
            _metric_card(
                "最高端到端带宽",
                "N/A"
                if best is None
                else "{} B/cycle".format(
                    _format_number(best.number("BANDWIDTH", "end_to_end_bpc"))
                ),
                "无 free-running 场景" if best is None else best.case_name,
            ),
            _metric_card("输入", source_name, "原始 TXT 可追溯"),
        )
    )

    style = r"""
:root { color-scheme: light; --ink:#17202a; --muted:#61707c; --line:#d8dee4;
  --paper:#ffffff; --canvas:#f3f5f6; --teal:#167d72; --blue:#3973b9;
  --amber:#c18416; --red:#b64b45; --gray:#7a838b; }
* { box-sizing:border-box; }
body { margin:0; background:var(--canvas); color:var(--ink);
  font-family:"Segoe UI","Microsoft YaHei",Arial,sans-serif; font-size:14px; line-height:1.5; }
header { background:#182329; color:#fff; padding:28px max(24px,calc((100% - 1440px)/2)); }
header h1 { margin:0 0 6px; font-size:28px; letter-spacing:0; }
header p { margin:0; color:#cbd5da; }
main { max-width:1440px; margin:0 auto; padding:24px; }
.metrics { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:12px; margin-bottom:24px; }
.metric { min-width:0; padding:14px 16px; background:var(--paper); border:1px solid var(--line); border-radius:6px; }
.metric-label,.metric-note,.subtle { color:var(--muted); font-size:12px; }
.metric-value { margin:4px 0; font-size:22px; font-weight:700; overflow-wrap:anywhere; }
section { margin:0 0 20px; padding:20px; background:var(--paper); border:1px solid var(--line); }
.section-heading { display:flex; justify-content:space-between; gap:20px; align-items:baseline; margin-bottom:16px; }
h2 { margin:0; font-size:19px; letter-spacing:0; }
.section-heading p { margin:0; color:var(--muted); text-align:right; }
.chart-list { display:grid; gap:10px; }
.chart-group { padding:12px 0; border-top:1px solid #edf0f2; }
.chart-group:first-child { border-top:0; }
.chart-group.error { border-left:3px solid var(--red); padding-left:10px; }
.group-heading { display:flex; justify-content:space-between; gap:12px; margin-bottom:8px; }
.state { display:inline-block; padding:1px 7px; border-radius:4px; background:#edf0f2; font-size:12px; }
.state.pass { color:#12665d; background:#e1f2ed; }
.state.error { color:#8e312c; background:#f8e3e1; }
.state.warning { color:#825808; background:#fbefd5; }
.bar-line { display:grid; grid-template-columns:92px minmax(100px,1fr) 86px; gap:10px; align-items:center; min-height:24px; }
.bar-name { color:var(--muted); }
.bar-track { height:9px; background:#edf0f2; overflow:hidden; }
.bar-fill { display:block; height:100%; min-width:0; }
.bar-teal,.seg-teal { background:var(--teal); }
.bar-blue,.seg-blue { background:var(--blue); }
.bar-amber,.seg-amber { background:var(--amber); }
.bar-red,.seg-red { background:var(--red); }
.bar-gray,.seg-gray { background:var(--gray); }
.bar-value { text-align:right; font-variant-numeric:tabular-nums; }
.scenario-picker { display:flex; align-items:center; gap:8px; color:var(--muted); }
.scenario-picker select { min-width:260px; max-width:480px; padding:7px 30px 7px 9px;
  color:var(--ink); background:var(--paper); border:1px solid var(--line); }
.profile-stats { display:grid; grid-template-columns:repeat(5,minmax(0,1fr));
  border-top:1px solid var(--line); border-bottom:1px solid var(--line); }
.profile-stat { min-width:0; padding:12px 14px; border-right:1px solid var(--line); }
.profile-stat:last-child { border-right:0; }
.profile-stat span,.profile-stat small { display:block; color:var(--muted); font-size:12px; }
.profile-stat strong { display:block; margin:3px 0; font-size:18px; overflow-wrap:anywhere; }
.topology-overview { margin-top:20px; border:1px solid var(--line); background:#fbfcfc; }
.topology-heading { display:flex; justify-content:space-between; gap:12px; align-items:center; padding:12px 14px; border-bottom:1px solid var(--line); }
.topology-heading strong,.topology-heading span { display:block; }
.topology-heading span { color:var(--muted); font-size:12px; }
.topology-subnets { display:flex; border:1px solid var(--line); background:var(--paper); }
.topology-subnet { min-width:48px; padding:6px 9px; border:0; border-right:1px solid var(--line); background:var(--paper); color:var(--ink); font:inherit; font-size:12px; cursor:pointer; }
.topology-subnet:last-child { border-right:0; }
.topology-subnet.active { background:#182329; color:#fff; }
.topology-rings { display:flex; gap:6px; padding:10px 14px 0; }
.topology-ring { padding:4px 8px; border:1px solid var(--line); background:var(--paper); color:var(--muted); font:inherit; font-size:12px; cursor:pointer; }
.topology-ring.active { border-color:var(--teal); color:var(--teal); }
.topology-legend { display:flex; flex-wrap:wrap; gap:14px; padding:10px 14px 0; color:var(--muted); font-size:12px; }
.topology-legend span { display:flex; align-items:center; gap:6px; }
.topology-legend i { display:inline-block; }
.topology-legend-hot { width:16px; height:3px; background:var(--red); }
.topology-legend-wide { width:16px; height:5px; background:var(--teal); }
.topology-legend-private { width:16px; border-top:2px dashed #9faab1; }
.topology-canvas { overflow-x:auto; }
.topology-canvas svg { display:block; min-width:1040px; width:100%; height:auto; }
.topology-skeleton { fill:none; stroke:#d9e1e5; stroke-width:2; stroke-linecap:round; }
.topology-edge { fill:none; stroke:#c9d1d6; stroke-width:2; stroke-linecap:round; opacity:0; cursor:pointer; }
.topology-overview[data-active-subnet="req"] .topology-edge[data-subnet="req"],
.topology-overview[data-active-subnet="rsp"] .topology-edge[data-subnet="rsp"],
.topology-overview[data-active-subnet="dat"] .topology-edge[data-subnet="dat"] { opacity:.92; }
.topology-edge.topology-edge-idle { stroke:#b7c2c9; opacity:.42 !important; }
.topology-edge.topology-edge-low { stroke:var(--teal); }
.topology-edge.topology-edge-warm { stroke:var(--amber); }
.topology-edge.topology-edge-hot { stroke:var(--red); }
.topology-edge.topology-edge-thin { stroke-width:2; }
.topology-edge.topology-edge-medium { stroke-width:3.5; }
.topology-edge.topology-edge-wide { stroke-width:5; }
.topology-overview[data-active-ring="h0"] .topology-edge:not([data-ring="h0"]),
.topology-overview[data-active-ring="v0"] .topology-edge:not([data-ring="v0"]),
.topology-overview[data-active-ring="v1"] .topology-edge:not([data-ring="v1"]) { opacity:.08 !important; }
.topology-overview[data-active-ring="h0"] .topology-skeleton:not([data-ring="h0"]),
.topology-overview[data-active-ring="v0"] .topology-skeleton:not([data-ring="v0"]),
.topology-overview[data-active-ring="v1"] .topology-skeleton:not([data-ring="v1"]) { opacity:.16; }
.topology-edge[data-missing="true"] { stroke-dasharray:3 4; }
.topology-private-link { stroke:#9faab1; stroke-width:1.5; stroke-dasharray:4 4; }
.topology-node { cursor:pointer; }
.topology-node circle { fill:var(--paper); stroke-width:4; stroke:#6d7880; }
.topology-node text { fill:#45535c; font-size:12px; font-weight:600; }
.topology-node-rbrg circle { stroke:#865aa3; }
.topology-node-home_agent circle { stroke:var(--blue); }
.topology-node-l2_buffer circle { stroke:var(--teal); }
.topology-path-summary { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:10px; padding:12px 14px; border-top:1px solid var(--line); }
.topology-path-summary div { min-width:0; padding:8px 10px; border-left:3px solid var(--teal); background:var(--paper); }
.topology-path-summary span,.topology-path-summary strong { display:block; }
.topology-path-summary span { color:var(--muted); font-size:11px; }
.topology-path-summary strong { margin-top:2px; font-size:13px; overflow-wrap:anywhere; }
.topology-detail-panel { min-height:78px; padding:12px 14px; border-top:1px solid var(--line); background:var(--paper); }
.topology-detail[hidden] { display:none; }
.topology-detail.active { display:block; }
.topology-detail-row { display:flex; justify-content:space-between; gap:12px; padding:3px 0; }
.topology-detail-row span,.topology-detail-empty { color:var(--muted); }
.topology-unavailable { margin-top:20px; padding:14px; border:1px dashed #bdc6cc; color:var(--muted); }
.profile-grid { display:grid; grid-template-columns:1.2fr 1fr .9fr; gap:24px; margin-top:20px; }
.profile-pane { min-width:0; }
.profile-pane h3 { margin:0 0 12px; font-size:15px; }
.direction-row { display:grid; grid-template-columns:92px minmax(0,1fr); gap:12px;
  align-items:center; margin-bottom:14px; }
.direction-row > div:first-child strong,.direction-row > div:first-child span { display:block; }
.direction-row > div:first-child span { color:var(--muted); font-size:12px; }
.direction-track { display:flex; width:100%; height:12px; background:#edf0f2; overflow:hidden; }
.direction-cw { background:var(--teal); }
.direction-ccw { background:var(--blue); }
.direction-values { display:flex; justify-content:space-between; gap:12px; margin-top:4px;
  color:var(--muted); font-size:12px; font-variant-numeric:tabular-nums; }
.hottest-edge { display:flex; justify-content:space-between; gap:12px; padding-top:10px;
  border-top:1px solid #e5e9ec; }
.hottest-edge span { color:var(--muted); }
.hottest-edge strong { text-align:right; }
.profile-kv-list { border-top:1px solid #e5e9ec; }
.profile-kv { display:flex; justify-content:space-between; gap:12px; padding:7px 0;
  border-bottom:1px solid #e5e9ec; font-variant-numeric:tabular-nums; }
.profile-kv span { color:var(--muted); }
.table-wrap { overflow-x:auto; }
table { width:100%; border-collapse:collapse; font-variant-numeric:tabular-nums; }
th,td { padding:9px 10px; border-bottom:1px solid #e5e9ec; text-align:right; white-space:nowrap; }
th:first-child,td:first-child { text-align:left; }
thead th { color:#46545d; background:#f5f7f8; font-size:12px; }
tbody th { font-weight:600; }
td .bar-line { grid-template-columns:minmax(90px,1fr) 70px; min-width:200px; }
td .bar-name { display:none; }
.legend { display:flex; flex-wrap:wrap; gap:14px; margin-bottom:10px; color:var(--muted); font-size:12px; }
.dot { display:inline-block; width:9px; height:9px; margin-right:-9px; }
.stacked { display:flex; width:180px; height:10px; background:#edf0f2; overflow:hidden; }
.segment { display:block; height:100%; }
.empty { padding:18px; border:1px dashed #bdc6cc; color:var(--muted); text-align:center; }
.raw-data { margin-top:18px; }
details { border-top:1px solid #e5e9ec; padding:10px 0; }
summary { cursor:pointer; font-weight:600; }
pre { overflow:auto; padding:12px; background:#151d21; color:#dbe4e8; font-size:12px; }
@media (max-width:800px) {
  .metrics { grid-template-columns:repeat(2,minmax(0,1fr)); }
  .section-heading { display:block; }
  .section-heading p { margin-top:4px; text-align:left; }
  .bar-line { grid-template-columns:72px minmax(60px,1fr) 72px; }
  .scenario-picker { margin-top:8px; }
  .scenario-picker select { min-width:0; width:100%; }
  .profile-stats { grid-template-columns:repeat(2,minmax(0,1fr)); }
  .profile-stat { border-bottom:1px solid var(--line); }
  .profile-grid { grid-template-columns:1fr; }
  .topology-path-summary { grid-template-columns:1fr; }
}
@media print {
  body { background:#fff; }
  header { padding:18px 0; color:#000; background:#fff; border-bottom:2px solid #000; }
  header p { color:#333; }
  main { max-width:none; padding:12px 0; }
  section,.metric { break-inside:avoid; }
  details pre { display:block; }
}
"""
    body = "".join(
        (
            '<div class="metrics">{}</div>'.format(summary),
            _scenario_profile_section(free_running)
            if free_running
            else "",
            _bandwidth_section(free_running) if free_running else "",
            _latency_section(free_running) if free_running else "",
            _shared_section(free_running),
            _bottleneck_section(free_running),
            _aggregation_wave_section(scenarios),
            _details_section(scenarios),
        )
    )
    script = r"""
<script>
(function () {
  var select = document.getElementById("scenario-select");
  if (select) {
    var profiles = document.querySelectorAll("[data-profile]");
    select.addEventListener("change", function () {
      for (var index = 0; index < profiles.length; ++index) {
        profiles[index].hidden = profiles[index].getAttribute("data-profile") !== select.value;
      }
    });
  }

  var topologies = document.querySelectorAll(".topology-overview");
  for (var topologyIndex = 0; topologyIndex < topologies.length; ++topologyIndex) {
    (function (topology) {
      function showDetail(target) {
        var details = topology.querySelectorAll("[data-topology-detail]");
        for (var detailIndex = 0; detailIndex < details.length; ++detailIndex) {
          var detail = details[detailIndex];
          var active = detail.getAttribute("data-topology-detail") === target;
          detail.hidden = !active;
          if (active) detail.classList.add("active");
          else detail.classList.remove("active");
        }
      }

      var subnetButtons = topology.querySelectorAll(".topology-subnet");
      for (var subnetIndex = 0; subnetIndex < subnetButtons.length; ++subnetIndex) {
        subnetButtons[subnetIndex].addEventListener("click", function () {
          topology.setAttribute("data-active-subnet", this.getAttribute("data-subnet"));
          for (var itemIndex = 0; itemIndex < subnetButtons.length; ++itemIndex) {
            subnetButtons[itemIndex].classList.toggle("active", subnetButtons[itemIndex] === this);
          }
        });
      }

      var ringButtons = topology.querySelectorAll(".topology-ring");
      for (var ringIndex = 0; ringIndex < ringButtons.length; ++ringIndex) {
        ringButtons[ringIndex].addEventListener("click", function () {
          topology.setAttribute("data-active-ring", this.getAttribute("data-ring"));
          for (var itemIndex = 0; itemIndex < ringButtons.length; ++itemIndex) {
            ringButtons[itemIndex].classList.toggle("active", ringButtons[itemIndex] === this);
          }
        });
      }

      var drillTargets = topology.querySelectorAll("[data-detail-target]");
      for (var targetIndex = 0; targetIndex < drillTargets.length; ++targetIndex) {
        drillTargets[targetIndex].addEventListener("click", function () {
          showDetail(this.getAttribute("data-detail-target"));
        });
      }
    }(topologies[topologyIndex]));
  }
}());
</script>
"""
    return (
        '<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>Ring 性能横向对比</title><style>{}</style></head><body>'
        '<header><h1>Ring 性能横向对比</h1>'
        '<p>来源：{}</p></header><main>{}</main>{}</body></html>'
    ).format(style, html_lib.escape(str(source_name)), body, script)


def generate_report(input_path, output_path=None):
    """Read PERF results and write the comparison HTML file."""
    input_path = Path(input_path)
    output_path = (
        Path(output_path) if output_path else input_path.with_suffix(".html")
    )
    text = input_path.read_text(encoding="utf-8")
    scenarios = parse_perf_results(text)
    output_path.write_text(
        render_html(scenarios, input_path.name), encoding="utf-8"
    )
    return output_path


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate one offline HTML report from PERF_* results."
    )
    parser.add_argument("input", type=Path, help="PERF_* result text file")
    parser.add_argument(
        "output", nargs="?", type=Path, help="output HTML file"
    )
    args = parser.parse_args(argv)
    try:
        generated = generate_report(args.input, args.output)
    except (OSError, PerfReportError) as error:
        print("error: {}".format(error), file=sys.stderr)
        return 1
    print(generated)
    return 0


if __name__ == "__main__":
    sys.exit(main())
