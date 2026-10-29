#!/usr/bin/env python3
"""Generate docs/img/datapath-{dark,light}.svg: an animated datapath diagram.

A packet token travels through the modelled stages (PCS, MAC, ITCH parse, book,
OFI, DMA). Stage cycle counts are read from the headers by make_figures.py's
reader, so the diagram cannot drift from the code. Solid boxes are derived or
executed by the module; dashed boxes are fixed documented model parameters.

Safari-safe by construction (shown via <img> on GitHub, so no script):
  * CSS keyframes only; whole groups translate or fade; no SMIL, no per-character
    positioning, no textLength. All text is static font text.
  * The base style is the finished state (token parked after the DMA stage). The
    animation starts and ends there, and prefers-reduced-motion turns it off.
Illustrative: the token is a drawing of "one message flows through"; it carries no
data. Latency is in simulated cycles on a synthetic input, not a hardware measurement.

Usage: python3 scripts/make_datapath.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_figures as mf  # noqa: E402  (reuse the header reader and themes)

OUT = os.path.join(HERE, "..", "docs", "img")
MONO = "ui-monospace,'SFMono-Regular','DejaVu Sans Mono',Menlo,Consolas,monospace"
SANS = "system-ui,-apple-system,'Segoe UI',Helvetica,Arial,sans-serif"
DUR = 10  # seconds per loop


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def t(x, y, s, size, fill, anchor="start", weight="normal", family=MONO):
    return (f'<text x="{x}" y="{y}" font-family="{family}" font-size="{size}" font-weight="{weight}" '
            f'fill="{fill}" text-anchor="{anchor}">{esc(s)}</text>')


def build(th, p):
    W, H = 1280, 440
    stages = [  # name, sub, cycles label, derived?, what the packet is here
        ("PCS", "64b/66b-style blocks", "1 block / cycle", True, "7-byte payload blocks + CRC32"),
        ("MAC", "AXI4-Stream, 512-bit", "1 beat / cycle", True, "64-byte beats, IPG checked"),
        ("ITCH 5.0 parse", "zero-copy, big-endian", "%d cycles" % p["parser"], False, "A / E / X message fields"),
        ("Order book", "five-level, quote RAM", "%d cycle + RAM %d" % (p["book"], p["bram"]), False, "best bid / ask (price, qty)"),
        ("OFI", "order-flow imbalance", "%d stages" % p["ofi"], True, "signed 32-bit e_n"),
        ("PCIe DMA", "Gen4 x16 TLP, MSI-X", "%d cycles" % p["dma"], False, "decision record in host ring"),
    ]
    n = len(stages)
    bx, bw, gap, by, bh = 76, 168, 24, 150, 150
    track0, track1 = bx + bw / 2 - 14, bx + (n - 1) * (bw + gap) + bw / 2 - 14  # token left edge, start and end
    dist = track1 - track0
    t0, t1 = 16.0, 84.0  # percent of loop spent travelling
    css = [".tk{transform:translateX(%.1fpx);opacity:1}.gl{opacity:0}" % dist]
    css.append("@keyframes tk{0%%,8%%{transform:translateX(%.1fpx);opacity:1}11%%{transform:translateX(%.1fpx);opacity:0}"
               "13%%{transform:translateX(0);opacity:0}%.1f%%{transform:translateX(0);opacity:1}%.1f%%{transform:translateX(%.1fpx);opacity:1}"
               "100%%{transform:translateX(%.1fpx);opacity:1}}" % (dist, dist, t0, t1, dist, dist))
    css.append(".an .tk{animation:tk %ds linear infinite}" % DUR)
    for i in range(n):
        c = t0 + (t1 - t0) * (i / (n - 1))
        a, b, e = max(c - 5, 0), c + 5, c + 9
        css.append("@keyframes g%d{0%%,%.1f%%{opacity:0}%.1f%%{opacity:.35}%.1f%%{opacity:.35}%.1f%%{opacity:0}100%%{opacity:0}}"
                   % (i, a, c, b, e))
        css.append(".an .g%d{animation:g%d %ds linear infinite}" % (i, i, DUR))
    css.append("@media (prefers-reduced-motion:reduce){.an .tk,.an .gl{animation:none}}")

    s = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" role="img" aria-labelledby="t d">'
         % (W, H, W, H),
         '<title id="t">fpga-sim-core datapath</title>',
         '<desc id="d">Animated datapath diagram: one packet flows through PCS, MAC, ITCH 5.0 parser (%d cycles, fixed parameter), '
         'order book (%d cycle, fixed parameter, quote RAM latency %d), OFI (%d stages) and PCIe DMA (%d cycles, fixed parameter). '
         'Solid boxes are derived or executed by the module, dashed boxes are fixed documented model parameters. '
         'Illustrative; simulated cycles on a synthetic input, not a hardware measurement.</desc>'
         % (p["parser"], p["book"], p["bram"], p["ofi"], p["dma"]),
         "<style>%s</style>" % "".join(css),
         '<rect width="%d" height="%d" fill="%s"/>' % (W, H, th["bg"]), '<g class="an">',
         '<rect x="24" y="24" width="%d" height="%d" rx="10" fill="%s" stroke="%s"/>' % (W - 48, H - 48, th["panel"], th["border"])]
    for i, c in enumerate((th["red"], th["amber"], th["green"])):
        s.append('<circle cx="%d" cy="44" r="6" fill="%s"/>' % (46 + i * 20, c))
    s.append(t(W / 2, 49, "datapath.schematic  --  one message, wire to host  (illustrative)", 13, th["muted"], "middle"))
    s.append('<line x1="24" y1="64" x2="%d" y2="64" stroke="%s"/>' % (W - 24, th["border"]))
    s.append(t(56, 106, "A message flows through the cycle model", 26, th["fg"], weight="bold", family="Georgia,'Times New Roman',serif"))
    s.append(t(56, 134, "Solid box: cycles derived from the module as it runs.  Dashed box: fixed, documented model parameter.", 13, th["muted"]))
    # conveyor
    cy = by + bh / 2
    s.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" stroke-width="2"/>'
             % (bx - 8, cy, bx + n * (bw + gap) - gap + 8, cy, th["border"]))
    for i, (name, sub, cyc, derived, what) in enumerate(stages):
        x = bx + i * (bw + gap)
        dash = "" if derived else ' stroke-dasharray="6 5"'
        s.append('<rect x="%d" y="%d" width="%d" height="%d" rx="10" fill="%s" stroke="%s" stroke-width="1.5"%s/>'
                 % (x, by, bw, bh, th["bg"], th["green"] if derived else th["blue"], dash))
        s.append('<rect class="gl g%d" x="%d" y="%d" width="%d" height="%d" rx="10" fill="%s"/>' % (i, x, by, bw, bh, th["green"]))
        s.append(t(x + bw / 2, by + 30, name, 16, th["fg"], "middle", "bold"))
        s.append(t(x + bw / 2, by + 50, sub, 11, th["muted"], "middle"))
        s.append(t(x + bw / 2, by + 126, cyc, 13, th["green"] if derived else th["blue"], "middle", "bold"))
        s.append(t(x + bw / 2, by + bh + 28, what, 11, th["muted"], "middle"))
        s.append(t(x + bw / 2, by + bh + 48, ("derived" if derived else "fixed parameter"), 11,
                   th["green"] if derived else th["blue"], "middle"))
    # token: starts parked over the last stage (finished state)
    s.append('<g class="tk"><rect x="%.1f" y="%.1f" width="28" height="28" rx="6" fill="%s"/>'
             '<rect x="%.1f" y="%.1f" width="28" height="28" rx="6" fill="none" stroke="%s" stroke-width="1.5" opacity=".5" '
             'transform="translate(4 4)"/></g>' % (track0, cy - 14, th["green"], track0, cy - 14, th["green"]))
    s.append(t(56, 396, "Cycle model at 322.265625 MHz, synthetic ITCH input, simulated cycles. Not a hardware measurement.",
               13, th["amber"], family=SANS))
    s.append("</g></svg>")
    return "".join(s)


def main():
    p = mf.read_source_params()
    for name, th in mf.THEMES.items():
        with open(os.path.join(OUT, "datapath-%s.svg" % name), "w") as f:
            f.write(build(th, p) + "\n")


if __name__ == "__main__":
    main()
