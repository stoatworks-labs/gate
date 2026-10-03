#!/usr/bin/env python3
"""
The OpenFX build against the FFGL build, on the same input, pixel for pixel.

    python3 tools/ofx_agree.py --ofxprobe PATH --dir BUILD --gatest BUILD/gatest
                               [--size 320x180] [--max 1] ["ofxName=v|FFGL Name=v" ...]
    python3 tools/ofx_agree.py ... --control "weave=0.25|Weave=0.26"

ofxprobe (resolume-ofx-bridge) renders the OFX plugin once, at time 0 on a
60 fps clip, on a ramp of its own; it writes the input and the output side by
side into a BMP. The input half is read back and piped through
`gatest --pipe --fps 60` as frame 0, which is the FFGL plugin's own frame 0 on
the same 60 Hz clock -- the two builds' film positions are the same number
there (AGENTS.md, "The OpenFX build"). Each pair names the same setting in
both builds: the OFX parameter's script name and value, then the FFGL
parameter's display name and value.

Prints the worst channel difference in 8-bit levels and how many pixels
differ, and exits 1 when the worst exceeds --max. With --control the sense is
reversed: the two settings are meant to DIFFER, and it exits 1 unless the
worst exceeds --max -- so the comparison is shown to be able to fail.

One frame only: the stock ofxprobe has no clip to fetch earlier frames from,
so the hold and the flicker over time are checked elsewhere (AGENTS.md).
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

IDENTIFIER = "com.stoatworks.gate"


def read_bmp24(path):
    """Rows top first, each W*3 bytes of BGR."""
    data = open(path, "rb").read()
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24 or height <= 0:
        raise SystemExit(f"{path}: expected a bottom-up 24-bit BMP")
    stride = (width * 3 + 3) & ~3
    rows = [data[offset + y * stride: offset + y * stride + width * 3] for y in range(height)]
    rows.reverse()
    return width, height, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ofxprobe", required=True)
    ap.add_argument("--dir", required=True, help="the directory holding Gate.ofx.bundle")
    ap.add_argument("--gatest", required=True)
    ap.add_argument("--size", default="320x180")
    ap.add_argument("--max", type=int, default=1, help="worst level difference allowed")
    ap.add_argument("--control", action="store_true", help="the settings must differ")
    ap.add_argument("pairs", nargs="*")
    args = ap.parse_args()

    width, height = (int(v) for v in args.size.split("x"))
    ofx_sets, ffgl_sets = [], []
    for pair in args.pairs:
        ofx, ffgl = pair.split("|")
        ofx_sets += ["--set", ofx]
        ffgl_sets += ["--set", ffgl]

    with tempfile.TemporaryDirectory() as tmp:
        bmp = os.path.join(tmp, "probe.bmp")
        probe = subprocess.run(
            [args.ofxprobe, "--dir", args.dir, "--render", IDENTIFIER, "--size", args.size, "--out", bmp] + ofx_sets,
            capture_output=True, text=True)
        if probe.returncode != 0 or "rendered" not in probe.stdout or "WARNING" in probe.stderr:
            print(probe.stdout + probe.stderr)
            print("ofxprobe did not render (or did not know a parameter)")
            return 2

        bmp_width, _, rows = read_bmp24(bmp)
        gap = bmp_width - 2 * width
        pipe_in = bytearray()
        ofx_out = []
        for row in rows:
            for x in range(width):
                b, g, r = row[3 * x: 3 * x + 3]
                pipe_in += bytes((r, g, b, 255))
            ofx_out.append(row[3 * (width + gap): 3 * (2 * width + gap)])

        gatest = subprocess.run([args.gatest, "--pipe", "--size", args.size, "--fps", "60"] + ffgl_sets,
                                input=bytes(pipe_in), capture_output=True)
        ffgl_out = gatest.stdout
        if gatest.returncode != 0 or len(ffgl_out) != width * height * 4:
            print(gatest.stderr.decode(errors="replace"))
            print("gatest --pipe did not render one frame")
            return 2

    worst = differ = brighter = darker = 0
    for y in range(height):
        row = ofx_out[y]
        for x in range(width):
            ffgl = ffgl_out[(y * width + x) * 4:(y * width + x) * 4 + 3]
            b, g, r = row[3 * x: 3 * x + 3]
            ofx = (r, g, b)
            d = max(abs(ofx[c] - ffgl[c]) for c in range(3))
            if d:
                differ += 1
                worst = max(worst, d)
                brighter += sum(1 for c in range(3) if ofx[c] > ffgl[c])
                darker += sum(1 for c in range(3) if ofx[c] < ffgl[c])

    summary = (f"worst {worst}/255, {differ} of {width * height} pixels differ "
               f"(channels OFX brighter {brighter}, darker {darker})")
    print(summary)
    if args.control:
        return 0 if worst > args.max else 1
    return 0 if worst <= args.max else 1


if __name__ == "__main__":
    sys.exit(main())
