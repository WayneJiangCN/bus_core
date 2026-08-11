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
    "CONFIG": ("active_masters", "bytes_per_master", "burst_bytes"),
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
                '<tr><th>{}{}</th><td>{} {}</td><td>{} {}</td><td>{}</td>'
                '<td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>'
            ).format(
                html_lib.escape(record["domain"].upper()),
                html_lib.escape(record["ring"]),
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
        '<div class="table-wrap"><table><thead><tr><th>Ring</th><th>Node</th>'
        '<th>Queue</th><th>Direction</th><th>Depth</th><th>Peak</th>'
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
        '<div class="profile-grid">'
        '<div class="profile-pane"><h3>Physical Channel Utilization（物理通道利用率）</h3>{}</div>'
        '<div class="profile-pane"><h3>Deflection Recovery（绕圈恢复）</h3>{}</div>'
        '<div class="profile-pane"><h3>RBRG Channel（RBRG 路径）</h3>{}</div>'
        '</div><div class="profile-pane"><h3>Per-Edge Hotspots（逐边热点）</h3>{}</div>'
        '<div class="profile-pane"><h3>Endpoint Buffer</h3>{}</div>'
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
                '<div class="profile-stats">{}</div>'
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
        and scenario.number("CONFIG", "burst_bytes") in (16, 128, 256, 512)
    ]
    shared.sort(key=lambda scenario: scenario.number("CONFIG", "burst_bytes"))
    if not shared:
        body = '<div class="empty">输入中没有 shared read 粒度场景。</div>'
    else:
        maximum = max(
            scenario.number("BANDWIDTH", "end_to_end_bpc")
            for scenario in shared
        )
        rows = []
        for scenario in shared:
            burst = scenario.number("CONFIG", "burst_bytes")
            carrier = "{}/{}/{}".format(
                scenario.number("L2_BUFFER", "carrier_128b"),
                scenario.number("L2_BUFFER", "carrier_256b"),
                scenario.number("L2_BUFFER", "carrier_512b"),
            )
            role = "未优化对照" if burst == 16 else "优化粒度"
            rows.append(
                (
                    '<tr><th>{}B <span class="subtle">{}</span></th>'
                    '<td>{}</td><td>{}</td><td>{}</td><td>{}</td>'
                    '<td>{}</td><td>{}</td></tr>'
                ).format(
                    burst,
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
            '<th>Burst</th><th>端到端 B/cycle</th><th>物理包</th>'
            '<th>后端读节省</th><th>H-Ring 多播载体</th>'
            '<th>H-Ring 载体接收者</th>'
            '<th>Carrier 128/256/512B</th>'
            '</tr></thead><tbody>{}</tbody></table></div>'
        ).format("".join(rows))
    return (
        '<section id="shared"><div class="section-heading">'
        '<h2>Shared read 粒度收益</h2>'
        '<p>16B 作为未优化对照，128B 为主优化粒度，256B/512B 用于观察宽 carrier。</p>'
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
                '<td>{}</td><td>{}</td><td>{}</td><td>{}/{}/{}/{}</td>'
                '<td>{}</td><td>{}</td><td>{}</td>'
                '<td><span class="state {}">{}</span></td></tr>'
            ).format(
                html_lib.escape(scenario.case_name),
                html_lib.escape(scenario.value("CONFIG", "op")),
                html_lib.escape(scenario.value("CONFIG", "pattern")),
                _format_number(scenario.number("CONFIG", "active_masters")),
                _format_number(scenario.number("CONFIG", "bytes_per_master")),
                _format_number(scenario.number("CONFIG", "burst_bytes")),
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
        '<th>Pattern</th><th>Master</th><th>每 Master 字节</th><th>Burst</th>'
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
  if (!select) return;
  var profiles = document.querySelectorAll("[data-profile]");
  select.addEventListener("change", function () {
    for (var index = 0; index < profiles.length; ++index) {
      profiles[index].hidden = profiles[index].getAttribute("data-profile") !== select.value;
    }
  });
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
