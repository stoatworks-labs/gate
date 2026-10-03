#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. The shaders are assembled at
#                 run time from their bodies, so the text compiled here
#                 is what `gatest --dump-shaders` writes: the exact strings the
#                 plugin hands the driver. Then a grep for every GLSL 4.10
#                 reserved word used as an identifier, because Apple's compiler
#                 and glslc accept some (`packed`) that Mesa refuses.
#   demo          the browser demo's copy of every shader, and of the constants
#                 it hands them, is still the plugin's, character for character
#                 (demo/tools/check_shaders.py). Says nothing about the page's
#                 hand PORT of the CPU half; only a reader checks that.
#   physics       every harness check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. Each measures its property out of
#                 the plugin's own picture:
#                   --flicker   a flat grey's spectrum is the shutter's Fourier
#                               series, the beat of blades x fps on 60 Hz on top
#                   --hold      the picture changes only at pull-downs, FPS a
#                               second, and every pull-down happens in the dark
#                   --scratch   a scratch holds its x while the picture moves
#                               under it; its side sets its polarity
#                   --weave     the offsets have the stated sigma and lag-1
#                               correlation, with and without shrinkage
#                   --fade      a neutral grey goes magenta, each dye on the law
#                   --resize    the print and the held pictures survive a resize
#                   --negative  every one of those FAILS on a perturbed model
#                 and again on Apple's SOFTWARE renderer (CI's), which is not
#                 bit-repeatable.
#   pipe          the fleet's frame contract, plus what a closed stdout (exit
#                 1, not SIGPIPE's 141), an unknown cue, a stepped option, a
#                 stepped event and a ramped slider do. (Gate has no boolean.)
#   sweep         does every control change the picture (the mean of 60 frames,
#                 150 for the rare events). A GLSL uniform whose name does not
#                 match the C++ is ignored without a word.
#   bench         the render cost and the state held, for the record.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#   openfx        the OpenFX bundle: CFBundleExecutable names the binary on
#                 disk, it exports OfxGetPlugin, it is universal, it ad-hoc
#                 signs (the release job's command), no installed plugin
#                 shadows its identifier, ofxprobe renders it -- and on
#                 ofxprobe's own input its frame 0 agrees with the FFGL
#                 build's frame 0 (gatest --pipe) to one 8-bit level, at the
#                 defaults and three other settings, while two settings that
#                 differ by one notch of Weave are told apart
#                 (tools/ofx_agree.py).
#   fusion        the OpenFX bundle where the host reports no frame rate, as
#                 Resolve 21.1's Fusion page does (no FrameRate anywhere, clip
#                 FrameRange [0, 0]): it must render, and render exactly what a
#                 24 fps host gets, its stated fallback. Needs a test host with
#                 `--quirks fusion` (OFXHOST=...); skipped without one.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

GATEST="$BUILD/gatest"

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails. No shaders at all is a FAILURE: it means the dump broke.
#---------------------------------------------------------------------------
step "shaders"
dir="$( mktemp -d )"
"$GATEST" --dump-shaders "$dir" >/dev/null
if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
else
	n=0; bad=0
	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done
	if [ "$n" -eq 0 ]; then
		fail "no shaders were dumped"
	elif [ "$bad" -eq 0 ]; then
		pass "all $n shaders compile"
	else
		fail "$bad of $n shaders do not compile"
	fi
fi
# The GLSL 4.10 reserved words (s3.6 of the spec: keywords reserved for future
# use, plus the ones the fleet has been bitten by) as identifiers. Apple's
# compiler and glslc accept `packed`; Mesa's llvmpipe, which the Arena gate
# runs on, does not. Matched as whole words followed by something an
# identifier is followed by, on lines that are not comments.
reserved='common|partition|active|asm|class|union|enum|typedef|template|this|packed|resource|goto|inline|noinline|public|static|extern|external|interface|long|short|half|fixed|unsigned|superp|input|output|hvec2|hvec3|hvec4|fvec2|fvec3|fvec4|sampler3DRect|filter|sizeof|cast|namespace|using|row_major|patch|sample|subroutine'
hits=$( cat "$dir"/*.vert "$dir"/*.frag | sed 's|//.*||' | grep -nwE "($reserved)" | grep -vE '^\s*$' || true )
if [ -z "$hits" ]; then
	pass "no GLSL 4.10 reserved word used as an identifier"
else
	fail "a GLSL 4.10 reserved word appears in a shader:"
	printf '%s\n' "$hits" | sed 's/^/      /'
fi
rm -rf "$dir"

#---------------------------------------------------------------------------
# The browser demo's copy of every shader, and of the print-data layout and
# the Model.h and Controls constants it hands them, is the plugin's, character
# for character. A drifted comment counts. A shader change here means
# `python3 demo/tools/sync_shaders.py`, never a hand edit of demo/plugin.js.
# It says nothing about the page's PORT of the CPU half; only a reader checks
# that.
#---------------------------------------------------------------------------
step "demo shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if out=$(python3 demo/tools/check_shaders.py 2>&1); then
		pass "$( printf '%s\n' "$out" | tail -1 )"
	else
		fail "the demo's shaders or constants have drifted from source/ -- run: python3 demo/tools/sync_shaders.py"
		printf '%s\n' "$out" | tail -12
	fi
else
	printf '   skipped: no demo/\n'
fi

for size in 320x180 1280x720; do
	step "physics at $size"
	for check in flicker hold scratch weave fade resize negative; do
		if out=$("$GATEST" --$check --size $size 2>&1); then
			pass "gatest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "gatest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

# The whole physics list again on Apple's SOFTWARE renderer, which is what
# GitHub's macOS runners have. It is not repeatable at the last bit (repousse's
# resize check failed CI by one ulp), so a check that asserts exactness on this
# Mac's GPU is found here before CI finds it.
step "physics at 320x180 on the software renderer (CI's)"
for check in flicker hold scratch weave fade resize negative; do
	if out=$(GATEST_RENDERER=software "$GATEST" --$check --size 320x180 2>&1); then
		pass "gatest --$check (software): $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "gatest --$check at 320x180 on the software renderer -- run: GATEST_RENDERER=software $GATEST --$check --size 320x180"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

step "names"
if out=$("$GATEST" --names 2>&1); then
	pass "$( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
else
	fail "gatest --names"
	printf '%s\n' "$out" | sed 's/^/      /'
fi

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- and a cue naming no parameter must be refused rather than
# silently doing nothing to the picture.
#---------------------------------------------------------------------------
step "pipe"
W=64; H=36
frame=$(( W * H * 4 ))
raw=$( mktemp ); cues=$( mktemp ); out=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
got=$( "$GATEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever gatest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$GATEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
# A reader that hangs up early (`| head -c 1`, ffmpeg dying) must end the run
# with exit 1 and a message, not SIGPIPE's silent 141.
head -c $(( frame * 20 )) /dev/zero > "$raw"
"$GATEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout ends the run with exit 1, not SIGPIPE"
else
	fail "a closed stdout gave exit $status, not 1"
fi
# A failed render ends the run with exit 1 too.
"$GATEST" --pipe --size ${W}x${H} --fail-render-at 3 < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 1 ]; then
	pass "a failed render ends the run with exit 1"
else
	fail "a failed render gave exit $status, not 1"
fi
# An option STEPS between cues. The print changes every frame (weave, dust,
# the flicker), so no two frames of a run are alike; instead each frame is
# compared with the same frame of a run that held one value throughout. Lamp 0
# at frame 0 and 2 at frame 4: frames 0-3 must be the Carbon Arc run's, 4-5
# the Tungsten run's. A ramp would put Xenon (1) on frame 2 and match neither.
python3 -c "import sys; sys.stdout.buffer.write(bytes([200,200,200,255]) * ($W * $H * 6))" > "$raw"
steps() {  # steps NAME A B : scripted A->B at frame 4 against constant A and constant B
	printf '0 %s %s\n4 %s %s\n' "$1" "$2" "$1" "$3" > "$cues"
	"$GATEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
	"$GATEST" --pipe --size ${W}x${H} --set "$1=$2" < "$raw" > "$out.a" 2>/dev/null
	"$GATEST" --pipe --size ${W}x${H} --set "$1=$3" < "$raw" > "$out.b" 2>/dev/null
	python3 - "$out" "$out.a" "$out.b" $frame <<'PY'
import sys
s, a, b = (open(p, 'rb').read() for p in sys.argv[1:4]); n = int(sys.argv[4])
f = lambda d, i: d[i * n:(i + 1) * n]
ok = len(s) == 6 * n and all(f(s, i) == f(a, i) for i in range(4)) and all(f(s, i) == f(b, i) for i in (4, 5)) and f(a, 2) != f(b, 2)
sys.exit(0 if ok else 1)
PY
}
if steps Lamp 0 2; then
	pass "an option cue steps (frames 0-3 Carbon Arc, 4-5 Tungsten), no ramp between"
else
	fail "an option cue did not step -- see the loadScript/valueAt kinds in tools/gatest/main.cpp"
fi
# An event FIRES on its cue frame only. Cue Dots 0 at frame 0 and 1 at frame
# 4: frames 0-3 must be the no-press run's, and frame 4 must not (the dots are
# on the print from the frame in the gate). A ramp would cross 0.5 on frame 2
# and put the dots there.
printf '0 Cue Dots 0\n4 Cue Dots 1\n' > "$cues"
"$GATEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
"$GATEST" --pipe --size ${W}x${H} < "$raw" > "$out.a" 2>/dev/null
if python3 - "$out" "$out.a" $frame <<'PY'
import sys
s, a = (open(p, 'rb').read() for p in sys.argv[1:3]); n = int(sys.argv[3])
f = lambda d, i: d[i * n:(i + 1) * n]
sys.exit(0 if len(s) == 6 * n and all(f(s, i) == f(a, i) for i in range(4)) and f(s, 4) != f(a, 4) else 1)
PY
then
	pass "an event cue fires on its frame only (frames 0-3 untouched, the dots from frame 4)"
else
	fail "an event cue did not fire on its frame alone"
fi
# A slider RAMPS: Vignette 0 at frame 0 and 1 at frame 4 puts 0.5 on frame 2.
printf '0 Vignette 0\n4 Vignette 1\n' > "$cues"
"$GATEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
"$GATEST" --pipe --size ${W}x${H} --set "Vignette=0.5" < "$raw" > "$out.a" 2>/dev/null
if python3 - "$out" "$out.a" $frame <<'PY'
import sys
s, a = (open(p, 'rb').read() for p in sys.argv[1:3]); n = int(sys.argv[3])
sys.exit(0 if len(s) == 6 * n and s[2 * n:3 * n] == a[2 * n:3 * n] else 1)
PY
then
	pass "a slider cue ramps (Vignette 0 -> 1 over frames 0-4 is 0.5 on frame 2)"
else
	fail "a slider cue did not ramp"
fi
rm -f "$out.a" "$out.b"
rm -f "$raw" "$cues" "$out"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$GATEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$GATEST" --bench --frames 60 2>&1 | sed -n '3,6p' | sed 's/^/   /'

BUNDLE="$BUILD/Gate.bundle"
BIN="$BUNDLE/Contents/MacOS/Gate"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.gate" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Gate.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Gate" "id:          GA01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

#---------------------------------------------------------------------------
# The OpenFX bundle.
#
# cmake/InfoOFX.plist.in is copied from repo to repo, and the version it is
# usually copied from had the PREVIOUS plugin's name hardcoded into
# CFBundleExecutable. That does not fail the build: the bundle assembles, lipo
# and nm both pass, ofxprobe loads it and renders a correct frame. It fails at
# RELEASE time, in codesign, with "code object is not signed at all" -- so the
# plist is checked against the binary on disk and the release job's codesign
# is run against a copy.
#
# ofxprobe also scans /Library/OFX/Plugins, and the FIRST plugin with an
# identifier wins: an installed Gate there would be what got measured. Checked
# before anything is rendered.
#---------------------------------------------------------------------------
OFX="$BUILD/Gate.ofx.bundle"
if [ "$(uname)" = "Darwin" ]; then
	step "openfx"
	if [ ! -d "$OFX" ]; then
		fail "no OpenFX bundle at $OFX (configured with -DBUILD_OFX=OFF?)"
	else
		exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$OFX/Contents/Info.plist" 2>/dev/null)
		ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$OFX/Contents/Info.plist" 2>/dev/null)
		OFXBIN="$OFX/Contents/MacOS/$exe"
		if [ -n "$exe" ] && [ -f "$OFXBIN" ]; then
			pass "CFBundleExecutable ($exe) is on disk"
		else
			fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
		fi
		if [ "$ident" = "com.stoatworks.gate.ofx" ]; then
			pass "CFBundleIdentifier is $ident"
		else
			fail "CFBundleIdentifier is '$ident'"
		fi
		syms=$(nm -gU "$OFXBIN" 2>/dev/null)
		case "$syms" in
			*_OfxGetPlugin*) pass "exports OfxGetPlugin" ;;
			*) fail "no OfxGetPlugin -- no OFX host will load it" ;;
		esac
		archs=$(lipo -archs "$OFXBIN" 2>/dev/null)
		case "$archs" in *arm64*x86_64*|*x86_64*arm64*) pass "universal ($archs)" ;; *) fail "not universal (got: $archs)" ;; esac
		tmp=$(mktemp -d)
		cp -R "$OFX" "$tmp/"
		if codesign --force --sign - --timestamp=none "$tmp/Gate.ofx.bundle" >/dev/null 2>&1; then
			pass "ad-hoc signs (the command the release job runs)"
		else
			fail "ad-hoc signing the OpenFX bundle failed"
		fi
		rm -rf "$tmp"

		OFXPROBE="${OFXPROBE:-../resolume-ofx-bridge/build/ofxprobe}"
		[ -x "$OFXPROBE" ] || OFXPROBE="$HOME/Projects/resolume/resolume-ofx-bridge/build/ofxprobe"
		if [ ! -x "$OFXPROBE" ]; then
			printf '   skipped: ofxprobe not built at %s -- the OpenFX render is unchecked\n' "$OFXPROBE"
		else
			installed=$("$OFXPROBE" --json 2>/dev/null)
			case "$installed" in
				*'"com.stoatworks.gate"'*) fail "an installed plugin already has the identifier com.stoatworks.gate -- ofxprobe would measure that, not this build" ;;
				*) pass "no installed plugin shadows com.stoatworks.gate" ;;
			esac
			out=$("$OFXPROBE" --dir "$BUILD" --render com.stoatworks.gate --size 320x180 2>&1)
			if ! printf '%s\n' "$out" | grep -q "rendered"; then
				fail "the OpenFX bundle does not render"
				printf '%s\n' "$out" | sed 's/^/      /'
			elif printf '%s\n' "$out" | grep -qE "^ *0 of [0-9]+ bytes differ"; then
				fail "the OpenFX bundle renders its input unchanged"
			else
				pass "ofxprobe renders it ($(printf '%s\n' "$out" | grep -oE '[0-9]+ of [0-9]+ bytes differ'))"
			fi

			agree() {  # agree LABEL [--control] PAIRS...
				local label=$1; shift
				local result
				if result=$(python3 tools/ofx_agree.py --ofxprobe "$OFXPROBE" --dir "$BUILD" --gatest "$GATEST" "$@" 2>&1); then
					pass "$label: $( printf '%s\n' "$result" | tail -1 )"
				else
					fail "$label: $( printf '%s\n' "$result" | tail -1 )"
				fi
			}
			agree "frame 0 agrees with the FFGL build at the defaults"
			agree "frame 0 agrees: 16 fps, 1 blade at 261 deg, carbon arc, framed up, every mark 1, Age 1, Mix 0.7" \
				"fps=0|FPS=0" "blades=0|Blades=0" "shutterAngle=0.8|Shutter Angle=0.8" "lamp=0|Lamp=0" \
				"framing=0.3|Framing=0.3" "weave=1|Weave=1" "shrinkage=1|Shrinkage=1" "hair=1|Hair=1" \
				"scratches=1|Scratches=1" "dust=1|Dust=1" "splices=1|Splices=1" "age=1|Age=1" \
				"vignette=1|Vignette=1" "mix=0.7|Mix=0.7"
			agree "frame 0 agrees: 25 fps, 2 blades at 180, tungsten, framed down, weave 0.8, Age 0.5" \
				"fps=3|FPS=3" "blades=1|Blades=1" "shutterAngle=0.5|Shutter Angle=0.5" "lamp=2|Lamp=2" \
				"framing=0.62|Framing=0.62" "weave=0.8|Weave=0.8" "shrinkage=0.9|Shrinkage=0.9" \
				"scratches=1|Scratches=1" "dust=1|Dust=1" "age=0.5|Age=0.5" "vignette=1|Vignette=1"
			agree "frame 0 agrees: framed 0.4 down, past the frame line into the next picture" \
				"framing=0.9|Framing=0.9" "age=0.3|Age=0.3"
			agree "the comparison can fail: Weave 0.25 against 0.26 is told apart" --control "weave=0.25|Weave=0.26"
		fi

		#-------------------------------------------------------------------
		# Fusion. Resolve's Fusion page reports no frame rate on the effect or
		# any clip, and the Support library turns that into an exception; the
		# first build let it escape render and the whole composition failed.
		# A test host with `--quirks fusion` presents the same properties.
		# Here a 12-frame sequence at FPS 16 must render under it, frame for
		# frame what a 24 fps host gets (the fallback), and differ from a
		# 60 fps one (so the fallback, not luck, is what was used).
		#-------------------------------------------------------------------
		step "fusion (no frame rate from the host)"
		# Captured and matched, not piped into grep -q: under pipefail a grep
		# that finds its match early can fail the pipeline (see registration).
		hasquirks() { local h; h=$("$1" --help 2>&1); case "$h" in *--quirks*) return 0 ;; *) return 1 ;; esac; }
		if [ -z "${OFXHOST:-}" ] && [ -x "${OFXPROBE:-}" ] && hasquirks "$OFXPROBE"; then
			OFXHOST="$OFXPROBE"
		fi
		if [ -z "${OFXHOST:-}" ] || [ ! -x "$OFXHOST" ] || ! hasquirks "$OFXHOST"; then
			printf '   skipped: no test host with --quirks fusion (set OFXHOST to one)\n'
		else
			tmp=$(mktemp -d)
			python3 - "$tmp" <<'PY'
import sys
d = sys.argv[1]
W, H = 96, 54
for n in range(12):
    px = bytearray()
    for y in range(H):
        for x in range(W):
            bar = n * 6 <= x < n * 6 + 8
            px += bytes((250, 250, 250)) if bar else bytes(((x * 5) % 256, (y * 9) % 256, 40 + 16 * n))
    open(f"{d}/f{n:04d}.ppm", "wb").write(b"P6\n%d %d\n255\n" % (W, H) + px)
PY
			common=(--no-system-dirs --dir "$BUILD" --render com.stoatworks.gate --seq "$tmp/f%04d.ppm" --set fps=0 --set framing=0.35 --time 9)
			fusion=$("$OFXHOST" "${common[@]}" --quirks fusion --out-only "$tmp/fusion.ppm" 2>&1)
			if ! printf '%s\n' "$fusion" | grep -q "^rendered"; then
				fail "does not render under --quirks fusion -- a frame-rate read escapes"
				printf '%s\n' "$fusion" | grep -iE 'fail|error' | sed 's/^/      /'
			else
				pass "renders under --quirks fusion (no FrameRate, FrameRange [0,0])"
				"$OFXHOST" "${common[@]}" --frame-rate 24 --out-only "$tmp/r24.ppm" >/dev/null 2>&1
				"$OFXHOST" "${common[@]}" --frame-rate 60 --out-only "$tmp/r60.ppm" >/dev/null 2>&1
				if cmp -s "$tmp/fusion.ppm" "$tmp/r24.ppm" && ! cmp -s "$tmp/fusion.ppm" "$tmp/r60.ppm"; then
					pass "under Fusion it renders exactly what a 24 fps host gets (and not a 60 fps one)"
				else
					fail "under Fusion it does not match the 24 fps fallback"
				fi
			fi
			rm -rf "$tmp"
		fi
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
