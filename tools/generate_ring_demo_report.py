#!/usr/bin/env python3
"""Generate a portable, offline HTML report from a Ring demo result file."""

import argparse
import html
import re
from pathlib import Path


_KEY_VALUE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
_MAX_BUCKET = 64


def _first_record(records, name):
    values = records.get(name, [])
    return values[0] if values else {}


def _as_int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _parse_buckets(value):
    buckets = {}
    if not value or value == "none":
        return buckets
    for item in value.split(","):
        if ":" not in item:
            continue
        key, count = item.split(":", 1)
        key = key.rstrip("+")
        bucket = _as_int(key, -1)
        if bucket < 1:
            continue
        bucket = min(bucket, _MAX_BUCKET)
        buckets[bucket] = buckets.get(bucket, 0) + max(0, _as_int(count))
    return buckets


def _parse_key_values(line):
    return {match.group(1): match.group(2)
            for match in _KEY_VALUE.finditer(line)}


def parse_result(text):
    """Parse the machine-readable TEST_* lines in a demo result."""
    records = {}
    for line in text.splitlines():
        if not line.startswith("TEST_"):
            continue
        name, _, _ = line.partition(" ")
        records.setdefault(name, []).append(_parse_key_values(line))

    home_agent = _first_record(records, "TEST_HOME_AGENT")
    l2_traffic = _first_record(records, "TEST_L2_TRAFFIC")
    stalls = _first_record(records, "TEST_STALLS")
    result = _first_record(records, "TEST_RESULT")
    return {
        "raw_text": text,
        "records": records,
        "ha_waiter_buckets": _parse_buckets(
            _first_record(records, "TEST_HA_WAITER_DIST").get("buckets")
        ),
        "l2_recipient_buckets": _parse_buckets(
            _first_record(records, "TEST_L2_RECIPIENT_DIST").get("buckets")
        ),
        "carrier_counts": {
            "bytes_128": _as_int(
                _first_record(records, "TEST_L2_CARRIER_DIST").get("bytes_128")
            ),
            "bytes_256": _as_int(
                _first_record(records, "TEST_L2_CARRIER_DIST").get("bytes_256")
            ),
            "bytes_512": _as_int(
                _first_record(records, "TEST_L2_CARRIER_DIST").get("bytes_512")
            ),
            "other": _as_int(
                _first_record(records, "TEST_L2_CARRIER_DIST").get("other")
            ),
        },
        "home_agent": home_agent,
        "l2_traffic": l2_traffic,
        "stalls": stalls,
        "result": result,
    }


def _distribution_stats(buckets):
    entries = sum(buckets.values())
    weighted = sum(bucket * count for bucket, count in buckets.items())
    average = weighted / entries if entries else 0.0
    return entries, weighted, average


def _format_number(value):
    if isinstance(value, float):
        return "{:.3f}".format(value)
    return "{:,}".format(value)


def _metric(label, value, note=""):
    return (
        '<div class="metric">'
        '<div class="metric-label">{}</div>'
        '<div class="metric-value">{}</div>'
        '<div class="metric-note">{}</div>'
        '</div>'
    ).format(html.escape(label), html.escape(str(value)), html.escape(note))


def _bar_chart(title, subtitle, values, label_suffix=""):
    if not values:
        return (
            '<section class="panel">'
            '<h2>{}</h2><p class="muted">{}</p>'
            '<div class="empty">无分布数据</div></section>'
        ).format(html.escape(title), html.escape(subtitle))

    maximum = max(values.values())
    rows = []
    for bucket in sorted(values):
        count = values[bucket]
        label = "{}{}".format(bucket, "+" if bucket == _MAX_BUCKET else label_suffix)
        width = 0.0 if maximum == 0 else 100.0 * count / maximum
        rows.append(
            (
                '<div class="bar-row">'
                '<div class="bar-label">{}</div>'
                '<div class="bar-track"><div class="bar-fill" style="width:{:.2f}%"></div></div>'
                '<div class="bar-count">{}</div>'
                '</div>'
            ).format(
                html.escape(label), width, html.escape(_format_number(count))
            )
        )
    return (
        '<section class="panel">'
        '<h2>{}</h2><p class="muted">{}</p>'
        '<div class="bar-list">{}</div></section>'
    ).format(html.escape(title), html.escape(subtitle), "".join(rows))


def _carrier_chart(values):
    labels = {
        "bytes_128": "128B",
        "bytes_256": "256B",
        "bytes_512": "512B",
        "other": "其他",
    }
    return _bar_chart(
        "Ring carrier 大小分布",
        "每个成功注入 DAT 物理包的实际承载大小。",
        {labels[key]: value for key, value in values.items() if value},
        label_suffix="",
    )


def render_html(parsed, source_name):
    """Render parsed result data as one self-contained HTML document."""
    ha_entries, _, ha_average = _distribution_stats(
        parsed["ha_waiter_buckets"]
    )
    ring_packets, ring_recipients, ring_average = _distribution_stats(
        parsed["l2_recipient_buckets"]
    )
    reduction = (
        100.0 * (1.0 - ring_packets / ring_recipients)
        if ring_recipients
        else 0.0
    )
    carrier_total = sum(parsed["carrier_counts"].values())
    status = parsed["result"].get("status", "UNKNOWN")
    dominant = parsed["stalls"].get("dominant", "unknown")
    home_agent = parsed["home_agent"]
    l2_traffic = parsed["l2_traffic"]

    cards = "".join(
        [
            _metric("HA 已完成 transaction", _format_number(ha_entries),
                    "按最终 waiter 数计数"),
            _metric("HA 平均 waiter 数", _format_number(ha_average),
                    "只统计已完成 transaction"),
            _metric("Ring 物理 DAT 包", _format_number(ring_packets),
                    "只统计 push_inject 成功"),
            _metric("Ring 平均接收者数", _format_number(ring_average),
                    "一个物理包服务的 recipient 数"),
            _metric("物理包减少比例", "{}%".format(_format_number(reduction)),
                    "逻辑 recipient 相对物理包"),
            _metric("carrier 总数", _format_number(carrier_total),
                    "应与物理 DAT 包数对应"),
        ]
    )

    config = _first_record(parsed["records"], "TEST_CONFIG")
    config_note = "；".join(
        [
            "场景={}".format(config.get("case", "unknown")),
            "masters={}".format(config.get("masters", "unknown")),
            "sector={}".format(config.get("sector_size", "unknown")) + "B",
            "burst={}".format(config.get("core_burst_bytes", "unknown")) + "B",
        ]
    )
    raw_text = html.escape(parsed["raw_text"])
    status_class = "ok" if status.upper() == "PASS" else "warn"
    template = """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Ring Demo 分布报告</title>
<style>
:root{--ink:#172033;--muted:#667085;--paper:#f5f7fb;--panel:#fff;
--blue:#3478f6;--cyan:#25a7a0;--gold:#e3a32b;--line:#e4e8f0}
*{box-sizing:border-box}body{margin:0;background:var(--paper);color:var(--ink);
font:15px/1.55 "Segoe UI","Microsoft YaHei",Arial,sans-serif}
.wrap{max-width:1180px;margin:0 auto;padding:28px 22px 48px}
.hero{padding:28px 30px;border-radius:22px;color:#fff;
background:linear-gradient(135deg,#172b57,#2466c7 62%,#23a6a1);
box-shadow:0 14px 38px #17356b2b}
.hero h1{margin:0 0 8px;font-size:30px;letter-spacing:.02em}
.hero p{margin:5px 0;color:#e7f0ff}.status{display:inline-block;margin-top:15px;
padding:4px 12px;border-radius:99px;background:#ffffff25;font-weight:700}
.status.ok{background:#b9f2d455}.status.warn{background:#ffd98b66}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin:18px 0}
.metric,.panel{background:var(--panel);border:1px solid var(--line);
border-radius:16px;box-shadow:0 5px 16px #15254d0a}
.metric{padding:17px 18px}.metric-label{color:var(--muted);font-size:13px}
.metric-value{margin-top:3px;font-size:26px;font-weight:750;color:#1c4d9b}
.metric-note{color:var(--muted);font-size:12px;margin-top:2px}
.charts{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.panel{padding:20px}.panel h2{margin:0 0 3px;font-size:19px}
.muted{margin:0 0 17px;color:var(--muted);font-size:13px}
.bar-row{display:grid;grid-template-columns:54px 1fr 66px;gap:10px;
align-items:center;margin:10px 0}.bar-label{font-weight:650;color:#42526b}
.bar-track{height:12px;background:#e9eef7;border-radius:99px;overflow:hidden}
.bar-fill{height:100%;min-width:3px;border-radius:99px;
background:linear-gradient(90deg,var(--blue),var(--cyan))}
.bar-count{text-align:right;color:var(--muted);font-variant-numeric:tabular-nums}
.empty{padding:28px 12px;text-align:center;color:var(--muted);
border:1px dashed #cbd3e1;border-radius:12px}
.wide{grid-column:1/-1}.facts{display:flex;flex-wrap:wrap;gap:9px;margin-top:12px}
.fact{padding:7px 11px;border-radius:10px;background:#f0f4fa;color:#45546b}
details{margin-top:16px;background:var(--panel);border:1px solid var(--line);
border-radius:14px;padding:14px 18px}summary{cursor:pointer;font-weight:700}
pre{white-space:pre-wrap;overflow:auto;margin:12px 0 0;padding:14px;
background:#101828;color:#d9e5ff;border-radius:10px;font:12px/1.5 Consolas,monospace}
footer{color:var(--muted);font-size:12px;text-align:center;margin-top:22px}
@media(max-width:820px){.grid,.charts{grid-template-columns:1fr}.wide{grid-column:auto}}
@media print{body{background:#fff}.hero{box-shadow:none}.metric,.panel{box-shadow:none}}
</style>
</head>
<body>
<main class="wrap">
<header class="hero">
  <h1>Ring Demo 分布报告</h1>
  <p>__CONFIG_NOTE__</p>
  <p>报告从机器可解析的 TEST_* 文本生成；无需网络、无需 Python、无需安装依赖。</p>
  <span class="status __STATUS_CLASS__">结果：__STATUS__</span>
</header>
<section class="grid">__CARDS__</section>
<section class="charts">
__HA_CHART__
__RING_CHART__
__CARRIER_CHART__
</section>
<section class="panel wide" style="margin-top:16px">
  <h2>运行摘要</h2>
  <div class="facts">
    <span class="fact">主要瓶颈：__DOMINANT__</span>
    <span class="fact">HA backend reads：__BACKEND_READS__</span>
    <span class="fact">L2 accepted：__L2_ACCEPTED__</span>
    <span class="fact">L2 injected：__L2_INJECTED__</span>
    <span class="fact">carrier 分类总数：__CARRIER_TOTAL__</span>
  </div>
</section>
<details>
  <summary>查看原始结果文本</summary>
  <pre>__RAW_TEXT__</pre>
</details>
<footer>源文件：__SOURCE_NAME__ · 单文件离线报告</footer>
</main>
</body>
</html>
"""
    return (
        template
        .replace("__CONFIG_NOTE__", html.escape(config_note))
        .replace("__STATUS_CLASS__", status_class)
        .replace("__STATUS__", html.escape(status))
        .replace("__CARDS__", cards)
        .replace(
            "__HA_CHART__",
            _bar_chart(
                "HA transaction 聚合分布（waiter）",
                "每个已完成 transaction 最终聚合的请求数；64 表示 64 个及以上。",
                parsed["ha_waiter_buckets"],
            ),
        )
        .replace(
            "__RING_CHART__",
            _bar_chart(
                "Ring 物理包接收者分布",
                "每个成功注入 DAT 物理包最终服务的 recipient 数；64 表示 64 个及以上。",
                parsed["l2_recipient_buckets"],
            ),
        )
        .replace("__CARRIER_CHART__", _carrier_chart(parsed["carrier_counts"]))
        .replace("__DOMINANT__", html.escape(dominant))
        .replace(
            "__BACKEND_READS__",
            html.escape(home_agent.get("backend_reads", "0")),
        )
        .replace(
            "__L2_ACCEPTED__",
            html.escape(l2_traffic.get("l2_buffer_accepted", "0")),
        )
        .replace(
            "__L2_INJECTED__",
            html.escape(l2_traffic.get("l2_buffer_injected", "0")),
        )
        .replace("__CARRIER_TOTAL__", html.escape(str(carrier_total)))
        .replace("__SOURCE_NAME__", html.escape(str(source_name)))
        .replace("__RAW_TEXT__", raw_text)
    )


def generate_report(input_path, output_path=None):
    input_path = Path(input_path)
    if output_path is None:
        output_path = input_path.with_suffix(".html")
    output_path = Path(output_path)
    parsed = parse_result(input_path.read_text(encoding="utf-8"))
    output_path.write_text(
        render_html(parsed, input_path.name), encoding="utf-8"
    )
    return output_path


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate an offline HTML report from a Ring demo TXT result."
    )
    parser.add_argument("input", type=Path, help="TEST_* result text file")
    parser.add_argument(
        "output", type=Path, nargs="?",
        help="HTML output path; defaults to input path with .html suffix",
    )
    args = parser.parse_args(argv)
    generated = generate_report(args.input, args.output)
    print("generated {}".format(generated))
    return 0


if __name__ == "__main__":
    main()
