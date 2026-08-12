import io
import re
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))

from generate_ring_perf_report import (
    PerfReportError,
    generate_report,
    main,
    parse_perf_results,
    render_html,
)


_EXTERNAL_SCRIPT_RE = re.compile(
    r"<script\b[^>]*\s+src\s*=", re.IGNORECASE
)


def has_external_script(document):
    return _EXTERNAL_SCRIPT_RE.search(document) is not None


def perf_block(case_name, burst_bytes, end_to_end_bpc, ceiling_bpc=16.0):
    return """\
PERF_CONFIG case={case_name} op=read pattern=sequential_shared active_masters=8 bytes_per_master=131072 burst_bytes={burst_bytes}
PERF_COUNTS completed_packets=64 completed_bytes=1048576 protocol_errors=0 drained=1
PERF_BANDWIDTH first_request=10 first_response=20 last_response=100 transfer_cycles=91 end_to_end_bpc={end_to_end_bpc} steady_response_bpc=9.000000 scaling_efficiency=0.900000 jain_fairness=1.000000
PERF_LATENCY p50=20 p95=30 p99=40 max=50
PERF_RING req_packets=64 req_bytes=4096 req_busy_cycles=64 req_stalls=1 rsp_packets=0 rsp_bytes=0 rsp_busy_cycles=0 rsp_stalls=0 dat_packets=64 dat_bytes=1048576 dat_busy_cycles=8192 dat_stalls=2 cross_station_injected=128 cross_station_ejected=128 hottest_subnet=dat hottest_src_station=1 hottest_direction=cw hottest_cycles=8192
PERF_HOME_AGENT rd_requests=64 backend_reads=8 backend_read_saved=56 l2_hits=48 l2_misses=16 write_hazard_stalls=0
PERF_L2_BUFFER responses_accepted=8 h_carriers=8 h_unicast_carriers=0 h_multicast_carriers=8 h_scatter_carriers=0 h_carrier_recipients=64 dat_bytes=1048576 occupancy_peak=4 buffer_full_stalls=0 issue_interval_stalls=2 dat_inject_stalls=3 carrier_128b=8 carrier_256b=0 carrier_512b=0
PERF_MEMORY accepted_read_bytes=1024 accepted_write_bytes=0 credit_stalls=0 queue_full_stalls=0 outstanding_peak=4
PERF_THEORY total_useful_bytes=1048576 physical_packets=8 fabric_min_cycles=65536 fabric_ceiling_bpc={ceiling_bpc} measured_over_fabric_ceiling=0.500000 assumption=finite_trace_packet_slot_upper_bound
PERF_RESULT status=PASS protocol_errors=0
""".format(
        case_name=case_name,
        burst_bytes=burst_bytes,
        end_to_end_bpc=end_to_end_bpc,
        ceiling_bpc=ceiling_bpc,
    )


def multi_ring_records():
    return """\
PERF_RING_DOMAIN type=h id=0 req_cw_packets=4 req_cw_bytes=64 req_cw_busy_cycles=8 req_cw_stalls=2 req_ccw_packets=0 req_ccw_bytes=0 req_ccw_busy_cycles=0 req_ccw_stalls=0 rsp_cw_packets=0 rsp_cw_bytes=0 rsp_cw_busy_cycles=0 rsp_cw_stalls=0 rsp_ccw_packets=0 rsp_ccw_bytes=0 rsp_ccw_busy_cycles=0 rsp_ccw_stalls=0 dat_cw_packets=2 dat_cw_bytes=256 dat_cw_busy_cycles=6 dat_cw_stalls=1 dat_ccw_packets=0 dat_ccw_bytes=0 dat_ccw_busy_cycles=0 dat_ccw_stalls=0 hottest_subnet=dat hottest_src_station=1 hottest_src_direction=cw hottest_dst_station=2 hottest_dst_direction=ccw hottest_packets=2 hottest_bytes=256 hottest_busy_cycles=6 hottest_serialization_busy_stall=1 hottest_stalls=1 hottest_inflight_peak=1
PERF_RING_DOMAIN type=v id=0 req_cw_packets=1 req_cw_bytes=16 req_cw_busy_cycles=2 req_cw_stalls=0 req_ccw_packets=0 req_ccw_bytes=0 req_ccw_busy_cycles=0 req_ccw_stalls=0 rsp_cw_packets=0 rsp_cw_bytes=0 rsp_cw_busy_cycles=0 rsp_cw_stalls=0 rsp_ccw_packets=0 rsp_ccw_bytes=0 rsp_ccw_busy_cycles=0 rsp_ccw_stalls=0 dat_cw_packets=3 dat_cw_bytes=384 dat_cw_busy_cycles=7 dat_cw_stalls=2 dat_ccw_packets=1 dat_ccw_bytes=128 dat_ccw_busy_cycles=3 dat_ccw_stalls=0 hottest_subnet=dat hottest_src_station=0 hottest_src_direction=cw hottest_dst_station=1 hottest_dst_direction=cw hottest_packets=3 hottest_bytes=384 hottest_busy_cycles=7 hottest_serialization_busy_stall=2 hottest_stalls=2 hottest_inflight_peak=2
PERF_RING_DOMAIN type=v id=1 req_cw_packets=0 req_cw_bytes=0 req_cw_busy_cycles=0 req_cw_stalls=0 req_ccw_packets=2 req_ccw_bytes=32 req_ccw_busy_cycles=4 req_ccw_stalls=1 rsp_cw_packets=1 rsp_cw_bytes=8 rsp_cw_busy_cycles=1 rsp_cw_stalls=0 rsp_ccw_packets=0 rsp_ccw_bytes=0 rsp_ccw_busy_cycles=0 rsp_ccw_stalls=0 dat_cw_packets=2 dat_cw_bytes=256 dat_cw_busy_cycles=5 dat_cw_stalls=0 dat_ccw_packets=1 dat_ccw_bytes=128 dat_ccw_busy_cycles=2 dat_ccw_stalls=1 hottest_subnet=req hottest_src_station=3 hottest_src_direction=ccw hottest_dst_station=0 hottest_dst_direction=cw hottest_packets=2 hottest_bytes=32 hottest_busy_cycles=4 hottest_serialization_busy_stall=1 hottest_stalls=1 hottest_inflight_peak=1
PERF_RBRG id=0 v_to_h_req_packets=2 v_to_h_req_bytes=32 v_to_h_req_queue_peak=1 v_to_h_req_queue_full_stalls=0 v_to_h_req_inject_stalls=0 v_to_h_dat_packets=0 v_to_h_dat_bytes=0 v_to_h_dat_queue_peak=0 v_to_h_dat_queue_full_stalls=0 v_to_h_dat_inject_stalls=0 h_to_v_rsp_packets=1 h_to_v_rsp_bytes=8 h_to_v_rsp_queue_peak=1 h_to_v_rsp_queue_full_stalls=0 h_to_v_rsp_inject_stalls=0 h_to_v_dat_packets=2 h_to_v_dat_bytes=256 h_to_v_dat_queue_peak=2 h_to_v_dat_queue_full_stalls=1 h_to_v_dat_inject_stalls=3
PERF_RBRG id=1 v_to_h_req_packets=1 v_to_h_req_bytes=16 v_to_h_req_queue_peak=1 v_to_h_req_queue_full_stalls=0 v_to_h_req_inject_stalls=0 v_to_h_dat_packets=0 v_to_h_dat_bytes=0 v_to_h_dat_queue_peak=0 v_to_h_dat_queue_full_stalls=0 v_to_h_dat_inject_stalls=0 h_to_v_rsp_packets=1 h_to_v_rsp_bytes=8 h_to_v_rsp_queue_peak=1 h_to_v_rsp_queue_full_stalls=0 h_to_v_rsp_inject_stalls=0 h_to_v_dat_packets=1 h_to_v_dat_bytes=128 h_to_v_dat_queue_peak=1 h_to_v_dat_queue_full_stalls=0 h_to_v_dat_inject_stalls=1
PERF_FANOUT_CROSS_RING logical_recipients=10 h_ring_carriers=4 v_ring_carriers=3 h_ring_saved_packets=6 v_ring_saved_packets=7 total_segment_packets_saved=13
"""


def multi_ring_perf_block():
    return perf_block("multi_ring", 128, 8.0).replace(
        "PERF_HOME_AGENT",
        multi_ring_records() + "PERF_HOME_AGENT",
    )


def aggregation_wave_perf_block():
    return (
        perf_block("wave_shared_128b", 128, 1.0)
        .replace(
            "burst_bytes=128",
            "burst_bytes=128 run_mode=aggregation_wave "
            "max_aicore_per_vring=8 home_agent_waiters_per_entry=18 "
            "l2_response_latency=256",
        )
        .replace(
            "scaling_efficiency=0.900000",
            "scaling_efficiency=0.000000 scaling_efficiency_available=0",
        )
        .replace(
            "write_hazard_stalls=0",
            "write_hazard_stalls=0 rd_merged_pending=56 "
            "rd_merged_inflight=0 rd_merged_responding=0 "
            "table_full_stalls=0 waiter_full_stalls=0 "
            "aggregation_closed_stalls=0",
        )
        .replace("carrier_512b=0", "carrier_512b=0 carrier_other=0")
        .replace(
            "PERF_THEORY total_useful_bytes=1048576",
            "PERF_THEORY_NO_MERGE total_useful_bytes=1048576 "
            "physical_packets=128 fabric_min_cycles=131072 "
            "fabric_ceiling_bpc=8.000000 "
            "measured_over_fabric_ceiling=0.125000 "
            "assumption=finite_trace_packet_slot_upper_bound\n"
            "PERF_THEORY_IDEAL_MERGE total_useful_bytes=1048576 "
            "logical_read_requests=64 backend_reads=8 "
            "backend_read_saved=56 h_carriers=8 "
            "h_unicast_carriers=0 h_multicast_carriers=8 "
            "h_scatter_carriers=0 h_carrier_recipients=64 v_carriers=8",
        )
        .replace(
            "PERF_RESULT status=PASS",
            "PERF_THEORY total_useful_bytes=1048576 physical_packets=8 "
            "fabric_min_cycles=65536 fabric_ceiling_bpc=16.0 "
            "measured_over_fabric_ceiling=0.500000 "
            "assumption=finite_trace_packet_slot_upper_bound\n"
            "PERF_RESULT status=PASS",
        )
    )


def single_scenario_metric_records():
    return """\
PERF_MEASUREMENT start_cycle=10 end_cycle=109 window_cycles=100 measurement_valid=1
PERF_HW_CHANNEL domain=v ring=0 subnet=dat direction=cw window_cycles=100 edge_count=4 busy_cycles=200 cycle_util_pct=50 payload_util_pct=40 serialization_efficiency_pct=80 imbalance_pct=10
PERF_HW_CHANNEL domain=v ring=0 subnet=req direction=ccw window_cycles=100 edge_count=4 busy_cycles=20 cycle_util_pct=5 payload_util_pct=2.5 serialization_efficiency_pct=50 imbalance_pct=10
PERF_RING_EDGE domain=v ring=0 subnet=dat direction=cw src_station=1 dst_station=2 cycle_util_pct=75 payload_util_pct=60
PERF_RING_EDGE domain=v ring=0 subnet=req direction=ccw src_station=2 dst_station=1 cycle_util_pct=10 payload_util_pct=5
PERF_RING_BUFFER node_type=master node=1 subnet=dat side=eject direction=shared depth=8 peak=8 avg_occupancy_pct=62.5 full_cycles=12 full_pct=12.5
PERF_RING_BUFFER node_type=target node=0 subnet=req side=inject direction=cw depth=4 peak=2 avg_occupancy_pct=25 full_cycles=0 full_pct=0
PERF_DEFLECTION domain=v ring=0 subnet=dat events=9 unique_packets=3 completed_packets=3 avg_rounds=3 max_rounds=4 avg_delay_cycles=18 max_delay_cycles=24 fanout_recipient_retry_events=2
PERF_DEFLECTION domain=v ring=0 subnet=req events=0 unique_packets=0 completed_packets=0 avg_rounds=0 max_rounds=0 avg_delay_cycles=0 max_delay_cycles=0 fanout_recipient_retry_events=0
PERF_RBRG_CHANNEL rbrg=0 path=h_to_v_dat cycle_util_pct=55 payload_util_pct=50 queue_peak=4 queue_full_stalls=2 destination_inject_stalls=1
PERF_RBRG_CHANNEL id=1 path=v_to_h_req cycle_util_pct=10 payload_util_pct=8 queue_peak=1 queue_full_stalls=0 destination_inject_stalls=0
"""


def single_scenario_metric_perf_block():
    return perf_block("single_metric", 128, 8.0).replace(
        "PERF_HOME_AGENT",
        single_scenario_metric_records() + "PERF_HOME_AGENT",
    )


def current_ring_perf_block():
    return (
        perf_block("current_ring", 128, 8.0)
        .replace(
            " hottest_subnet=dat hottest_src_station=1 hottest_direction=cw "
            "hottest_cycles=8192",
            "",
        )
        .replace(
            "PERF_HOME_AGENT",
            "PERF_HW_CHANNEL domain=v ring=0 subnet=dat direction=cw "
            "window_cycles=100 edge_count=4 width_bytes=16 packets=64 "
            "bytes=1024 busy_cycles=50 downstream_register_full_stalls=1 "
            "serialization_busy_stalls=2 pipeline_full_stalls=0 "
            "send_reject_stalls=0 stalls=3 cycle_util_pct=12.5 "
            "payload_util_pct=10 serialization_efficiency_pct=80 "
            "cw_busy_cycles=50 ccw_busy_cycles=10 subnet_imbalance_pct=66.7\n"
            "PERF_RING_EDGE domain=v ring=0 subnet=dat direction=cw "
            "src_station=1 dst_station=2 window_cycles=100 width_bytes=16 "
            "packets=64 bytes=1024 busy_cycles=50 "
            "serialization_busy_stalls=2 stalls=3 inflight_peak=1 "
            "cycle_util_pct=50 payload_util_pct=40 "
            "serialization_efficiency_pct=80\nPERF_HOME_AGENT",
        )
    )


def fixed_topology_perf_block():
    channel_records = []
    edge_records = []
    for domain, ring, station_count in (("h", 0, 10), ("v", 0, 5), ("v", 1, 5)):
        for subnet in ("req", "rsp", "dat"):
            for direction in ("cw", "ccw"):
                channel_records.append(
                    "PERF_HW_CHANNEL domain={} ring={} subnet={} direction={} "
                    "window_cycles=100 edge_count={} width_bytes=128 packets=8 "
                    "bytes=1024 busy_cycles=40 cycle_util_pct=40 "
                    "payload_util_pct=40 serialization_efficiency_pct=100 "
                    "cw_busy_cycles=40 ccw_busy_cycles=20 subnet_imbalance_pct=33.3".format(
                        domain, ring, subnet, direction, station_count
                    )
                )
            for station in range(station_count):
                next_station = (station + 1) % station_count
                edge_records.append(
                    "PERF_RING_EDGE domain={} ring={} subnet={} direction=cw "
                    "src_station={} dst_station={} window_cycles=100 "
                    "width_bytes=128 packets=8 bytes=1024 busy_cycles={} "
                    "serialization_busy_stalls=2 stalls=3 inflight_peak=1 "
                    "cycle_util_pct={} payload_util_pct={} "
                    "serialization_efficiency_pct=100".format(
                        domain,
                        ring,
                        subnet,
                        station,
                        next_station,
                        80 if subnet == "dat" else 20,
                        80 if subnet == "dat" else 20,
                        80 if subnet == "dat" else 10,
                    )
                )

    buffer_records = []
    for node_type, count in (("master", 8), ("home_agent", 4), ("l2_buffer", 4)):
        for node in range(count):
            buffer_records.append(
                "PERF_RING_BUFFER node_type={} node={} subnet=dat side=inject "
                "direction=cw depth=12 pushes=64 pops=64 push_rejects=2 "
                "occupancy=0 peak=8 occupancy_area=400 avg_occupancy_pct=33.3 "
                "full_cycles=4 full_pct=4".format(node_type, node)
            )

    rbrg_records = []
    for rbrg in range(2):
        rbrg_records.append(
            "PERF_RBRG_CHANNEL id={} path=h_to_v_dat window_cycles=100 "
            "width_bytes=128 packets=16 bytes=2048 busy_cycles=80 "
            "cycle_util_pct=80 payload_util_pct=80 "
            "serialization_efficiency_pct=100 queue_peak=8 "
            "queue_full_stalls=2 destination_inject_stalls=3".format(rbrg)
        )

    ha_source_records = [
        "PERF_HA_SOURCE ha=0 master=0 rd_packets=64 wr_packets=4 total_packets=68",
        "PERF_HA_SOURCE ha=0 master=3 rd_packets=32 wr_packets=0 total_packets=32",
        "PERF_HA_SOURCE ha=1 master=4 rd_packets=16 wr_packets=8 total_packets=24",
    ]

    records = "\n".join(
        channel_records
        + edge_records
        + buffer_records
        + rbrg_records
        + ha_source_records
    )
    return (
        perf_block("fixed_topology", 128, 160.0, ceiling_bpc=256.0)
        .replace(
            "burst_bytes=128",
            "burst_bytes=128 run_mode=free_running max_aicore_per_vring=4 "
            "home_agent_waiters_per_entry=8 l2_response_latency=64",
        )
        .replace("PERF_HOME_AGENT", records + "\nPERF_HOME_AGENT")
    )


SAMPLE_RESULTS = (
    "[==========] Running 2 tests from 1 test suite.\n"
    + perf_block("shared_16b", 16, 4.25)
    + "[       OK ] RingPerfBenchmark.Shared16B\n"
    + perf_block("shared_128b", 128, 8.5)
)


class RingPerfParserTest(unittest.TestCase):
    def test_parses_multiple_scenarios_in_input_order(self):
        scenarios = parse_perf_results(SAMPLE_RESULTS)

        self.assertEqual(
            [scenario.case_name for scenario in scenarios],
            ["shared_16b", "shared_128b"],
        )
        self.assertEqual(scenarios[0].number("CONFIG", "burst_bytes"), 16)
        self.assertEqual(
            scenarios[1].number("BANDWIDTH", "end_to_end_bpc"), 8.5
        )
        self.assertEqual(scenarios[0].order, 0)
        self.assertIn("PERF_RESULT status=PASS", scenarios[0].raw_text)

    def test_preserves_repeated_multiring_records_in_input_order(self):
        scenario = parse_perf_results(multi_ring_perf_block())[0]

        self.assertEqual(
            [record["id"] for record in scenario.records("RING_DOMAIN")],
            ["0", "0", "1"],
        )
        self.assertEqual(
            [record["type"] for record in scenario.records("RING_DOMAIN")],
            ["h", "v", "v"],
        )
        self.assertEqual(
            [record["id"] for record in scenario.records("RBRG")], ["0", "1"]
        )
        self.assertEqual(
            scenario.value("FANOUT_CROSS_RING", "v_ring_carriers"), "3"
        )

    def test_parses_aggregation_mode_and_dual_theory_sections(self):
        scenario = parse_perf_results(aggregation_wave_perf_block())[0]

        self.assertEqual(
            scenario.value("CONFIG", "run_mode"), "aggregation_wave"
        )
        self.assertEqual(
            scenario.number("CONFIG", "l2_response_latency"), 256
        )
        self.assertEqual(
            scenario.number("HOME_AGENT", "rd_merged_pending"), 56
        )
        self.assertEqual(
            scenario.number("L2_BUFFER", "carrier_other"), 0
        )
        self.assertEqual(
            scenario.number("THEORY_NO_MERGE", "physical_packets"), 128
        )

    def test_preserves_single_scenario_metric_records_independently(self):
        scenario = parse_perf_results(single_scenario_metric_perf_block())[0]

        self.assertEqual(scenario.value("MEASUREMENT", "window_cycles"), "100")
        self.assertEqual(
            [(record["subnet"], record["direction"])
             for record in scenario.records("HW_CHANNEL")],
            [("dat", "cw"), ("req", "ccw")],
        )
        self.assertEqual(
            [(record["src_station"], record["dst_station"])
             for record in scenario.records("RING_EDGE")],
            [("1", "2"), ("2", "1")],
        )
        endpoint_buffers = scenario.records("RING_BUFFER")
        self.assertEqual(
            [(record["node_type"], record["node"])
             for record in endpoint_buffers],
            [("master", "1"), ("target", "0")],
        )
        self.assertEqual(
            [(record["subnet"], record["side"], record["direction"],
              record["depth"], record["peak"],
              record["avg_occupancy_pct"], record["full_cycles"],
              record["full_pct"])
             for record in endpoint_buffers],
            [
                ("dat", "eject", "shared", "8", "8", "62.5", "12", "12.5"),
                ("req", "inject", "cw", "4", "2", "25", "0", "0"),
            ],
        )
        self.assertNotIn("domain", endpoint_buffers[0])
        self.assertNotIn("ring", endpoint_buffers[0])
        self.assertEqual(
            [record["subnet"] for record in scenario.records("DEFLECTION")],
            ["dat", "req"],
        )
        self.assertEqual(
            [(record.get("rbrg", record.get("id")), record["path"])
             for record in scenario.records("RBRG_CHANNEL")],
            [("0", "h_to_v_dat"), ("1", "v_to_h_req")],
        )

    def test_rejects_missing_required_section(self):
        text = perf_block("missing_latency", 128, 8.5).replace(
            "PERF_LATENCY p50=20 p95=30 p99=40 max=50\n", ""
        )

        with self.assertRaisesRegex(PerfReportError, "missing.*LATENCY"):
            parse_perf_results(text)

    def test_rejects_duplicate_case_name(self):
        text = perf_block("duplicate", 16, 4.0) + perf_block(
            "duplicate", 128, 8.0
        )

        with self.assertRaisesRegex(PerfReportError, "duplicate case"):
            parse_perf_results(text)

    def test_rejects_duplicate_section(self):
        text = perf_block("duplicate_section", 128, 8.0).replace(
            "PERF_LATENCY p50=20 p95=30 p99=40 max=50\n",
            "PERF_LATENCY p50=20 p95=30 p99=40 max=50\n"
            "PERF_LATENCY p50=21 p95=31 p99=41 max=51\n",
        )

        with self.assertRaisesRegex(PerfReportError, "duplicate section.*LATENCY"):
            parse_perf_results(text)

    def test_rejects_unclosed_scenario(self):
        text = perf_block("unclosed", 128, 8.0).replace(
            "PERF_RESULT status=PASS protocol_errors=0\n", ""
        )

        with self.assertRaisesRegex(PerfReportError, "unclosed.*unclosed"):
            parse_perf_results(text)

    def test_rejects_invalid_required_number_during_parse(self):
        text = perf_block("invalid_number", 128, 8.0).replace(
            "burst_bytes=128", "burst_bytes=invalid"
        )

        with self.assertRaisesRegex(
            PerfReportError, "invalid_number.*CONFIG.burst_bytes"
        ):
            parse_perf_results(text)


class RingPerfHtmlTest(unittest.TestCase):
    def test_renders_fixed_topology_with_ring_closures(self):
        document = render_html(
            parse_perf_results(fixed_topology_perf_block()), "fixed.txt"
        )

        self.assertIn('id="ring-topology-0"', document)
        self.assertIn('data-ring="h0"', document)
        self.assertIn('data-ring="v0"', document)
        self.assertIn('data-ring="v1"', document)
        self.assertIn('data-node="master-0"', document)
        self.assertIn('data-node="l2_buffer-3"', document)
        self.assertIn('data-edge="h0-dat-cw-9-0"', document)
        self.assertIn('data-edge="v0-dat-cw-4-0"', document)

    def test_topology_encodes_edge_heat_and_node_details(self):
        document = render_html(
            parse_perf_results(fixed_topology_perf_block()), "fixed.txt"
        )

        self.assertIn('data-edge="v0-dat-cw-1-2"', document)
        self.assertIn("topology-edge-hot", document)
        self.assertIn("topology-edge-wide", document)
        self.assertIn('data-node-detail="master-0"', document)
        self.assertIn('data-node-detail="rbrg-0"', document)
        self.assertIn('data-rbrg-path="0-h_to_v_dat"', document)
        self.assertIn(
            'class="topology-subnet active" data-subnet="dat"', document
        )

    def test_topology_keeps_detailed_tables_collapsed(self):
        topology_document = render_html(
            parse_perf_results(fixed_topology_perf_block()), "fixed.txt"
        )
        unsupported_document = render_html(
            parse_perf_results(SAMPLE_RESULTS), "sample.txt"
        )

        self.assertIn('<details class="topology-data">', topology_document)
        self.assertIn("详细 Ring 数据", topology_document)
        self.assertIn("当前拓扑不受支持", unsupported_document)
        self.assertNotIn('id="ring-topology-0"', unsupported_document)

    def test_topology_uses_compact_closed_ring_layout(self):
        document = render_html(
            parse_perf_results(fixed_topology_perf_block()), "fixed.txt"
        )

        self.assertIn('class="topology-skeleton"', document)
        self.assertIn('viewBox="0 0 1200 620"', document)
        self.assertIn('class="topology-legend"', document)
        self.assertIn('aria-label="M0 · station 1"', document)
        self.assertIn('aria-label="L2_1 · station 3"', document)
        self.assertIn('aria-label="HA1 · station 4"', document)
        self.assertIn('aria-label="L2_3 · station 8"', document)
        self.assertIn('aria-label="HA3 · station 9"', document)
        self.assertIn(">M0</text>", document)
        self.assertNotIn(">M0 · st1</text>", document)

    def test_topology_node_detail_shows_accepted_inject_packets(self):
        document = render_html(
            parse_perf_results(fixed_topology_perf_block()), "fixed.txt"
        )

        self.assertIn("<th>Pushes</th>", document)
        self.assertIn(
            "<td>DAT inject</td><td>cw</td><td>8</td><td>4</td>"
            "<td>64</td><td>2</td>",
            document,
        )

    def test_home_agent_node_detail_shows_request_sources_by_master(self):
        document = render_html(
            parse_perf_results(fixed_topology_perf_block()), "fixed.txt"
        )

        ha0_start = document.index('data-node-detail="home_agent-0"')
        ha0_end = document.index("</div></div>", ha0_start)
        ha0_detail = document[ha0_start:ha0_end]
        self.assertIn("REQ 来源 Master", ha0_detail)
        self.assertIn("<th>Master</th><th>RD</th><th>WR</th>", ha0_detail)
        self.assertIn("<td>M0</td><td>64</td><td>4</td><td>68</td>", ha0_detail)
        self.assertIn("<td>M3</td><td>32</td><td>0</td><td>32</td>", ha0_detail)
        self.assertNotIn("<td>M4</td>", ha0_detail)

    def test_details_table_aligns_the_four_latency_columns(self):
        document = render_html(parse_perf_results(SAMPLE_RESULTS), "sample.txt")

        details_start = document.index('<section id="details">')
        details_end = document.index("</section>", details_start)
        details = document[details_start:details_end]
        self.assertIn('<th colspan="4">延迟</th>', details)
        self.assertIn("<td>20</td><td>30</td><td>40</td><td>50</td>", details)
        self.assertNotIn("<td>4.250/9.000/20/30/40</td>", details)

        header = re.search(r"<thead><tr>(.*?)</tr></thead>", details).group(1)
        row = re.search(r"<tbody><tr>(.*?)</tr>", details).group(1)
        header_columns = sum(
            int(match.group(1) or 1)
            for match in re.finditer(r"<th(?: colspan=\"(\d+)\")?>", header)
        )
        row_columns = len(re.findall(r"<t[hd](?: [^>]*)?>", row))
        self.assertEqual(header_columns, row_columns)

    def test_renders_one_offline_comparison_report(self):
        document = render_html(
            parse_perf_results(SAMPLE_RESULTS), "ring_perf_result.txt"
        )

        self.assertIn("Ring 性能横向对比", document)
        self.assertIn("带宽与 Fabric 上限", document)
        self.assertIn("延迟分布", document)
        self.assertIn("Shared read 粒度收益", document)
        self.assertIn("瓶颈迁移", document)
        self.assertIn("H-Ring 多播载体", document)
        self.assertIn("H-Ring 载体接收者", document)
        self.assertIn("完整数据", document)
        self.assertIn("shared_16b", document)
        self.assertIn("shared_128b", document)
        self.assertIn("4.250", document)
        self.assertIn("8.500", document)
        self.assertNotIn("http://", document)
        self.assertNotIn("https://", document)
        self.assertFalse(has_external_script(document))
        self.assertIn("此日志中不可用", document)

    def test_renders_selected_multiring_profile_from_snapshot_records(self):
        document = render_html(
            parse_perf_results(multi_ring_perf_block()), "multi_ring.txt"
        )

        self.assertIn("H-Ring 与 V-Ring 双向 busy cycles", document)
        self.assertIn("H0 最热连接", document)
        self.assertIn("V1 最热连接", document)
        self.assertIn("RBRG 0 四路径", document)
        self.assertIn("h_to_v_dat", document)
        self.assertIn("逻辑接收者", document)
        self.assertIn("H-Ring 载体（L2）", document)
        self.assertIn("V-Ring 载体（RBRG H_TO_V_DAT）", document)
        self.assertIn("段包节省", document)
        self.assertFalse(has_external_script(document))

    def test_separates_aggregation_validation_from_throughput_comparison(self):
        scenarios = parse_perf_results(
            perf_block("free_running_read", 128, 8.0)
            + aggregation_wave_perf_block()
        )
        document = render_html(scenarios, "mixed.txt")

        self.assertIn("同步聚合验证", document)
        self.assertIn("wave_shared_128b", document)
        self.assertIn("预期 / 实际 H carrier", document)
        self.assertIn("N/A", document)
        bandwidth = document.split('<section id="bandwidth">', 1)[1].split(
            "</section>", 1
        )[0]
        self.assertIn("free_running_read", bandwidth)
        self.assertNotIn("wave_shared_128b", bandwidth)

    def test_external_script_detector_allows_inline_script(self):
        self.assertFalse(
            has_external_script("<script>window.ready = true;</script>")
        )
        self.assertFalse(
            has_external_script(
                '<script data-src="inline-hint">window.ready = true;</script>'
            )
        )

    def test_external_script_detector_rejects_src_in_any_position_or_case(self):
        external_tags = (
            '<script src="report.js"></script>',
            '<script defer src="report.js"></script>',
            '<script data-x="1" src="report.js"></script>',
            '<SCRIPT DATA-X="1" SRC="report.js"></SCRIPT>',
        )
        for tag in external_tags:
            with self.subTest(tag=tag):
                self.assertTrue(has_external_script(tag))

    def test_escapes_source_and_case_names(self):
        scenarios = parse_perf_results(
            perf_block("<case&name>", 128, 8.0)
        )

        document = render_html(scenarios, "<source&name>.txt")

        self.assertIn("&lt;case&amp;name&gt;", document)
        self.assertIn("&lt;source&amp;name&gt;.txt", document)
        self.assertNotIn("<case&name>", document)

    def test_zero_fabric_ceiling_does_not_render_non_finite_values(self):
        scenarios = parse_perf_results(
            perf_block("zero_ceiling", 128, 0.0, ceiling_bpc=0.0)
        )

        document = render_html(scenarios, "zero.txt")

        self.assertIn("无有效理论上限", document)
        self.assertNotIn("NaN", document)
        self.assertNotIn("Infinity", document)

    def test_renders_single_scenario_metric_sections(self):
        document = render_html(
            parse_perf_results(single_scenario_metric_perf_block()),
            "single_metric.txt",
        )

        self.assertIn("Physical Channel Utilization", document)
        self.assertIn("Per-Edge Hotspots", document)
        self.assertIn("Endpoint Buffer", document)
        self.assertIn("<th>Node</th>", document)
        self.assertNotIn("<th>Ring</th><th>Node</th>", document)
        self.assertIn("<th>master 1</th>", document)
        self.assertIn("<td>DAT eject</td>", document)
        self.assertIn("<td>shared</td>", document)
        self.assertIn("<td>8</td>", document)
        self.assertIn("<td>62.500</td>", document)
        self.assertIn("<td>12</td>", document)
        self.assertIn("<td>12.500</td>", document)
        self.assertIn("Deflection Recovery", document)
        self.assertIn("RBRG Channel", document)
        self.assertIn("cycle utilization", document)
        self.assertIn("payload utilization", document)
        self.assertIn("h_to_v_dat", document)

    def test_renders_current_ring_records_without_legacy_hottest_summary(self):
        scenario = parse_perf_results(current_ring_perf_block())[0]
        document = render_html([scenario], "current_ring.txt")

        self.assertIsNone(scenario.value("RING", "hottest_subnet"))
        self.assertIsNone(scenario.value("RING", "hottest_direction"))
        self.assertIsNone(scenario.value("RING", "hottest_cycles"))
        self.assertEqual(len(scenario.records("HW_CHANNEL")), 1)
        self.assertEqual(len(scenario.records("RING_EDGE")), 1)
        self.assertIn("Physical Channel Utilization", document)
        self.assertIn("Per-Edge Hotspots", document)
        self.assertIn(
            "Physical Channel Utilization / Per-Edge Hotspots", document
        )

    def test_omits_legacy_global_deflect_and_keeps_detail(self):
        document = render_html(
            parse_perf_results(single_scenario_metric_perf_block()),
            "single_metric.txt",
        )

        self.assertNotIn(">Deflect<", document)
        self.assertIn("Deflection Recovery", document)

    def test_renders_rbrg_destination_inject_stalls_column(self):
        document = render_html(
            parse_perf_results(single_scenario_metric_perf_block()),
            "single_metric.txt",
        )
        rbrg_body = document.split(
            "<th>Destination inject stalls</th></tr></thead><tbody>", 1
        )[1]
        rbrg_row = rbrg_body.split("</tr>", 1)[0]

        self.assertIn("<td>1</td>", rbrg_row)
        self.assertEqual(rbrg_row.count("<th>") + rbrg_row.count("<td>"), 7)

    def test_renders_no_activity_when_optional_metric_families_are_missing(self):
        sparse = perf_block("sparse_metric", 128, 8.0).replace(
            "PERF_HOME_AGENT",
            "PERF_MEASUREMENT start_cycle=10 end_cycle=109 "
            "window_cycles=100 measurement_valid=1\n"
            "PERF_HW_CHANNEL domain=v ring=0 subnet=dat direction=cw "
            "window_cycles=100 edge_count=4 busy_cycles=0 cycle_util_pct=0 "
            "payload_util_pct=0 serialization_efficiency_pct=0 "
            "imbalance_pct=0\nPERF_HOME_AGENT",
        )

        document = render_html(parse_perf_results(sparse), "sparse.txt")

        self.assertIn("Physical Channel Utilization", document)
        self.assertIn("Deflection Recovery", document)
        self.assertIn("RBRG Channel", document)
        self.assertIn("No activity", document)


class RingPerfFileTest(unittest.TestCase):
    def test_generate_report_uses_input_stem_by_default(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "ring_perf_result.txt"
            input_path.write_text(SAMPLE_RESULTS, encoding="utf-8")

            generated = generate_report(input_path)

            self.assertEqual(
                generated, input_path.with_name("ring_perf_result.html")
            )
            self.assertTrue(generated.is_file())
            self.assertIn(
                "<!doctype html>",
                generated.read_text(encoding="utf-8").lower(),
            )

    def test_generate_report_accepts_explicit_output_path(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "input.txt"
            output_path = Path(directory) / "comparison.html"
            input_path.write_text(SAMPLE_RESULTS, encoding="utf-8")

            generated = generate_report(input_path, output_path)

            self.assertEqual(generated, output_path)
            self.assertTrue(output_path.is_file())

    def test_cli_returns_nonzero_and_explains_invalid_input(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "invalid.txt"
            input_path.write_text("no PERF records\n", encoding="utf-8")
            errors = io.StringIO()

            with redirect_stderr(errors):
                status = main([str(input_path)])

            self.assertEqual(status, 1)
            self.assertIn("error:", errors.getvalue())
            self.assertIn("no complete PERF scenarios", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
