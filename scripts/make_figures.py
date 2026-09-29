#!/usr/bin/env python3
"""Generate the README figures from the simulator's real output.

Inputs (nothing is typed in by hand):
  * stdout of ./fpga-sim-demo (latency statistics, histogram, counts)
  * the VCD file the demo writes (clock, tick_valid, ofi)
  * include/pipeline/tick_to_trade.hpp and include/modules/*.hpp (stage
    latency parameters; the script aborts if it cannot find them)

Output: deterministic plain-SVG files (no matplotlib, no timestamps, no random)
in docs/img/, dark and light variants, plus docs/img/social-preview.png when a
Chromium binary is available (PNG rasterisation is the only non-deterministic
step across machines; the SVGs are byte-identical for identical inputs).

Usage:
  scripts/make_figures.py --demo /tmp/fpga-build/fpga-sim-demo [--out docs/img]
                          [--chromium /path/to/chrome]

Every figure is labelled: simulated cycles, synthetic input, not a hardware
measurement.
"""
import argparse
import html
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISCLAIMER = "simulated cycles, synthetic input, not a hardware measurement"
FONT = "ui-monospace, SFMono-Regular, Menlo, Consolas, 'DejaVu Sans Mono', monospace"

THEMES = {
    "dark": dict(bg="#0d1117", panel="#161b22", border="#30363d", fg="#e6edf3", muted="#8b949e",
                 grid="#21262d", green="#3fb950", blue="#58a6ff", amber="#e3b341", red="#ff7b72"),
    "light": dict(bg="#ffffff", panel="#f6f8fa", border="#d0d7de", fg="#1f2328", muted="#57606a",
                  grid="#e6eaef", green="#1a7f37", blue="#0969da", amber="#9a6700", red="#cf222e"),
}


def esc(s):
    return html.escape(str(s), quote=True)


# --------------------------------------------------------------------------- inputs
def read_source_params():
    def grab(path, pattern):
        with open(os.path.join(ROOT, path), encoding="utf-8") as f:
            m = re.search(pattern, f.read())
        if not m:
            sys.exit("cannot find %r in %s" % (pattern, path))
        return int(m.group(1))
    t = "include/pipeline/tick_to_trade.hpp"
    return dict(
        parser=grab(t, r"parser_cycles\s*=\s*(\d+)"),
        book=grab(t, r"book_cycles\s*=\s*(\d+)"),
        dma=grab(t, r"dma_post_cycles\s*=\s*(\d+)"),
        bram=grab(t, r"bram_latency\s*=\s*(\d+)"),
        ipg=grab(t, r"mac_ipg_idle_bytes\s*=\s*(\d+)"),
        ofi=grab("include/modules/ofi_dsp.hpp", r"pipeline_stages\s*=\s*(\d+)"),
    )


def run_demo(demo):
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run([os.path.abspath(demo)], cwd=d, capture_output=True, text=True, check=True)
        with open(os.path.join(d, "fpga_sim_core.vcd"), encoding="ascii") as f:
            vcd = f.read()
    return r.stdout, vcd


def parse_demo(out):
    def one(pat, cast=int):
        m = re.search(pat, out)
        if not m:
            sys.exit("demo output lacks %r" % pat)
        return cast(m.group(1)) if m.lastindex == 1 else tuple(cast(g) for g in m.groups())
    d = {}
    d["mhz"] = re.search(r"clock = ([\d.]+) MHz = (\d+/\d+) ns/cycle", out).groups()
    d["messages"] = one(r"ITCH 5\.0 stream .*?: (\d+) messages")
    d["mix"] = re.search(r"\(A=(\d+) E=(\d+) X=(\d+)\)", out).groups()
    d["seed"] = re.search(r"PRNG (0x[0-9A-Fa-f]+)", out).group(1)
    d["cycles"] = one(r"simulated (\d+) cycles")
    for k in ("min", "median", "p99", "max"):
        c, ns = re.search(r"^\s+%s\s+(\d+) cycles = ([\d.]+) ns" % k, out, re.M).groups()
        d[k] = (int(c), ns)
    hist = {}
    for m in re.finditer(r"^\s*(\d+)-\s*(\d+) cycles\s+(\d+) #", out, re.M):
        hist[int(m.group(1))] = (int(m.group(2)), int(m.group(3)))
    lo, hi = min(hist), max(hist)
    d["hist"] = [(b, b + 1, hist.get(b, (b + 1, 0))[1]) for b in range(lo, hi + 1, 2)]
    d["ofi_cum"] = one(r"cumulative = (-?\d+)")
    d["ofi_nonzero"] = one(r"nonzero on (\d+) of")
    d["dropped"] = one(r"adds dropped \(side full\): (\d+)")
    assert sum(h[2] for h in d["hist"]) == d["messages"], "histogram does not sum to message count"
    return d


def parse_vcd(text):
    """Return (tick[], ofi[], t_fs[]) sampled at each rising clock edge."""
    m = re.search(r"\$timescale\s+1 fs", text)
    if not m:
        sys.exit("unexpected VCD timescale")
    ids = {n: i for i, n in re.findall(r"\$var wire \d+ (\S+) (\w+) \$end", text)}
    clk, tick, ofi = ids["clk"], ids["tick_valid"], ids["ofi"]
    body = text.split("$enddefinitions $end", 1)[1]
    blocks, cur, t = [], {}, 0
    for line in body.splitlines():
        line = line.strip()
        if not line or line.startswith("$"):
            continue
        if line[0] == "#":
            blocks.append((t, cur)); cur, t = {}, int(line[1:])
        elif line[0] == "b":
            v, i = line[1:].split()
            cur[i] = int(v, 2)
        else:
            cur[line[1:]] = int(line[0])
    blocks.append((t, cur))
    state, samples = {}, []
    for t, upd in blocks:
        rising = upd.get(clk) == 1
        state.update(upd)
        if rising:
            o = state.get(ofi, 0)
            samples.append((t, state.get(tick, 0), o - (1 << 32) if o >= 1 << 31 else o))
    return [s[1] for s in samples], [s[2] for s in samples], [s[0] for s in samples]


# --------------------------------------------------------------------------- svg helpers
class Svg:
    def __init__(self, w, h, th, title, desc):
        self.w, self.h, self.th, self.parts = w, h, th, []
        self.head = ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
                     'role="img" aria-labelledby="t d" font-family="%s">\n<title id="t">%s</title>\n'
                     '<desc id="d">%s</desc>\n' % (w, h, w, h, FONT, esc(title), esc(desc)))
        self.rect(0, 0, w, h, fill=th["bg"])

    def rect(self, x, y, w, h, fill="none", stroke=None, rx=0, sw=1, dash=None, opacity=None):
        a = 'x="%g" y="%g" width="%g" height="%g" fill="%s"' % (x, y, w, h, fill)
        if rx: a += ' rx="%g"' % rx
        if stroke: a += ' stroke="%s" stroke-width="%g"' % (stroke, sw)
        if dash: a += ' stroke-dasharray="%s"' % dash
        if opacity is not None: a += ' opacity="%g"' % opacity
        self.parts.append("<rect %s/>" % a)

    def line(self, x1, y1, x2, y2, stroke, sw=1, dash=None):
        a = ' stroke-dasharray="%s"' % dash if dash else ""
        self.parts.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="%g"%s/>' % (x1, y1, x2, y2, stroke, sw, a))

    def path(self, d, stroke, sw=2, fill="none", join="miter"):
        self.parts.append('<path d="%s" fill="%s" stroke="%s" stroke-width="%g" stroke-linejoin="%s"/>' % (d, fill, stroke, sw, join))

    def text(self, x, y, s, size=14, fill=None, anchor="start", weight="normal", opacity=None):
        a = ' opacity="%g"' % opacity if opacity is not None else ""
        self.parts.append('<text x="%g" y="%g" font-size="%g" fill="%s" text-anchor="%s" font-weight="%s"%s>%s</text>'
                          % (x, y, size, fill or self.th["fg"], anchor, weight, a, esc(s)))

    def circle(self, x, y, r, fill):
        self.parts.append('<circle cx="%g" cy="%g" r="%g" fill="%s"/>' % (x, y, r, fill))

    def raw(self, s):
        self.parts.append(s)

    def render(self):
        return self.head + "\n".join(self.parts) + "\n</svg>\n"


def digital_path(vals, x0, cw, y_hi, y_lo):
    d, prev = "", None
    for i, v in enumerate(vals):
        y = y_hi if v else y_lo
        x = x0 + i * cw
        d += ("M%.2f %.2f" % (x, y)) if prev is None else ("L%.2f %.2f L%.2f %.2f" % (x, prev, x, y))
        d += "L%.2f %.2f" % (x + cw, y)
        prev = y
    return d


def clock_path(n, x0, cw, y_hi, y_lo):
    d = "M%.2f %.2f" % (x0, y_lo)
    for i in range(n):
        x = x0 + i * cw
        d += "L%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f" % (x, y_hi, x + cw / 2, y_hi, x + cw / 2, y_lo, x + cw, y_lo)
    return d


def pick_window(ofi, n=40):
    changes = [i for i in range(1, len(ofi)) if ofi[i] != ofi[i - 1] and ofi[i] != 0]
    start = max(0, changes[0] - 12) if changes else 0
    return start, min(start + n, len(ofi))


def terminal_chrome(s, x, y, w, h, title):
    th = s.th
    s.rect(x, y, w, h, fill=th["panel"], stroke=th["border"], rx=10)
    for i, c in enumerate((th["red"], th["amber"], th["green"])):
        s.circle(x + 22 + i * 20, y + 20, 6, c)
    s.text(x + w / 2, y + 25, title, 13, th["muted"], "middle")
    s.line(x, y + 40, x + w, y + 40, th["border"])


def wave_strip(s, x, y, w, h, tick, ofi, start, n):
    """Compact clk/tick_valid/ofi strip from the real VCD, n cycles starting at `start`."""
    th, cw = s.th, w / n
    rows = h / 3
    t, o = tick[start:start + n], ofi[start:start + n]
    for i in range(0, n + 1, 5):
        s.line(x + i * cw, y, x + i * cw, y + h, th["grid"])
    s.path(clock_path(n, x, cw, y + 6, y + rows - 6), th["muted"], 1.5)
    s.path(digital_path(t, x, cw, y + rows + 6, y + 2 * rows - 6), th["green"], 2)
    lo, hi = min(ofi), max(ofi)
    yo = lambda v: y + 3 * rows - 6 - (v - lo) / (hi - lo) * (rows - 12)
    d = ""
    for i, v in enumerate(o):
        d += ("M%.2f %.2f" % (x, yo(v)) if i == 0 else "L%.2f %.2f" % (x + i * cw, yo(o[i - 1])) + "L%.2f %.2f" % (x + i * cw, yo(v)))
    d += "L%.2f %.2f" % (x + n * cw, yo(o[-1]))
    s.path(d, th["blue"], 2)


def fig_hero(th, d, wave, p):
    tick, ofi, _ = wave
    s = Svg(1280, 360, th, "fpga-sim-core", "Deterministic C++20 cycle model of a tick-to-trade datapath; latency in simulated cycles.")
    terminal_chrome(s, 24, 24, 1232, 312, "fpga-sim-core  --  ./fpga-sim-demo")
    s.text(56, 100, "$", 30, th["green"], weight="bold")
    s.text(90, 100, "fpga-sim-core", 46, th["fg"], weight="bold")
    s.text(56, 138, "cycle model of a tick-to-trade datapath", 20, th["muted"])
    s.text(56, 164, "wire > PCS > MAC > ITCH > book > OFI > DMA", 15, th["muted"])
    y = 196
    for k, col in (("min", th["fg"]), ("median", th["fg"]), ("p99", th["amber"]), ("max", th["fg"])):
        s.text(56, y, k, 18, th["muted"])
        s.text(150, y, "%2d cycles" % d[k][0], 18, col, weight="bold")
        s.text(270, y, "= %s ns" % d[k][1], 18, th["muted"])
        y += 26
    s.text(56, 298, "C++20, header-only, %s MHz clock" % d["mhz"][0], 14, th["muted"])
    s.text(56, 318, DISCLAIMER + " | %d messages, fixed seed %s" % (d["messages"], d["seed"]), 13, th["muted"])
    start, end = pick_window(ofi, 40)
    x0, w = 640, 580
    s.text(x0 + 88, 96, "clk", 13, th["muted"], "end"); s.text(x0 + 88, 150, "tick_valid", 13, th["green"], "end"); s.text(x0 + 88, 204, "ofi", 13, th["blue"], "end")
    wave_strip(s, x0 + 100, 72, w - 100, 168, tick, ofi, start, end - start)
    s.text(x0 + 100, 262, "real VCD trace, cycles %d-%d of %d" % (start, end - 1, len(tick)), 13, th["muted"])
    return s


def fig_hist(th, d):
    W, H = 1280, 640
    s = Svg(W, H, th, "Tick-to-trade latency histogram",
            "Histogram of per-message latency in simulated cycles for %d synthetic ITCH messages: min %d, median %d, p99 %d, max %d."
            % (d["messages"], d["min"][0], d["median"][0], d["p99"][0], d["max"][0]))
    s.text(64, 64, "Tick-to-trade latency, %d synthetic ITCH messages" % d["messages"], 26, weight="bold")
    s.text(64, 94, "%s" % DISCLAIMER, 16, th["amber"])
    x0, x1, y0, y1 = 120, 1216, 150, 500
    bins = d["hist"]
    peak = max(b[2] for b in bins)
    top = ((peak + 49) // 50) * 50
    for v in range(0, top + 1, 50):
        yy = y1 - v / top * (y1 - y0)
        s.line(x0, yy, x1, yy, th["grid"])
        s.text(x0 - 12, yy + 5, str(v), 14, th["muted"], "end")
    s.text(x0, y0 - 18, "messages per 2-cycle bin", 14, th["muted"])
    slot = (x1 - x0) / len(bins)
    bw = min(slot * 0.66, 150)
    for i, (a, b, c) in enumerate(bins):
        cx = x0 + slot * (i + 0.5)
        h = c / top * (y1 - y0)
        s.rect(cx - bw / 2, y1 - h, bw, h, fill=th["blue"], rx=3)
        s.text(cx, y1 - h - 10, str(c), 18, th["fg"], "middle", "bold")
        s.text(cx, y1 + 30, "%d-%d" % (a, b), 16, th["fg"], "middle")
    s.text((x0 + x1) / 2, y1 + 64, "latency (simulated cycles, 1 cycle = %s ns)" % d["mhz"][1], 15, th["muted"], "middle")
    # markers for the percentile statistics, placed in their bins
    def bin_x(cyc):
        for i, (a, b, _) in enumerate(bins):
            if a <= cyc <= b:
                return x0 + slot * (i + 0.5) + (cyc - a - 0.5) * (bw / 2.4)
    for k, col, row in (("median", th["green"], 0), ("p99", th["amber"], 1)):
        bx = bin_x(d[k][0])
        s.line(bx, y1 - 6, bx, y0 + 30 + row * 28, col, 2, "5 4")
        s.text(bx + 8, y0 + 44 + row * 28, "%s = %d cycles" % (k, d[k][0]), 15, col, weight="bold")
    s.text(64, 590, "min %d  |  median %d  |  p99 %d  |  max %d cycles   (%s / %s / %s / %s ns)"
           % (d["min"][0], d["median"][0], d["p99"][0], d["max"][0], d["min"][1], d["median"][1], d["p99"][1], d["max"][1]), 16)
    s.text(64, 616, "source: ./fpga-sim-demo, PRNG seed %s, %d cycles simulated" % (d["seed"], d["cycles"]), 13, th["muted"])
    return s


def fig_card(th, d):
    s = Svg(1280, 420, th, "Simulation summary card", "Summary of the demo run in simulated cycles: min %d, median %d, p99 %d, max %d." %
            (d["min"][0], d["median"][0], d["p99"][0], d["max"][0]))
    s.text(48, 60, "fpga-sim-core  demo run", 26, weight="bold")
    s.text(48, 88, DISCLAIMER, 16, th["amber"])
    tw, gap, x, y = 280, 24, 48, 118
    for k, label, col in (("min", "MIN", th["fg"]), ("median", "MEDIAN", th["green"]), ("p99", "P99", th["amber"]), ("max", "MAX", th["fg"])):
        s.rect(x, y, tw, 150, fill=th["panel"], stroke=th["border"], rx=10)
        s.text(x + 22, y + 36, label, 15, th["muted"], weight="bold")
        s.text(x + 22, y + 96, str(d[k][0]), 60, col, weight="bold")
        s.text(x + 22 + 38 * len(str(d[k][0])) + 12, y + 96, "cycles", 18, th["muted"])
        s.text(x + 22, y + 128, "= %s ns" % d[k][1], 16, th["muted"])
        x += tw + gap
    s.text(48, 316, "%d messages (A=%s E=%s X=%s), %d cycles simulated, %d decision records posted" %
           (d["messages"], d["mix"][0], d["mix"][1], d["mix"][2], d["cycles"], d["messages"]), 16)
    s.text(48, 344, "clock %s MHz = %s ns/cycle  |  cumulative OFI %d, nonzero on %d of %d messages  |  book adds dropped: %d" %
           (d["mhz"][0], d["mhz"][1], d["ofi_cum"], d["ofi_nonzero"], d["messages"], d["dropped"]), 16)
    s.text(48, 388, "Latency = cycles from first PCS block arrival to DMA decision-record post; stage latencies are documented model parameters.", 13, th["muted"])
    return s


def fig_wave(th, d, wave):
    tick, ofi, tfs = wave
    start, end = pick_window(ofi, 40)
    n = end - start
    W, H = 1280, 640
    s = Svg(W, H, th, "Waveform of the demo VCD trace",
            "Overview of the full %d-cycle VCD trace and a %d-cycle zoom of clk, tick_valid and ofi." % (len(tick), n))
    s.text(48, 56, "Waveform of the trace the demo writes (fpga_sim_core.vcd)", 24, weight="bold")
    s.text(48, 82, "parsed from the real VCD; %s" % DISCLAIMER, 15, th["amber"])
    # overview
    ox, ow, oy = 168, 1064, 120
    N = len(tick)
    s.text(48, oy - 10, "overview: all %d cycles" % N, 14, th["muted"])
    s.rect(ox, oy, ow, 44, fill=th["panel"], stroke=th["border"])
    s.text(ox - 10, oy + 16, "tick_valid", 12, th["green"], "end")
    i = 0
    while i < N:
        if tick[i]:
            j = i
            while j < N and tick[j]: j += 1
            s.rect(ox + i / N * ow, oy + 4, max((j - i) / N * ow, 0.4), 14, fill=th["green"], opacity=0.85)
            i = j
        else:
            i += 1
    lo, hi = min(ofi), max(ofi)
    yo = lambda v: oy + 40 - (v - lo) / (hi - lo) * 18
    step = 4
    dd = "".join("%s%.2f %.2f" % ("M" if k == 0 else "L", ox + (k * step) / N * ow, yo(ofi[k * step])) for k in range((N - 1) // step + 1))
    s.path(dd, th["blue"], 1)
    s.text(ox - 10, oy + 38, "ofi", 12, th["blue"], "end")
    wx, ww = ox + start / N * ow, max(n / N * ow, 3)
    s.rect(wx, oy - 4, ww, 52, stroke=th["amber"], sw=2)
    # zoom
    zx, zw = 168, 1064
    cw = zw / n
    zy = 232
    s.text(zx + zw, zy - 16, "zoom: cycles %d-%d  (%.1f-%.1f ns, from VCD timestamps)" %
           (start, end - 1, tfs[start] / 1e6, tfs[end - 1] / 1e6), 14, th["amber"], "end")
    s.line(wx + ww / 2, oy + 48, zx, zy - 4, th["amber"], 1, "4 4")
    rows = {"clk": zy + 36, "tick_valid": zy + 116, "ofi": zy + 214}
    for name, yy, col in (("clk", rows["clk"], th["muted"]), ("tick_valid", rows["tick_valid"], th["green"]), ("ofi[31:0]", rows["ofi"], th["blue"])):
        s.text(zx - 14, yy + 5, name, 15, col, "end", "bold")
    for k in range(n + 1):
        gx = zx + k * cw
        s.line(gx, zy, gx, zy + 300, th["grid"] if (start + k) % 5 else th["border"])
        if (start + k) % 5 == 0 and k < n + 1:
            s.text(gx, zy + 322, str(start + k), 13, th["muted"], "middle")
    s.text(zx + zw / 2, zy + 350, "simulated cycle (clock edge index)", 14, th["muted"], "middle")
    s.path(clock_path(n, zx, cw, rows["clk"] - 18, rows["clk"] + 18), th["muted"], 2)
    s.path(digital_path(tick[start:end], zx, cw, rows["tick_valid"] - 18, rows["tick_valid"] + 18), th["green"], 2.5)
    # ofi bus
    bus_y, bh = rows["ofi"], 20
    k = 0
    seg = []
    o = ofi[start:end]
    while k < n:
        j = k
        while j < n and o[j] == o[k]: j += 1
        seg.append((k, j, o[k])); k = j
    for a, b, v in seg:
        xa, xb = zx + a * cw + 3, zx + b * cw - 3
        if xb - xa < 6: xb = xa + 6
        s.path("M%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f L%.2f %.2f Z" %
               (xa, bus_y, xa + 5, bus_y - bh, xb - 5, bus_y - bh, xb, bus_y, xb - 5, bus_y + bh, xa + 5, bus_y + bh),
               th["blue"], 2, th["panel"])
        if xb - xa > 34:
            s.text((xa + xb) / 2, bus_y + 5, "%+d" % v if v else "0", 14, th["fg"], "middle", "bold")
    s.text(48, 604, "tick_valid is the demo's wire_busy(): high while the PCS stage holds a message. ofi is the last OFI value (signed 32-bit, shares).", 13, th["muted"])
    s.text(48, 626, "1 cycle = 3103030 fs in the VCD (rounded); clk is drawn with the 50% duty the logger writes.", 13, th["muted"])
    return s


def fig_pipeline(th, p):
    W, H = 1280, 490
    s = Svg(W, H, th, "Tick-to-trade pipeline stages",
            "Pipeline: PCS block decode, MAC framer, ITCH parser %d cycles, order book %d cycle, quote RAM %d, OFI %d stages, DMA post %d cycles." %
            (p["parser"], p["book"], p["bram"], p["ofi"], p["dma"]))
    s.text(48, 54, "Tick-to-trade pipeline (include/pipeline/tick_to_trade.hpp)", 24, weight="bold")
    s.text(48, 80, "latency is counted in simulated cycles; every stage hand-off is a one-cycle register", 15, th["muted"])
    stages = [
        ("PCS", "Deserializer66b", "1 block/cycle", "4-6 blocks/msg", False),
        ("MAC", "MacFramer", "1 beat/cycle", "1 beat/msg", False),
        ("PARSE", "Itch50Parser", "%d cycles" % p["parser"], "fixed parameter", True),
        ("BOOK", "FiveLevelOrderBook", "%d cycle" % p["book"], "fixed parameter", True),
        ("QUOTE RAM", "DualPortBram", "latency %d" % p["bram"], "write + read-back", False),
        ("OFI", "OfiDsp", "%d stages" % p["ofi"], "as executed", False),
        ("DMA", "PcieGen4x16Dma", "%d cycles" % p["dma"], "fixed parameter", True),
    ]
    n = len(stages)
    bw, gap, x0, y0, bh = 144, 28, 48, 130, 190
    cols = []
    for i, (name, cls, lat, note, fixed) in enumerate(stages):
        x = x0 + i * (bw + gap)
        col = th["amber"] if fixed else th["green"]
        s.rect(x, y0, bw, bh, fill=th["panel"], stroke=col, rx=8, sw=2, dash="7 5" if fixed else None)
        s.text(x + bw / 2, y0 + 34, name, 17, th["fg"], "middle", "bold")
        s.text(x + bw / 2, y0 + 58, cls, 11, th["muted"], "middle")
        s.line(x + 14, y0 + 74, x + bw - 14, y0 + 74, th["border"])
        s.text(x + bw / 2, y0 + 112, lat, 16, col, "middle", "bold")
        s.text(x + bw / 2, y0 + 140, note, 12, th["fg"], "middle")
        if i < n - 1:
            ax = x + bw + 4
            s.line(ax, y0 + bh / 2, ax + gap - 10, y0 + bh / 2, th["muted"], 2)
            s.path("M%g %g l-7 -5 v10 z" % (ax + gap - 6, y0 + bh / 2), th["muted"], 1, th["muted"])
    s.text(x0, y0 - 16, "wire", 14, th["muted"])
    s.text(x0 + n * (bw + gap) - gap, y0 - 16, "decision record posted to host ring", 14, th["muted"], "end")
    ly = 372
    s.rect(x0, ly, 22, 14, stroke=th["green"], sw=2, rx=3); s.text(x0 + 32, ly + 12, "derived from data or from the module as it executes", 14)
    s.rect(x0 + 520, ly, 22, 14, stroke=th["amber"], sw=2, rx=3, dash="5 3"); s.text(x0 + 552, ly + 12, "fixed documented model parameter (module has no timing of its own)", 14)
    s.text(x0, 414, "Latency = inclusive cycles from a message's first PCS block arriving to its decision record being posted (queueing behind earlier messages included).", 13, th["muted"])
    s.text(x0, 438, "Parameter values are read from the source by scripts/make_figures.py. \"Trade\" means the record post; no order is sent.", 13, th["muted"])
    s.text(x0, 462, DISCLAIMER, 13, th["amber"])
    return s


def fig_social(d, wave):
    th = THEMES["dark"]
    tick, ofi, _ = wave
    s = Svg(1280, 640, th, "fpga-sim-core", "Deterministic C++20 tick-to-trade cycle model. Latency in simulated cycles.")
    for x in range(0, 1281, 40):
        s.line(x, 0, x, 640, th["grid"], 1)
    for y in range(0, 641, 40):
        s.line(0, y, 1280, y, th["grid"], 1)
    s.rect(56, 56, 1168, 528, fill=th["bg"], stroke=th["border"], rx=14, sw=2)
    s.text(96, 150, "fpga-sim-core", 84, th["fg"], weight="bold")
    s.text(96, 198, "deterministic C++20 cycle model of a tick-to-trade datapath", 27, th["muted"])
    s.text(96, 232, "wire > PCS > MAC > ITCH > book > OFI > DMA", 22, th["green"])
    x = 96
    for k, label, col in (("min", "min", th["fg"]), ("median", "median", th["green"]), ("p99", "p99", th["amber"]), ("max", "max", th["fg"])):
        s.rect(x, 268, 250, 130, fill=th["panel"], stroke=th["border"], rx=10)
        s.text(x + 20, 302, label, 17, th["muted"], weight="bold")
        s.text(x + 20, 368, "%d" % d[k][0], 62, col, weight="bold")
        s.text(x + 20 + 40 * len(str(d[k][0])) + 12, 368, "cycles", 20, th["muted"])
        x += 270
    start, end = pick_window(ofi, 40)
    wave_strip(s, 96, 424, 1088, 96, tick, ofi, start, end - start)
    s.text(96, 556, DISCLAIMER + " | %d messages | real VCD trace above" % d["messages"], 16, th["amber"])
    return s


# --------------------------------------------------------------------------- main
def crop_png(path, w, h):
    """Keep the top-left w x h pixels (headless Chromium adds ~88px of window chrome to the viewport)."""
    import struct, zlib
    data = open(path, "rb").read()
    pos, idat, hdr = 8, b"", None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR": hdr = struct.unpack(">IIBBBBB", chunk)
        elif typ == b"IDAT": idat += chunk
        pos += 12 + n
    W, H, depth, ctype, _, _, il = hdr
    assert depth == 8 and ctype in (2, 6) and il == 0
    bpp = 3 if ctype == 2 else 4
    raw, stride, rows, prev = zlib.decompress(idat), W * bpp, [], bytearray(W * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(b"\x00" + bytes(line[:w * bpp])); prev = line
    def chunk(t, body): return struct.pack(">I", len(body)) + t + body + struct.pack(">I", zlib.crc32(t + body) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def find_chromium(arg):
    for c in (arg, os.environ.get("CHROMIUM"), shutil.which("chromium"), shutil.which("google-chrome"),
              "/opt/pw-browsers/chromium-1194/chrome-linux/chrome"):
        if c and os.path.exists(c):
            return c
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--demo", required=True, help="path to the built fpga-sim-demo executable")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "img"))
    ap.add_argument("--chromium", help="Chromium binary used only to rasterise social-preview.png")
    a = ap.parse_args()
    out, vcd = run_demo(a.demo)
    d, p, wave = parse_demo(out), read_source_params(), parse_vcd(vcd)
    os.makedirs(a.out, exist_ok=True)

    def write(name, svg):
        with open(os.path.join(a.out, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(svg.render())

    for tn, th in THEMES.items():
        write("hero-%s.svg" % tn, fig_hero(th, d, wave, p))
        write("latency-histogram-%s.svg" % tn, fig_hist(th, d))
        write("summary-card-%s.svg" % tn, fig_card(th, d))
        write("waveform-%s.svg" % tn, fig_wave(th, d, wave))
        write("pipeline-%s.svg" % tn, fig_pipeline(th, p))
    write("social-preview.svg", fig_social(d, wave))
    chrome = find_chromium(a.chromium)
    if chrome:
        subprocess.run([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                        "--window-size=1280,760", "--screenshot=" + os.path.join(a.out, "social-preview.png"),
                        "file://" + os.path.join(os.path.abspath(a.out), "social-preview.svg")],
                       check=True, capture_output=True)
        crop_png(os.path.join(a.out, "social-preview.png"), 1280, 640)
    else:
        print("no Chromium found: social-preview.png not regenerated", file=sys.stderr)


if __name__ == "__main__":
    main()
