#!/usr/bin/env python3
"""
The OpenFX build against the FFGL build over time: a moving test card through
both, frame by frame.

    python3 tools/ofx_sequence.py --ofxprobe PATH --dir BUILD --gatest BUILD/gatest
                                  [--size 320x180] [--frames 120] [--fps 16]
                                  [--cue 10,300] [--control] ["ofxName=v|FFGL Name=v" ...]

The FFGL side is `gatest --pipe --fps 60`: frame n clocked at n / 60. The OpenFX
side renders the same frames as an image sequence on a 60 fps clip, every frame
in one instance (`--batch`), which is the same film position (AGENTS.md, "The
OpenFX build"). `--cue` presses Cue Dots on those frames in the FFGL run and, in
the OpenFX run, keyframes the toggle on at each and off three frames later.

It needs an ofxprobe with --seq, --frame-rate, --key and --batch: the test host
built from resolume-ofx-bridge in October 2026 for the fleet's OpenFX ports. The
stock ofxprobe renders one frame at time 0 and is what tools/ofx_agree.py uses.

The two builds' clocks are not the same arithmetic. gatest accumulates
dt x FPS; the OpenFX build multiplies. Where an exposure ends exactly on a
pull-down the sum can land a few ulps past it (at FPS 24 from frame 14, at 25
from frame 11, never at 16 or 18 in 700 frames), and the FFGL build then
captures that projector frame's picture one display frame early. Both
bookkeepings are simulated here -- gatest's clock and Gate.cpp's two held
buffers against GateOFX.cpp's holdAt -- and the frames where they pick a
different input frame are reported apart, not hidden and not counted.

Exits 1 when a frame the bookkeepings agree on differs by more than --max
levels; with --control, when none does (the settings are meant to differ).
"""
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

IDENTIFIER = "com.stoatworks.gate"
RATE = 60.0
FPS_INDEX = {16: 0, 18: 1, 24: 2, 25: 3}


#---------------------------------------------------------------------------
# The test card: hard edges, colour bars, a ramp, a scrolling checker, a moving
# block, and the frame number as bars, so a wrong input frame shows.
#---------------------------------------------------------------------------
def card(width, height, n):
    px = bytearray(width * height * 4)
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (16, 16, 16)]
    for y in range(height):
        for x in range(width):
            if y < height * 2 // 5:
                c = bars[min(7, x * 8 // width)]
            elif y < height * 3 // 5:
                v = (x * 255) // max(1, width - 1)
                c = (v, v, v)
            else:
                cx = ((x + 3 * n) // max(4, width // 20)) & 1
                cy = (y // max(4, height // 12)) & 1
                c = (220, 60, 30) if cx ^ cy else (30, 90, 200)
            i = (y * width + x) * 4
            px[i:i + 4] = bytes((c[0], c[1], c[2], 255))
    bx = (5 * n) % max(1, width - width // 8)
    for y in range(height * 2 // 5 - height // 10, height * 2 // 5 + height // 10):
        for x in range(bx, bx + width // 10):
            i = (y * width + x) * 4
            px[i:i + 4] = bytes((250, 250, 250, 255))
    for b in range(8):
        v = 255 if (n >> b) & 1 else 0
        for y in range(height - height // 16, height):
            for x in range(b * width // 8 + 2, (b + 1) * width // 8 - 2):
                i = (y * width + x) * 4
                px[i:i + 4] = bytes((v, v, v, 255))
    return px


def write_bmp(path, width, height, rgba):
    data = bytearray()
    for y in range(height - 1, -1, -1):
        row = rgba[y * width * 4:(y + 1) * width * 4]
        for x in range(width):
            data += bytes((row[4 * x + 2], row[4 * x + 1], row[4 * x], row[4 * x + 3]))
    header = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 32, 0, len(data), 2835, 2835, 0, 0)
    open(path, "wb").write(header + info + data)


def read_ppm(path):
    data = open(path, "rb").read()
    fields, i = [], 0
    while len(fields) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    return data[i + 1:]


#---------------------------------------------------------------------------
# The two bookkeepings. segments() is model::Segments' numbering.
#---------------------------------------------------------------------------
def segments(p0, p1):
    base = math.floor(p0)
    last = math.ceil(p1 - base) - 1
    return [base + j for j in range(last + 1)]


def ffgl_hold(fps, frames):
    """gatest --pipe's clock and Gate.cpp's held buffers: per display frame,
    (segments, the input frame each reads, the input frame 'above')."""
    out = []
    film, last_now, running = 0.0, -1.0, False
    held, current, held_frame, have_previous = [None, None], 0, -1, False
    for m in range(frames):
        now = m / RATE
        dt = 1.0 / 60.0 if last_now < 0 else min(max(now - last_now, 0.0), 0.25)
        last_now = now
        p1 = (film if running else 0.0) + dt * fps
        p0 = p1 - dt * fps
        running, film = True, p1
        segs = segments(p0, p1)
        latest = segs[-1]
        previous_frame = -1
        if latest > held_frame or held[current] is None:
            if held[current] is not None and held_frame >= 0:
                previous_frame, current, have_previous = held_frame, 1 - current, True
            held[current], held_frame = m, latest
        elif have_previous:
            previous_frame = held_frame - 1
        pictures = tuple(held[1 - current] if have_previous and k <= previous_frame else held[current] for k in segs)
        above = held[1 - current] if have_previous else held[current]
        out.append((tuple(segs), pictures, above))
    return out


def ofx_hold(fps, frames):
    """GateOFX.cpp's Clock and holdAt, with a fetch before frame 0 failing."""
    p1 = lambda t: fps * (t + 1.0) / RATE
    p0 = lambda t: p1(t) - fps / RATE
    latest = lambda t: segments(p0(t), p1(t))[-1]

    def capture(t, k):
        if latest(t) < k:
            return t
        j = max(0, math.ceil(t - (k * RATE / fps - 1.0)) - 1)
        while j > 0 and latest(t - j) < k:
            j -= 1
        while latest(t - (j + 1)) >= k:
            j += 1
        return t - j

    out = []
    for t in range(frames):
        newest = latest(t)
        current_time = capture(t, newest)
        previous = latest(current_time - 1)
        previous_time = capture(t, previous)
        older_up_to = previous if current_time == t else newest - 1
        have = previous_time >= 0
        older = previous_time if have else current_time
        segs = segments(p0(t), p1(t))
        pictures = tuple(older if have and k <= older_up_to else current_time for k in segs)
        out.append((tuple(segs), pictures, older))
    return out


def agreeing_frames(fps, frames):
    """True where both bookkeepings show the same input frames. An extra
    segment of zero length at an exposure's end (weight 0) does not count."""
    a, b = ffgl_hold(fps, frames), ofx_hold(fps, frames)
    result = []
    for m in range(frames):
        if a[m] == b[m]:
            result.append(True)
            continue
        pa, pb = dict(zip(a[m][0], a[m][1])), dict(zip(b[m][0], b[m][1]))
        t0, t1 = fps * m / RATE, fps * (m + 1) / RATE
        extra = set(pa) ^ set(pb)
        zero = all(abs(k - t1) < 1e-9 or abs(k + 1 - t0) < 1e-9 for k in extra)
        result.append(zero and a[m][2] == b[m][2] and all(pa[k] == pb[k] for k in set(pa) & set(pb)))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ofxprobe", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--gatest", required=True)
    ap.add_argument("--size", default="320x180")
    ap.add_argument("--frames", type=int, default=120)
    ap.add_argument("--fps", type=int, default=16, choices=sorted(FPS_INDEX))
    ap.add_argument("--cue", default="")
    ap.add_argument("--max", type=int, default=1)
    ap.add_argument("--control", action="store_true")
    ap.add_argument("pairs", nargs="*")
    args = ap.parse_args()

    width, height = (int(v) for v in args.size.split("x"))
    n = args.frames
    pairs = [f"fps={FPS_INDEX[args.fps]}|FPS={FPS_INDEX[args.fps]}"] + args.pairs
    ofx_sets, ffgl_sets = [], []
    for pair in pairs:
        ofx, ffgl = pair.split("|")
        ofx_sets += ["--set", ofx]
        ffgl_sets += ["--set", ffgl]
    cues = [int(c) for c in args.cue.split(",") if c]

    with tempfile.TemporaryDirectory() as tmp:
        raw = bytearray()
        for i in range(n):
            frame = card(width, height, i)
            raw += frame
            write_bmp(os.path.join(tmp, f"f{i:04d}.bmp"), width, height, frame)

        script = []
        if cues:
            path = os.path.join(tmp, "cues.txt")
            open(path, "w").write("".join(f"{c} Cue Dots 1\n" for c in cues))
            script = ["--script", path]
        ffgl = subprocess.run([args.gatest, "--pipe", "--size", args.size, "--fps", "60"] + ffgl_sets + script,
                              input=bytes(raw), capture_output=True).stdout
        if len(ffgl) != width * height * 4 * n:
            print("gatest --pipe did not render every frame")
            return 2

        batch = os.path.join(tmp, "batch.txt")
        with open(batch, "w") as f:
            for t in range(n):
                f.write(f"--time {t} --out-only {tmp}/o{t:04d}.ppm\n")
        keys = ["--key", "cueDots=0:0," + ",".join(f"{c}:1,{c + 3}:0" for c in cues)] if cues else []
        probe = subprocess.run([args.ofxprobe, "--no-system-dirs", "--dir", args.dir, "--render", IDENTIFIER,
                                "--seq", os.path.join(tmp, "f%04d.bmp"), "--frame-rate", "60"]
                               + ofx_sets + keys + ["--batch", batch], capture_output=True, text=True)
        if probe.returncode != 0 or "WARNING" in probe.stdout + probe.stderr:
            print((probe.stdout + probe.stderr)[-2000:])
            print("the test host did not render every frame (or did not know a parameter)")
            return 2
        outputs = [read_ppm(os.path.join(tmp, f"o{t:04d}.ppm")) for t in range(n)]

    agree = agreeing_frames(args.fps, n)
    worst = differ = counted = over = 0
    apart = []
    for t in range(n):
        o, g = outputs[t], ffgl[t * width * height * 4:(t + 1) * width * height * 4]
        frame_worst = frame_differ = 0
        for i in range(width * height):
            d = max(abs(o[3 * i + c] - g[4 * i + c]) for c in range(3))
            if d:
                frame_differ += 1
                frame_worst = max(frame_worst, d)
        if not agree[t]:
            apart.append((t, frame_worst))
            continue
        counted += 1
        worst = max(worst, frame_worst)
        differ += frame_differ
        over += frame_worst > args.max

    print(f"{counted} of {n} frames where the clocks agree: worst {worst}/255, "
          f"{differ} of {counted * width * height} pixels differ, {over} frames over {args.max}")
    if apart:
        print(f"{len(apart)} frames where gatest's accumulated clock captured a different input frame "
              f"(not counted): worst {max(w for _, w in apart)}/255, first {apart[:6]}")
    if args.control:
        return 0 if over > 0 else 1
    return 0 if over == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
