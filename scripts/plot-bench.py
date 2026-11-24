#!/usr/bin/env python3
"""Charts for the benchmark report, as standalone SVG files (Python standard library only).

    python3 scripts/plot-bench.py bench-results/run-YYYYMMDD-HHMMSS [OUT_DIR]

Reads the CSVs written by scripts/bench-experiments.sh and writes one SVG per chart into
OUT_DIR (default: RUN/charts). Every chart follows the same rules: one y-axis, a legend
whenever there are two or more series, direct labels on line ends, hover titles on every
mark, and colours that follow the stock mode (never its rank). The SVGs switch to dark
colours when the viewer prefers a dark theme.
"""
import csv
import html
import os
import statistics
import sys

# Stock mode -> categorical slot. Fixed, so a mode keeps its colour on every chart.
MODES = ["naive", "db-lock", "lockfree-shm"]
SLOT = {"naive": 1, "db-lock": 2, "lockfree-shm": 3}
LIGHT = {1: "#2a78d6", 2: "#eb6834", 3: "#1baf7a", 4: "#eda100", 8: "#e34948"}
DARK = {1: "#3987e5", 2: "#d95926", 3: "#199e70", 4: "#c98500", 8: "#e66767"}

STYLE = """
<style>
  .bg { fill: none; }
  .t1 { fill: #0b0b0b; font: 600 15px system-ui, -apple-system, 'Segoe UI', sans-serif; }
  .t2 { fill: #52514e; font: 12px system-ui, -apple-system, 'Segoe UI', sans-serif; }
  .t3 { fill: #52514e; font: 11px system-ui, -apple-system, 'Segoe UI', sans-serif; }
  .lab { fill: #0b0b0b; font: 600 11px system-ui, -apple-system, 'Segoe UI', sans-serif; }
  .grid { stroke: #e4e3de; stroke-width: 1; }
  .axis { stroke: #8a8984; stroke-width: 1; }
  .ref { stroke: #52514e; stroke-width: 1.5; stroke-dasharray: 5 4; }
  .gap { stroke: #ffffff; }
%s
  @media (prefers-color-scheme: dark) {
    .t1, .lab { fill: #f0efec; }
    .t2, .t3 { fill: #c3c2b7; }
    .grid { stroke: #3a3935; }
    .axis { stroke: #77766f; }
    .ref { stroke: #c3c2b7; }
    .gap { stroke: #1a1a19; }
%s
  }
</style>
"""


def series_css():
    light, dark = [], []
    for slot in LIGHT:
        light.append("  .s%d { fill: %s; stroke: %s; }" % (slot, LIGHT[slot], LIGHT[slot]))
        dark.append("    .s%d { fill: %s; stroke: %s; }" % (slot, DARK[slot], DARK[slot]))
    return "\n".join(light), "\n".join(dark)


def esc(s):
    return html.escape(str(s), quote=True)


def nice_step(v, ticks=4):
    """A round tick step (1, 2 or 5 x 10^k) giving about `ticks` intervals up to v."""
    if v <= 0:
        return 1
    raw = v / ticks
    exp = 1.0
    while exp * 10 <= raw:
        exp *= 10
    while exp > raw:
        exp /= 10
    for m in (1, 2, 5, 10):
        if m * exp >= raw:
            return m * exp
    return 10 * exp


def fmt(v):
    if v >= 1000 and v % 1000 == 0:
        return "%dk" % (v // 1000)
    if v >= 10 or v == int(v):
        return "%d" % round(v)
    return "%.1f" % v


class Svg:
    def __init__(self, w, h, title, subtitle):
        self.w, self.h = w, h
        self.parts = []
        light, dark = series_css()
        self.parts.append(
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
            'role="img" aria-label="%s">' % (w, h, w, h, esc(title)))
        self.parts.append(STYLE % (light, dark))
        self.parts.append('<title>%s</title>' % esc(title))
        self.text(16, 24, title, "t1")
        self.text(16, 42, subtitle, "t2")

    def text(self, x, y, s, cls, anchor="start", extra=""):
        self.parts.append('<text x="%.1f" y="%.1f" class="%s" text-anchor="%s"%s>%s</text>'
                          % (x, y, cls, anchor, extra, esc(s)))

    def add(self, s):
        self.parts.append(s)

    def legend(self, x, y, items):
        """items: [(slot, label)]; drawn in one row."""
        for slot, label in items:
            self.add('<rect x="%.1f" y="%.1f" width="12" height="12" rx="3" class="s%d"/>'
                     % (x, y - 10, slot))
            self.text(x + 17, y, label, "t2")
            x += 30 + 7 * len(label)

    def save(self, path):
        self.parts.append("</svg>")
        with open(path, "w") as f:
            f.write("\n".join(self.parts) + "\n")


class Panel:
    """A plot area with an ordinal or linear x axis and a linear y axis from 0."""

    def __init__(self, svg, x, y, w, h, ymax, ylabel, xticks=None, xlabel="", xmax=None):
        self.svg, self.x, self.y, self.w, self.h = svg, x, y, w, h
        self.end_labels = []
        step = nice_step(ymax)
        n = max(1, int(-(-ymax // step)))
        self.ymax = step * n
        self.xticks, self.xmax = xticks, xmax
        for i in range(n + 1):
            v = step * i
            py = self.py(v)
            svg.add('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" class="%s"/>'
                    % (x, py, x + w, py, "axis" if i == 0 else "grid"))
            svg.text(x - 6, py + 4, fmt(v), "t3", "end")
        svg.text(x, y - 8, ylabel, "t2")
        if xticks:
            for i, t in enumerate(xticks):
                svg.text(self.px(i), y + h + 16, t, "t3", "middle")
        if xlabel:
            svg.text(x + w / 2, y + h + 34, xlabel, "t2", "middle")

    def py(self, v):
        return self.y + self.h - (v / self.ymax) * self.h

    def px(self, i):
        if self.xmax is not None:  # linear x
            return self.x + (i / self.xmax) * self.w
        n = len(self.xticks)
        return self.x + self.w * (i + 0.5) / n

    def line(self, slot, points, label, tip):
        """points: [(xi, value)]; draws a 2 px line with 8 px markers and an end label."""
        if not points:
            return
        d = " ".join("%s%.1f,%.1f" % ("M" if k == 0 else "L", self.px(i), self.py(v))
                     for k, (i, v) in enumerate(points))
        self.svg.add('<path d="%s" class="s%d" fill="none" style="fill:none" stroke-width="2" '
                     'stroke-linejoin="round"/>' % (d, slot))
        for i, v in points:
            self.svg.add('<circle cx="%.1f" cy="%.1f" r="4" class="s%d gap" stroke-width="2" '
                         'style="stroke-width:2"><title>%s</title></circle>'
                         % (self.px(i), self.py(v), slot, esc(tip(i, v))))
        if label:
            i, v = points[-1]
            self.end_labels.append([self.py(v) + 4, self.px(i) + 8, label])

    def place_labels(self, gap=13):
        """Draw line-end labels, nudged apart vertically so none overlap."""
        labs = sorted(self.end_labels)
        for k in range(1, len(labs)):
            if labs[k][0] - labs[k - 1][0] < gap:
                labs[k][0] = labs[k - 1][0] + gap
        for y, x, label in labs:
            self.svg.text(x, y, label, "lab")
        self.end_labels = []

    def bar(self, slot, cx, width, v, tip, label=None):
        """A bar anchored to the baseline with a 4 px rounded data end."""
        top, base = self.py(v), self.py(0)
        h = base - top
        r = min(4, width / 2, h)
        x0, x1 = cx - width / 2, cx + width / 2
        if h <= 0:
            return
        d = ("M%.1f,%.1f L%.1f,%.1f Q%.1f,%.1f %.1f,%.1f L%.1f,%.1f Q%.1f,%.1f %.1f,%.1f "
             "L%.1f,%.1f Z" % (x0, base, x0, top + r, x0, top, x0 + r, top, x1 - r, top,
                              x1, top, x1, top + r, x1, base))
        self.svg.add('<path d="%s" class="s%d"><title>%s</title></path>' % (d, slot, esc(tip)))
        if label is not None:
            self.svg.text(cx, top - 5, label, "lab", "middle")

    def ref(self, v, label):
        py = self.py(v)
        self.svg.add('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" class="ref"/>'
                     % (self.x, py, self.x + self.w, py))
        self.svg.text(self.x + self.w + 8, py + 4, label, "t2")


def read(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def f(row, key):
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return 0.0


# --------------------------------------------------------------------------- charts

def chart_oversell(rows, out):
    modes = [m for m in MODES if any(r["mode"] == m for r in rows)]
    if not modes:
        return None
    runs = {m: [r for r in rows if r["mode"] == m] for m in modes}
    stock = f(rows[0], "stock")
    sold_key = "units_sold" if "units_sold" in rows[0] else "sold"
    vmax = max(max(f(r, sold_key) for r in runs[m]) for m in modes)
    svg = Svg(720, 390, "Oversell test: units sold for %d units of stock" % stock,
              "1,000 buyers check out the same SKU at the same instant; each bar is one run")
    p = Panel(svg, 70, 100, 560, 230, max(vmax, stock) * 1.1, "units sold",
              ["%s" % m for m in modes])
    group = p.w / len(modes)
    for mi, m in enumerate(modes):
        n = len(runs[m])
        bw = min(46, (group * 0.6) / n)
        for k, r in enumerate(runs[m]):
            cx = p.px(mi) + (k - (n - 1) / 2) * (bw + 2)
            v = f(r, sold_key)
            p.bar(SLOT[m], cx, bw, v, "%s, run %d: %d units sold (stock %d)" % (m, k + 1, v, stock),
                  "%d" % v)
    p.ref(stock, "stock: %d" % stock)
    svg.legend(70, 64, [(SLOT[m], m) for m in modes])
    svg.save(os.path.join(out, "oversell.svg"))
    return "oversell.svg"


def mode_lines(rows, key, clients):
    data = {}
    for m in MODES:
        pts = []
        for i, c in enumerate(clients):
            rr = [r for r in rows if r["mode"] == m and int(r["clients"]) == c]
            if rr:
                pts.append((i, statistics.mean(f(r, key) for r in rr)))
        if pts:
            data[m] = pts
    return data


def chart_lines(rows, out, name, key, title, subtitle, ylabel, unit):
    clients = sorted({int(r["clients"]) for r in rows})
    data = mode_lines(rows, key, clients)
    if not data:
        return None
    vmax = max(v for pts in data.values() for _, v in pts)
    svg = Svg(720, 410, title, subtitle)
    p = Panel(svg, 70, 100, 520, 240, vmax * 1.08, ylabel, [str(c) for c in clients],
              "concurrent clients")
    for m, pts in data.items():
        p.line(SLOT[m], pts, m, lambda i, v, m=m: "%s, %d clients: %s %s"
               % (m, clients[i], fmt(v), unit))
    p.place_labels()
    svg.legend(70, 64, [(SLOT[m], m) for m in data])
    svg.save(os.path.join(out, name))
    return name


def chart_latency_panels(rows, out):
    clients = sorted({int(r["clients"]) for r in rows})
    keys = [("checkout_p50_ms", "p50"), ("checkout_p95_ms", "p95"), ("checkout_p99_ms", "p99")]
    datas = [mode_lines(rows, k, clients) for k, _ in keys]
    if not datas[0]:
        return None
    vmax = max(v for d in datas for pts in d.values() for _, v in pts)
    svg = Svg(960, 400, "Checkout latency by percentile",
              "Mixed workload (4 browse + add + checkout per cycle), 1,000 SKUs; same y scale in every panel")
    for k, ((key, label), data) in enumerate(zip(keys, datas)):
        p = Panel(svg, 70 + k * 300, 100, 240, 220, vmax * 1.08,
                  "%s (ms)" % label if k == 0 else label, [str(c) for c in clients],
                  "concurrent clients")
        for m, pts in data.items():
            p.line(SLOT[m], pts, None, lambda i, v, m=m, label=label: "%s %s, %d clients: %.1f ms"
                   % (m, label, clients[i], v))
    svg.legend(70, 64, [(SLOT[m], m) for m in MODES if m in datas[0]])
    svg.save(os.path.join(out, "latency-checkout.svg"))
    return "latency-checkout.svg"


def chart_grouped(rows, out, name, key, title, subtitle, ylabel, unit, groups, group_label):
    modes = [m for m in MODES if any(r["mode"] == m for r in rows)]
    if not modes:
        return None
    vals = {}
    for g in groups:
        for m in modes:
            rr = [r for r in rows if r["mode"] == m and r["skus"] == g]
            if rr:
                vals[(g, m)] = statistics.mean(f(r, key) for r in rr)
    if not vals:
        return None
    svg = Svg(720, 390, title, subtitle)
    p = Panel(svg, 70, 100, 600, 230, max(vals.values()) * 1.12, ylabel,
              [group_label.get(g, g) for g in groups])
    group = p.w / len(groups)
    bw = min(56, group * 0.7 / len(modes))
    for gi, g in enumerate(groups):
        for k, m in enumerate(modes):
            if (g, m) not in vals:
                continue
            v = vals[(g, m)]
            cx = p.px(gi) + (k - (len(modes) - 1) / 2) * (bw + 2)
            p.bar(SLOT[m], cx, bw, v, "%s, %s: %s %s" % (m, group_label.get(g, g), fmt(v), unit),
                  fmt(v))
    svg.legend(70, 64, [(SLOT[m], m) for m in modes])
    svg.save(os.path.join(out, name))
    return name


def chart_recovery(rows, out):
    if not rows:
        return None
    vals = [f(r, "recovery_ms") for r in rows]
    labels = ["%s.%s" % (r["run"], r["kill"]) for r in rows]
    svg = Svg(720, 390, "Crash recovery: kill -9 of-core under load",
              "Time from SIGKILL until a request through Apache is answered again; each bar is one kill")
    p = Panel(svg, 70, 100, 560, 230, max(max(vals), 5000) * 1.08, "milliseconds", labels,
              "run.kill")
    bw = min(40, p.w / len(vals) * 0.6)
    for i, v in enumerate(vals):
        p.bar(3, p.px(i), bw, v, "run %s: back in %d ms" % (labels[i], v), "%d" % v)
    p.ref(5000, "target: 5 s")
    svg.save(os.path.join(out, "recovery.svg"))
    return "recovery.svg"


def chart_backpressure(rows, out):
    if not rows:
        return None
    clients = [int(r["clients"]) for r in rows]
    served = [f(r, "req_per_s") for r in rows]
    dur = [f(r, "duration_s") or 1 for r in rows]
    shed = [f(r, "busy_503") / d for r, d in zip(rows, dur)]
    vmax = max(served + shed)
    svg = Svg(720, 410, "Backpressure: a small core pushed past capacity",
              "2 core workers, 16-slot job queue, 64 API workers; requests per second by outcome")
    p = Panel(svg, 70, 100, 520, 240, vmax * 1.08, "requests per second",
              [str(c) for c in clients], "concurrent clients")
    p.line(3, list(enumerate(served)), "served",
           lambda i, v: "%d clients: %s served per second" % (clients[i], fmt(v)))
    p.line(4, list(enumerate(shed)), "shed (503 busy)",
           lambda i, v: "%d clients: %s refused as busy per second" % (clients[i], fmt(v)))
    p.place_labels()
    svg.legend(70, 64, [(3, "served"), (4, "shed with 503 busy")])
    svg.save(os.path.join(out, "backpressure.svg"))
    return "backpressure.svg"


def chart_backpressure_rss(samples, out):
    if not samples:
        return None
    t = [f(r, "t_ms") / 1000 for r in samples]
    rss = [f(r, "core_rss_kb") / 1024 for r in samples]
    tmax = max(t) or 1
    svg = Svg(720, 390, "Backpressure: of-core memory while overloaded",
              "Resident set size of of-core, sampled every 0.5 s through the whole backpressure run")
    p = Panel(svg, 70, 100, 600, 230, max(rss) * 1.25, "RSS (MiB)", None, "", xmax=tmax)
    for i in range(6):
        v = tmax * i / 5
        svg.text(p.px(v), p.y + p.h + 16, "%d" % v, "t3", "middle")
    svg.text(p.x + p.w / 2, p.y + p.h + 34, "seconds into the run", "t2", "middle")
    pts = list(zip(t, rss))
    d = " ".join("%s%.1f,%.1f" % ("M" if k == 0 else "L", p.px(x), p.py(v))
                 for k, (x, v) in enumerate(pts))
    svg.add('<path d="%s" class="s1" style="fill:none" stroke-width="2"/>' % d)
    svg.add('<title>of-core RSS from %.1f to %.1f MiB</title>' % (min(rss), max(rss)))
    svg.text(p.px(t[-1]) - 4, p.py(rss[-1]) - 8, "%.1f MiB" % rss[-1], "lab", "end")
    svg.save(os.path.join(out, "backpressure-memory.svg"))
    return "backpressure-memory.svg"


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    run = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(run, "charts")
    os.makedirs(out, exist_ok=True)
    made = []
    thr = read(os.path.join(run, "throughput.csv"))
    con = read(os.path.join(run, "contention.csv"))
    made.append(chart_oversell(read(os.path.join(run, "oversell.csv")), out))
    if thr:
        made.append(chart_lines(thr, out, "throughput-orders.svg", "orders_per_s",
                                "Throughput: orders placed per second",
                                "Mixed workload (4 browse + add + checkout per cycle), 1,000 SKUs",
                                "orders per second", "orders/s"))
        made.append(chart_lines(thr, out, "throughput-requests.svg", "req_per_s",
                                "Throughput: successful requests per second",
                                "Same runs, counting every browse, add and checkout",
                                "requests per second", "requests/s"))
        made.append(chart_latency_panels(thr, out))
    if con:
        groups = {"spread": "spread over 1,000 SKUs", "hot": "one hot SKU"}
        made.append(chart_grouped(con, out, "contention-orders.svg", "orders_per_s",
                                  "Contention: orders per second, hot SKU vs spread",
                                  "Checkout-only workload (add + checkout per cycle)",
                                  "orders per second", "orders/s", ["spread", "hot"], groups))
        made.append(chart_grouped(con, out, "contention-p99.svg", "checkout_p99_ms",
                                  "Contention: checkout p99 latency, hot SKU vs spread",
                                  "Same runs; lower is better", "p99 (ms)", "ms",
                                  ["spread", "hot"], groups))
    abl = read(os.path.join(run, "ablation.csv"))
    if abl:
        rows = []
        for r in abl:
            if r["skus"] != "hot":
                continue
            if r["variant"].startswith("lock taken mid"):
                rows.append(dict(r, skus="before"))
            elif r["variant"] == "lock late (after)":
                rows.append(dict(r, skus="after"))
        made.append(chart_grouped(rows, out, "lock-late.svg", "orders_per_s",
                                  "Lock late: one hot SKU, before and after",
                                  "200 clients, checkout only; the inventory row lock moved to just before COMMIT",
                                  "orders per second", "orders/s", ["before", "after"],
                                  {"before": "row lock mid-transaction",
                                   "after": "row lock just before COMMIT"}))
    made.append(chart_recovery(read(os.path.join(run, "recovery.csv")), out))
    made.append(chart_backpressure(read(os.path.join(run, "backpressure.csv")), out))
    made.append(chart_backpressure_rss(read(os.path.join(run, "backpressure-samples.csv")), out))
    for m in made:
        if m:
            print("chart:", os.path.join(out, m))


if __name__ == "__main__":
    main()
