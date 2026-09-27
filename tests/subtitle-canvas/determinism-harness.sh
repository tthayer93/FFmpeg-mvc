#!/bin/sh
# =============================================================================
# Subtitle-canvas determinism and interval-correctness harness.
#
# What this guards: the overlay that mvcsubdepth burns into a composed
# stereoscopic frame must be a pure function of the output pts.  Two runs of
# the same command over the same file must produce the same bytes, and the
# frames that carry a caption must be exactly the frames whose pts falls inside
# a caption's display interval.  Neither of those held before the filter paired
# its inputs by pts: the canvas it blended was whatever had last arrived from
# the transcoder's subtitle heartbeat, so caption edges could toggle
# paint/no-paint between two runs of one identical command.
#
# What it does:
#   1. builds a synthetic fixture in a scratch directory: a static,
#      spatially-busy pattern as the composed frame, plus a hand-written caption
#      track (caption-fixture.awk, no captured content in it at all) carrying
#      four captions and two clears at known times;
#   2. runs the shipping filter shape - three framed pads off the video plus
#      [rgba] out of mvcsubdepth - six times over it, and compares every run to
#      the first, reporting flaky_rows (the frames on which the runs did not
#      agree), which must be zero for all four pads;
#   3. runs the same graph WITHOUT the caption stream and compares the three
#      video pads, so the overlay branch provably cannot disturb them;
#   4. classifies every frame as painted / not painted by comparing its [rgba]
#      hash against its own raw hash (a caption here is fully opaque, so the two
#      differ if and only if caption pixels reached that frame) and requires the
#      result to match the schedule the fixture was built from - frame by frame,
#      on both sides of every caption edge;
#   5. requires the RIGHT caption: each caption has its own colour and its own
#      band, so the frames of one caption must share one overlay image that no
#      other caption produces.  Painting the wrong epoch is the symptom this
#      change is about, and that is what this catches;
#   6. requires the same frame count on [rgba] as on the video pads: the
#      pts-keyed pairing holds a frame until the caption side has passed it, so
#      a stranded frame would be a bug of the same family as the one replaced;
#   7. re-runs the shape with eof_action=repeat and with repeatlast=0 and
#      requires byte-identical output, because those spellings of the option are
#      still supposed to mean what they meant; and it re-runs the shape with
#      shortest=1 (alone, and spelled together with eof_action=pass) and requires
#      the whole graph to END at the subtitle side's EOF: shortest forces endall
#      over a pass preselection, so both spellings - not eof_action=pass alone -
#      must end the mvcsubdepth output short of the full delivery while the
#      video-side pads it shares keep all their frames (how far an endall run
#      drains before the EOF aborts the graph is scheduling-sensitive, so the lock
#      is that it falls short, not a fixed frame count).
#   8. builds a SECOND input from the same schedule with sup-fixture.awk - the raw
#      presentation-graphic segment stream that the supdec demuxer reads, rather
#      than the transport-stream wrapper caption-fixture.awk writes - and runs the
#      whole painted==covered determinism battery against it: it must be
#      deterministic across two runs, cover the frames the schedule says, and paint
#      the identical picture to the wrapped input on all four pads, so the delivered
#      overlay is shown to be a function of the pts schedule rather than of how the
#      caption track was framed.
#
# Most caption edges deliberately miss the frame grid, but a few sit exactly on
# it.  Those exact edges are what lock the filter's boundary rule: the frame on
# the edge keeps the previous display state, and the edge's successor is only
# allowed to cover the next grid line.  The media acceptance runs repeat that
# check on real packet times.
#
# Usage:  sh tests/subtitle-canvas/determinism-harness.sh [path-to-ffmpeg]
#         (default: the ffmpeg the build left at the repository root, or $FFMPEG)
# Exit:   0 = every check passed, 1 = at least one failed.  Runs in well under
#         a minute: the fixture is six seconds of a lossless pattern.
# =============================================================================
set -u

HERE=$(dirname "$0")
ROOT=$(cd "$HERE/../.." && pwd)
FF=${1:-${FFMPEG:-$ROOT/ffmpeg}}
N=6
FIXW=1280
FIXH=720
CAPW=640                 # one eye; the caption canvas is authored at eye size
CAPH=720
FR=25
DUR=6
NF=$((FR * DUR))         # frames expected on every pad
TICK=$((1000 / FR))      # frame period in ms; caption edges are off this grid

PASS=0
FAIL=0
ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL %s\n' "$1"; }
note() { printf '     %s\n' "$1"; }

LC_ALL=C
export LC_ALL

T=$(mktemp -d "${TMPDIR:-/tmp}/subcanvas.XXXXXX") || {
    printf 'harness: FATAL: mktemp failed\n' >&2; exit 1; }
cleanup() { rm -rf "$T"; }
trap cleanup EXIT HUP INT TERM

if [ ! -x "$FF" ]; then
    printf 'harness: FATAL: no ffmpeg at %s (build the tree first)\n' "$FF" >&2
    exit 1
fi
FFPROBE=$(dirname "$FF")/ffprobe
if [ ! -x "$FFPROBE" ]; then
    printf 'harness: FATAL: no ffprobe next to %s\n' "$FF" >&2
    exit 1
fi
banner=$("$FF" -hide_banner -version 2>&1 | head -n 1)
case "$banner" in
    "ffmpeg version "*) ok "running against the build under test: $banner" ;;
    *)                 bad "cannot get a version banner from $FF" ;;
esac

# The schedule.  A caption runs until the next entry, whatever that entry is -
# that is how this caption format ends a caption at all, since it carries no
# end time of its own.  "D" = a composition showing one caption object,
# "C" = a composition with no object, which blanks the screen.
#    type  start_ms  obj  x   y   w   h  colour
cat > "$T/schedule.txt" <<'SCHEDULE'
D 820 1 40 120 400 40 1
D 1620 2 60 300 480 40 2
C 2380
D 3180 3 80 480 400 40 3
D 4000 5 20 200 240 40 1
C 4400
D 4780 4 100 600 320 40 4
C 5580
SCHEDULE

# ----------------------------------------------------------------- the fixture
# A caption elementary stream cannot be read out of a file by anything in this
# tree, so the fixture is written as the smallest thing the shipped demuxers do
# read: one transport stream with one caption program.
if awk -f "$HERE/caption-fixture.awk" -v vw=$CAPW -v vh=$CAPH \
       < "$T/schedule.txt" > "$T/caps.ts" 2>"$T/fixture.err"; then
    ok "caption track written ($(wc -c < "$T/caps.ts" | tr -d ' ') bytes, synthetic)"
else
    bad "caption track generation failed"
    printf -- '--- generator stderr ---\n%s\n--- end ---\n' "$(cat "$T/fixture.err")"
fi

# one file, one lossless pattern, one caption stream.  copyts keeps the caption
# packet pts that the schedule was written with: without it the remux shifts the
# caption stream by its own start time, which would move every edge in the
# schedule out from under the comparison below.
if "$FF" -hide_banner -nostdin -v error -copyts \
         -f lavfi -i "smptebars=size=${FIXW}x${FIXH}:rate=${FR}:duration=${DUR}" \
         -i "$T/caps.ts" -map 0:v -map 1:s:0 \
         -c:v ffv1 -g "$FR" -fps_mode cfr -c:s copy -y "$T/fixture.mkv" \
         >"$T/mux.log" 2>&1 && [ -s "$T/fixture.mkv" ]; then
    ok "fixture muxed: one video stream, one caption stream"
else
    bad "fixture mux failed"
    printf -- '--- mux log ---\n%s\n--- end ---\n' "$(cat "$T/mux.log")"
fi

probe=$("$FFPROBE" -v error -show_entries stream=codec_type,codec_name \
              -of default=nw=1:nk=1 "$T/fixture.mkv" 2>&1)
case "$probe" in
    *ffv1*)  ok "fixture video decodes" ;;
    *)       bad "fixture video missing or undecodable: $probe" ;;
esac
case "$probe" in
    *pgs_subtitle*) ok "fixture captions decode as a bitmap caption stream" ;;
    *)              bad "fixture captions missing or undecodable: $probe" ;;
esac

# Nothing below this point means anything if the fixture is not what it claims to
# be, so stop here rather than report forty consequences of one cause.
if [ "$FAIL" != 0 ]; then
    printf 'harness: the fixture is not usable, stopping before the runs\n'
    printf '=== subtitle-canvas harness result: %d passed, %d failed ===\n' \
           "$PASS" "$FAIL"
    exit 1
fi

# ------------------------------------------------------------- the shape under
# test: four framed outputs off one decode.
G_HEAD="[0:v]split=4[mraw][ml0][mr0][msub];\
[mraw]format=rgba[mrawf];\
[ml0]format=rgba[ml0f];\
[mr0]format=rgba[mr0f];\
[ml0f]crop=iw/2:ih:0:0[Ml];\
[mr0f]crop=iw/2:ih:iw/2:0[Mr];\
[msub]format=rgba[vc];\
[0:s:0]format=rgba[sub];[vc][sub]"
G_TAIL="[rgba]"
OV_PASS="mvcsubdepth=depth=plane=0:eof_action=pass"
OV_REPEAT="mvcsubdepth=depth=plane=0:eof_action=repeat"
OV_RLAST0="mvcsubdepth=depth=plane=0:repeatlast=0"
OV_SHORTEST="mvcsubdepth=depth=plane=0:shortest=1"
OV_SHORTEST_PASS="mvcsubdepth=depth=plane=0:shortest=1:eof_action=pass"
G_FULL="${G_HEAD}${OV_PASS}${G_TAIL}"
G_NOSUB="[0:v]split=3[mraw][ml0][mr0];[mraw]format=rgba[mrawf];\
[ml0]format=rgba[ml0f];[mr0]format=rgba[mr0f];\
[ml0f]crop=iw/2:ih:0:0[Ml];[mr0f]crop=iw/2:ih:iw/2:0[Mr]"

# run_leg <tag> <graph> <pad=name>...
# Writes $T/<tag>-<name>.fmd5 and its .payload (the "0," data lines).  The input
# file is $IN, which the second-input leg repoints at the raw .sup fixture.
run_leg() {
    tag=$1; graph=$2; shift 2
    opts=""
    for spec in "$@"; do
        opts="$opts -map ${spec%%=*} -f framemd5 -y $T/$tag-${spec##*=}.fmd5"
    done
    # shellcheck disable=SC2086
    "$FF" -hide_banner -nostdin -v error -stats -i "${IN:-$T/fixture.mkv}" -an \
         -filter_complex "$graph" $opts 2>"$T/$tag.log"
    rc=$?
    for spec in "$@"; do
        grep '^0,' "$T/$tag-${spec##*=}.fmd5" > "$T/$tag-${spec##*=}.payload" 2>/dev/null
    done
    return $rc
}

# The wrapped transport-stream fixture is the input by default; the second-input
# leg repoints IN at the raw .sup fixture and restores it after.
IN=$T/fixture.mkv

# ------------------------------------------------------------------- the runs
allran=1
i=1
while [ "$i" -le "$N" ]; do
    if ! run_leg "run$i" "$G_FULL" "[mrawf]=raw" "[Ml]=l" "[Mr]=r" "[rgba]=rgba"; then
        bad "run $i exited nonzero"
        printf -- '--- run %d: %s--- ---\n' "$i" "$(cat "$T/run$i.log")"
        allran=0
    fi
    i=$((i + 1))
done
[ "$allran" = 1 ] && ok "$N runs of the shipping shape completed"

run_leg "rep1" "${G_HEAD}${OV_REPEAT}${G_TAIL}" "[mrawf]=raw" "[Ml]=l" "[Mr]=r" "[rgba]=rgba"
REPRC1=$?
run_leg "rep2" "${G_HEAD}${OV_REPEAT}${G_TAIL}" "[mrawf]=raw" "[Ml]=l" "[Mr]=r" "[rgba]=rgba"
REPRC2=$?
run_leg "rl0"  "${G_HEAD}${OV_RLAST0}${G_TAIL}" "[mrawf]=raw" "[Ml]=l" "[Mr]=r" "[rgba]=rgba"
RL0RC=$?
# The shortest=1 spellings are run several times each in section [7] (the endall
# tail is scheduling-sensitive), so they are not run here.
run_leg "nosub" "$G_NOSUB" "[mrawf]=raw" "[Ml]=l" "[Mr]=r"

md5of() { if [ -s "$1" ]; then md5sum "$1" | awk '{printf "%s", $1}'; else printf NOFILE; fi; }
nrows() { if [ -s "$1" ]; then wc -l < "$1" | tr -d ' '; else printf 0; fi; }
# byte-identical AND not nothing: two empty files are not agreement
samefile() { [ -s "$1" ] && [ -s "$2" ] && cmp -s "$1" "$2"; }

# ------------------------------------------ [1] nothing was stranded or dropped
# Every pad of every run must hold one line per frame of the fixture.  A frame
# the pairing held back, or one that never reached a pad, shortens that payload -
# and two short payloads comparing equal would be evidence of nothing.
i=1
while [ "$i" -le "$N" ]; do
    tally=0
    for pad in raw l r rgba; do
        n=$(nrows "$T/run$i-$pad.payload")
        if [ "$n" != "$NF" ]; then
            bad "run $i pad $pad delivered $n frames, expected $NF"
            tally=1
        fi
    done
    [ "$tally" = 0 ] && ok "run $i: all four pads delivered $NF frames"
    i=$((i + 1))
done

# --------------------------------------------- [2] six runs are one and the same
for pad in raw l r rgba; do
    files=""
    i=2
    while [ "$i" -le "$N" ]; do files="$files $T/run$i-$pad.payload"; i=$((i + 1)); done
    flaky=$(awk 'NR == FNR { ref[FNR] = $0; nr = FNR; next }
                 FNR <= nr && ref[FNR] != $0 { diff[FNR] = 1 }
                 END { c = 0; for (i in diff) c++; printf "%d", c }' \
              "$T/run1-$pad.payload" $files)
    if [ "$flaky" = 0 ]; then
        ok "pad $pad: flaky_rows=0 over $N runs (payload md5 $(md5of "$T/run1-$pad.payload"))"
    else
        bad "pad $pad: flaky_rows=$flaky over $N runs"
    fi
done

# ------------------------------------------- [3] the video pads and no captions
for pad in raw l r; do
    if samefile "$T/nosub-$pad.payload" "$T/run1-$pad.payload"; then
        ok "pad $pad: byte-identical to the run with no caption stream at all"
    else
        bad "pad $pad: differs from the run with no caption stream"
    fi
done

# ---------------------------------------- [4] painted frames == covered frames
# The lever is that a caption here is opaque: a frame's overlay hash differs
# from that same frame's raw hash exactly when caption pixels reached it.  What
# SHOULD be painted comes from the schedule, never from the output.
paint=$(awk -v tick="$TICK" '
    FILENAME ~ /schedule/    { kind[++ns] = $1; at[ns] = $2 + 0; next }
    FILENAME ~ /raw\.payload/   { raw[FNR] = $NF; nr = FNR; next }
    FILENAME ~ /rgba\.payload/  { over[FNR] = $NF; next }
    END {
        cov = 0; paint = 0; wrong = 0; bare = 0
        for (i = 1; i <= nr; i++) {
            t = (i - 1) * tick
            want = 0; own = 0
            for (k = 1; k <= ns; k++)
                if (at[k] < t) { want = (kind[k] == "D"); own = want ? k : 0 }
            if (want) cov++
            got = (over[i] != raw[i])
            if (got) paint++
            if (got != want) {
                wrong++
                if (wrong <= 8)
                    printf "MISMATCH|frame=%d|t_ms=%d|want=%d|got=%d\n", i - 1, t, want, got
            }
            if (!want && !got) bare++
            printf "FRAME|%d|%d|%d|%d|%s|%s\n", i - 1, t, want, own, over[i], raw[i]
        }
        printf "TOTALS|frames=%d|covered=%d|painted=%d|bare=%d|wrong=%d\n", \
               nr, cov, paint, bare, wrong
    }' "$T/schedule.txt" "$T/run1-raw.payload" "$T/run1-rgba.payload")

totals=$(printf '%s\n' "$paint" | grep '^TOTALS|')
# no sed in this shell: split with awk instead
val() { printf '%s\n' "$totals" | awk -F'|' -v K="$1" '
    { for (i = 2; i <= NF; i++) if (index($i, K "=") == 1) { print substr($i, length(K) + 2); next } }'; }

wrong=$(val wrong); cov=$(val covered); paintn=$(val painted)
bare=$(val bare); frames=$(val frames)
# The covered count again, counted straight off the schedule in its own words.
# Independent of the comparison above, so a schedule that turned up empty could
# not pass both sides of it.
expect=$(awk -v tick="$TICK" -v nf="$NF" '
    { kind[NR] = $1; at[NR] = $2 + 0; n = NR }
    END {
        c = 0
        for (i = 0; i < nf; i++) {
            t = i * tick; want = 0
            for (k = 1; k <= n; k++) if (at[k] < t) want = (kind[k] == "D")
            c += want
        }
        printf "%d", c
    }' "$T/schedule.txt")

if [ "${wrong:-x}" = 0 ]; then
    ok "overlay and schedule agree on all $NF frames ($paintn painted, $cov covered by schedule)"
else
    bad "overlay and schedule disagree on ${wrong:-?} frames"
    printf '%s\n' "$paint" | grep '^MISMATCH|' | head -n 8
fi
if [ "$cov" = "$expect" ] && [ "$frames" = "$NF" ]; then
    ok "the schedule covers $cov of the $NF frames, counted twice over"
else
    bad "the schedule covers $cov frames, expected $expect, over $frames frames"
fi
# a fixture that painted every frame, or none, would agree with itself and prove
# nothing, so both states have to be present
if [ "$cov" -gt 0 ] && [ "$bare" -gt 0 ] && [ "$cov" -lt "$NF" ]; then
    ok "the fixture exercises both states: $cov painted, $bare bare"
else
    bad "the fixture does not exercise both states ($cov painted, $bare bare of $NF)"
fi

# ------------------------------------------------- [5] the right caption painted
# One image per caption, and a different image per caption.
NCAP=$(awk '$1 == "D" { n++ } END { printf "%d", n + 0 }' "$T/schedule.txt")
oneimage=$(printf '%s\n' "$paint" | awk -F'|' '
    $1 == "FRAME" && $4 == 1 { k = $5 "|" $6; if (!(k in seen)) { seen[k] = 1; n++ } }
    END { printf "%d", n + 0 }')
classes=$(printf '%s\n' "$paint" | awk -F'|' '
    $1 == "FRAME" && $4 == 1 { if (!($6 in img)) { img[$6] = 1; n++ } }
    END { printf "%d", n + 0 }')
if [ "$oneimage" = "$NCAP" ] && [ "$classes" = "$NCAP" ]; then
    ok "the $NCAP captions produced $classes distinct overlay images, one each"
else
    bad "expected $NCAP distinct overlay images, saw $classes (frame-class pairs: $oneimage)"
fi
mixed=$(printf '%s\n' "$paint" | awk -F'|' '
    $1 == "FRAME" && $4 == 1 && ($5 in img) && img[$5] != $6 { c++ }
    $1 == "FRAME" && $4 == 1 && !($5 in img) { img[$5] = $6 }
    END { printf "%d", c + 0 }')
if [ "$mixed" = 0 ]; then ok "no caption interval contains two different overlay images"
else bad "$mixed frames sit inside a caption interval that changed image mid-caption"; fi

# a frame the schedule leaves bare must be untouched, byte for byte
barebad=$(printf '%s\n' "$paint" | awk -F'|' '
    $1 == "FRAME" && $4 == 0 && $6 != $7 { c++ } END { printf "%d", c + 0 }')
if [ "$barebad" = 0 ]; then ok "every frame outside every caption interval is passed through as is"
else bad "$barebad frames outside the caption intervals were modified"; fi

# -------------------------------------- [6] both sides of each caption boundary
frame_want() {
    printf '%s\n' "$paint" | awk -F'|' -v F="$1" '
        $1 == "FRAME" && $2 == F { print $4; found = 1; exit }
        END { if (!found) print "?" }'
}
frame_got() {
    printf '%s\n' "$paint" | awk -F'|' -v F="$1" '
        $1 == "FRAME" && $2 == F { print ($6 != $7); found = 1; exit }
        END { if (!found) print "?" }'
}
for edge in $(awk '$1 == "D" || $1 == "C" { printf "%s:%s\n", $1, $2 }' "$T/schedule.txt"); do
    kind=${edge%%:*}; tms=${edge##*:}
    if [ $((tms % TICK)) -eq 0 ]; then
        exact=1
        edge_frame=$((tms / TICK))
        before=$((edge_frame - 1))
        new=$((edge_frame + 1))       # the edge's state begins on the NEXT frame
        edge_want=$(frame_want "$edge_frame")
        edge_got=$(frame_got "$edge_frame")
    else
        exact=0
        before=$(((tms - 1) / TICK))
        new=$(((tms + TICK - 1) / TICK))
        edge_frame=-1
        edge_want=""
        edge_got=""
    fi
    # The expectation here comes from the schedule, not from the edge kind: a
    # caption that begins while another one is still on screen changes which
    # caption is painted, not whether a caption is painted at all.  The distinct
    # images above are what keep the filter honest about the first.
    want_before=$(frame_want "$before")
    got_before=$(frame_got "$before")
    want_new=$(frame_want "$new")
    got_new=$(frame_got "$new")
    case "$kind" in
        D) word="caption start" ;;
        *) word="caption end" ;;
    esac
    if [ "$before" -lt 0 ] || [ "$new" -ge "$NF" ]; then
        bad "$word at ${tms} ms: frames $before/$new fall outside the fixture"
        continue
    fi
    if [ "$exact" = 1 ]; then
        if [ "$got_before" = "$want_before" ] &&
           [ "$got_new" = "$want_new" ] &&
           [ "$edge_got" = "$edge_want" ] &&
           [ "$edge_got" = "$want_before" ]; then
            ok "$word at ${tms} ms: frames $before/$edge_frame hold the previous state, frame $new the new one"
        else
            bad "$word at ${tms} ms: frame $before says $got_before (want $want_before), edge frame $edge_frame says $edge_got (want $edge_want), frame $new says $got_new (want $want_new)"
        fi
    else
        if [ "$got_before" = "$want_before" ] && [ "$got_new" = "$want_new" ]; then
            ok "$word at ${tms} ms: frame $before holds the previous state, frame $new the new one"
        else
            bad "$word at ${tms} ms: frame $before says $got_before (want $want_before), frame $new says $got_new (want $want_new)"
        fi
    fi
done

# ------------------------------------------- [7] the option surface still means
# what it meant.  With this schedule eof_action=repeat and repeatlast=0 agree
# byte for byte with eof_action=pass; shortest=1 is deliberately different - it
# forces endall, which ends the whole graph at the subtitle side's EOF.
if [ "$REPRC1" = 0 ] && [ "$REPRC2" = 0 ] && [ "$RL0RC" = 0 ]; then
    repsame=YES; rl0same=YES
    for pad in raw l r rgba; do
        for tag in rep1 rep2; do
            samefile "$T/$tag-$pad.payload" "$T/run1-$pad.payload" || repsame=NO
        done
        samefile "$T/rl0-$pad.payload" "$T/run1-$pad.payload" || rl0same=NO
    done
    if [ "$repsame" = YES ]; then ok "eof_action=repeat gives the same bytes as eof_action=pass"
    else bad "eof_action=repeat differs from eof_action=pass on some pad"; fi
    if [ "$rl0same" = YES ]; then ok "repeatlast=0 is the same spelling as eof_action=pass"
    else bad "repeatlast=0 differs from eof_action=pass on some pad"; fi
else
    bad "the option-surface legs did not run (repeat rc=$REPRC1,$REPRC2 repeatlast=0 rc=$RL0RC)"
fi

# shortest=1 must still mean endall: the whole graph ends at the subtitle side's
# EOF, so the [rgba] pad (the only one that runs through mvcsubdepth) falls short
# of the full shipping delivery while the video-side pads it shares, which do not
# pass through the filter, keep every frame.  shortest=1:eof_action=pass is the
# SAME spelling - shortest forces endall over a pass preselection - so it must
# ALSO fall short, and not run the full length that eof_action=pass alone gives.
# (An if/else-if chain instead of framesync's two independent ifs would have let
# the pass preselection win, delivering all NF frames.)  How far one endall run
# drains before the subtitle EOF aborts the graph is scheduling-sensitive, and a
# run whose held video queue happens to reach the video's own end delivers the
# full count, so no single run is a fixed target.  The lock is therefore
# statistical rather than a fixed frame count: a mis-mirror to pass NEVER ends
# early, so if the shortest spelling delivers the full NF frames on EVERY one of
# KSHORTEST runs it is behaving like pass and that fails; ending short on at least
# one of those runs - with the video pads it shares still intact - is the pass.
KSHORTEST=5
# run_shortest_surface <graph> <tagbase>
#   runs the graph KSHORTEST times, and echoes "<allrc> <min_rgba> <raw_at_min>"
#   where min_rgba is the fewest [rgba] frames any single run delivered.
run_shortest_surface() {
    graph=$1; base=$2
    min=-1; raw_at_min=0; allrc=0; k=1
    while [ "$k" -le "$KSHORTEST" ]; do
        run_leg "$base$k" "$graph" "[mrawf]=raw" "[Ml]=l" "[Mr]=r" "[rgba]=rgba" || allrc=1
        n=$(nrows "$T/$base$k-rgba.payload")
        if [ "$min" -lt 0 ] || [ "$n" -lt "$min" ]; then
            min=$n; raw_at_min=$(nrows "$T/$base$k-raw.payload")
        fi
        k=$((k + 1))
    done
    printf '%s %s %s' "$allrc" "$min" "$raw_at_min"
}
set -- $(run_shortest_surface "${G_HEAD}${OV_SHORTEST}${G_TAIL}" "shrt")
shrt_allrc=$1; shrt_min=$2; shrt_raw=$3
set -- $(run_shortest_surface "${G_HEAD}${OV_SHORTEST_PASS}${G_TAIL}" "shrtp")
shrtp_allrc=$1; shrtp_min=$2; shrtp_raw=$3
if [ "$shrt_allrc" = 0 ]; then
    if [ "$shrt_min" -gt 0 ] && [ "$shrt_min" -lt "$NF" ] && [ "$shrt_raw" = "$NF" ]; then
        ok "shortest=1 forces endall: [rgba] fell short of $NF on at least one of $KSHORTEST runs (min $shrt_min, video pads at $shrt_raw)"
    else
        bad "shortest=1 ran like pass, not endall (min $shrt_min of $NF across $KSHORTEST runs, video side $shrt_raw)"
    fi
else
    bad "the shortest option-surface legs did not run cleanly (shortest rc=$shrt_allrc)"
fi
if [ "$shrtp_allrc" = 0 ]; then
    if [ "$shrtp_min" -gt 0 ] && [ "$shrtp_min" -lt "$NF" ] && [ "$shrtp_raw" = "$NF" ]; then
        ok "shortest=1:eof_action=pass is endall, not pass: [rgba] fell short on at least one of $KSHORTEST runs (min $shrtp_min of $NF)"
    else
        bad "shortest=1:eof_action=pass ran like pass, not endall (min $shrtp_min of $NF across $KSHORTEST runs, video side $shrtp_raw)"
    fi
else
    bad "the shortest:eof_action=pass option-surface leg did not run cleanly (rc=$shrtp_allrc)"
fi

# -------------------------------------- [8] second input: the raw .sup framing
# The same display schedule, generated by sup-fixture.awk as the raw presentation
# graphic segment stream that the supdec demuxer reads, instead of the transport-
# stream wrapper caption-fixture.awk writes.  How a caption track is framed is not
# part of the picture this filter paints: re-run the painted==covered determinism
# battery through that second demuxer and require the same delivered bytes.
IN=$T/sup.mkv
if awk -f "$HERE/sup-fixture.awk" -v vw=$CAPW -v vh=$CAPH \
       < "$T/schedule.txt" > "$T/caps.sup" 2>"$T/supgen.err"; then
    ok "raw .sup caption track written ($(wc -c < "$T/caps.sup" | tr -d ' ') bytes, synthetic)"
else
    bad "raw .sup generation failed"
    printf -- '--- generator stderr ---\n%s\n--- end ---\n' "$(cat "$T/supgen.err")"
fi
if "$FF" -hide_banner -nostdin -v error -copyts \
         -f lavfi -i "smptebars=size=${FIXW}x${FIXH}:rate=${FR}:duration=${DUR}" \
         -f sup -i "$T/caps.sup" -map 0:v -map 1:s:0 \
         -c:v ffv1 -g "$FR" -fps_mode cfr -c:s copy -y "$T/sup.mkv" \
         >"$T/supmux.log" 2>&1 && [ -s "$T/sup.mkv" ]; then
    ok "sup fixture muxed from the raw segment stream: one video, one caption stream"
else
    bad "sup fixture mux failed"
    printf -- '--- sup mux log ---\n%s\n--- end ---\n' "$(cat "$T/supmux.log")"
fi
supprobe=$("$FFPROBE" -v error -show_entries stream=codec_name \
               -of default=nw=1:nk=1 "$T/sup.mkv" 2>&1)
case "$supprobe" in
    *pgs_subtitle*) ok "sup captions decode as a bitmap caption stream" ;;
    *)              bad "sup captions missing or undecodable: $supprobe" ;;
esac

sup1rc=0; run_leg "sup1" "$G_FULL" "[mrawf]=raw" "[Ml]=l" "[Mr]=r" "[rgba]=rgba" || sup1rc=$?
sup2rc=0; run_leg "sup2" "$G_FULL" "[mrawf]=raw" "[Ml]=l" "[Mr]=r" "[rgba]=rgba" || sup2rc=$?
IN=$T/fixture.mkv

suptally=0; supsame=YES
for pad in raw l r rgba; do
    [ "$(nrows "$T/sup1-$pad.payload")" = "$NF" ] || suptally=1
    samefile "$T/sup2-$pad.payload" "$T/sup1-$pad.payload" || supsame=NO
done
if [ "$sup1rc" = 0 ] && [ "$sup2rc" = 0 ] && [ "$suptally" = 0 ]; then
    ok "sup: all four pads delivered $NF frames off the raw .sup input"
else
    bad "sup: the raw .sup pads did not deliver $NF frames (rc=$sup1rc,$sup2rc)"
fi
if [ "$sup1rc" = 0 ] && [ "$sup2rc" = 0 ] && [ "$supsame" = YES ]; then
    ok "sup: two runs are byte-identical on all four pads (deterministic through supdec)"
else
    bad "sup: the two raw .sup runs differ (framing-dependent nondeterminism)"
fi
# painted==covered, read off this fixture's own bytes against the schedule.
supwrong=$(awk -v tick="$TICK" '
    FILENAME ~ /schedule/         { kind[++ns] = $1; at[ns] = $2 + 0; next }
    FILENAME ~ /sup1-raw\.payload/  { raw[FNR] = $NF; nr = FNR; next }
    FILENAME ~ /sup1-rgba\.payload/ { over[FNR] = $NF; next }
    END {
        wrong = 0
        for (i = 1; i <= nr; i++) {
            t = (i - 1) * tick; want = 0
            for (k = 1; k <= ns; k++) if (at[k] < t) want = (kind[k] == "D")
            if ((over[i] != raw[i]) != want) wrong++
        }
        printf "%d", wrong
    }' "$T/schedule.txt" "$T/sup1-raw.payload" "$T/sup1-rgba.payload")
if [ "${supwrong:-x}" = 0 ]; then
    ok "sup: overlay and schedule agree on all $NF frames (painted==covered, raw .sup input)"
else
    bad "sup: overlay and schedule disagree on ${supwrong:-?} frames (raw .sup input)"
fi
# The decisive cross-input claim: the same picture, byte for byte, from the other
# framing (run1 is the wrapped transport-stream delivery of the same schedule).
supsamepict=YES
for pad in raw l r rgba; do
    samefile "$T/sup1-$pad.payload" "$T/run1-$pad.payload" || supsamepict=NO
done
if [ "$supsamepict" = YES ]; then
    ok "sup: the raw .sup input paints the identical picture to the wrapped input on all four pads"
else
    bad "sup: the raw .sup input differs from the wrapped input on some pad (framing leaked into the picture)"
fi

printf '=== subtitle-canvas harness result: %d passed, %d failed ===\n' "$PASS" "$FAIL"
[ "$FAIL" = 0 ] || exit 1
exit 0
