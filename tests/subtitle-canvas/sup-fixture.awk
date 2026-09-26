# Emit a synthetic presentation-graphic-stream caption track (.sup framing).
#
# The output is the raw segment stream that the raw-PGS reader takes: one
# framed record per subtitle segment,
#
#     "PG" <pts32> <dts32> <segment_type> <segment_length16> <payload>
#
# with pts in 90 kHz ticks and one record per segment, which is exactly what
# libavformat/supdec.c parses.  A caption is one display set:
#
#     PCS   presentation   (video size, composition, one object reference)
#     WDS   window                          (coords of the object box)
#     PDS   palette                         (the colours, in YCrCb + alpha)
#     ODS   object                          (the bitmap, run-length coded)
#     END   displayend                      (commit the set to the screen)
#
# and a caption CLEARING is the same set with no object: an empty display set
# means "screen blank from here on", which is how this stream format ends a
# caption without carrying an explicit end time.
#
# Input: a table on stdin, one display set per line, fields separated by
# blanks:
#
#     D <pts_ms> <obj_id> <x> <y> <w> <h> <color>   caption
#     C <pts_ms>                                    clear
#
# The geometry is deliberately simple - one solid opaque rectangle per caption.
# A solid run is the cheapest thing this bitmap encoding can say:
#
#     00 (C0|len/256) (len%256) color   - a long run of one colour
#     00 00                            - end of line
#
# Segment types and payload layouts match what libavcodec/pgssubdec.c parses,
# including the flag semantics that matter here: the presentation state bits
# (top two bits of the state byte) are set to "epoch continue" so each set is
# self-contained (the palette and the object of a set cannot be confused with
# the previous set's), and the object composition flag carries 0x40, which is
# what makes the decoder attach the palette to the rectangle at all.
#
# Everything is plain-decimal arithmetic and single-byte printf("%c"), so any
# awk will do; run it under LC_ALL=C so that %c stays one byte above 127.
#
# -v vw/-v vh set the declared video size, which is also the size of the
# canvas the transcoder will render these captions onto.
BEGIN {
    FS = "[ \t]+"
    OFS = ""
    if (vw == 0) vw = 640
    if (vh == 0) vh = 720
    nbad = 0
}

# ---------------------------------------------------------------- byte helpers
function put8(v) { printf "%c", v % 256 }
function put16(v) { put8(int(v / 256)); put8(v) }
function put24(v) { put8(int(v / 65536)); put16(v) }
function put32(v) { put16(int(v / 65536)); put16(v) }

# one framed segment: header, then the caller writes the payload
function seg_begin(type, len) {
    printf "PG"; put32(pts_ticks); put32(0)
    put8(type); put16(len)
}

# palette segment (0x14): id, version, then (color, y, cr, cb, alpha) entries
function put_palette(   i) {
    seg_begin(20, 2 + 5 * 5)
    put8(0); put8(0)
    put8(0); put8(0);   put8(0);   put8(0);   put8(0)     # index 0: transparent
    put8(1); put8(235); put8(128); put8(128); put8(255)   # index 1: near white
    put8(2); put8(80);  put8(90);  put8(240); put8(255)   # index 2: blue
    put8(3); put8(150); put8(240); put8(60);  put8(255)   # index 3: red
    put8(4); put8(60);  put8(60);  put8(90);  put8(255)   # index 4: green
}

# presentation segment (0x16): video size, rate code, composition id, state,
# palette update flag, palette id, object count, then the object references
function put_pcs(nobj, comp) {
    seg_begin(22, 11 + 8 * nobj)
    put16(vw); put16(vh); put8(32)
    put16(comp); put8(192); put8(0); put8(0); put8(nobj)
    if (nobj) {
        put16(oid); put8(0); put8(64)     # composition flag 0x40: palette applies
        put16(ox); put16(oy)
    }
}

# window segment (0x17): the object's box
function put_window() {
    seg_begin(23, 8)
    put16(ox); put16(oy); put16(ox + ow); put16(oy + oh)
}

# object segment (0x15): id, version, sequence descriptor (0xC0 = the first and
# last chunk of this object), total coded size, bitmap dimensions, then the
# run-length data - ow x oh solid colour, one run and one line end per row.
function put_object() {
    rle = 6 * oh
    seg_begin(21, 11 + rle)
    put16(oid); put8(0); put8(192)
    put24(rle + 4)
    put16(ow); put16(oh)
    for (i = 0; i < oh; i++) {
        put8(0); put8(192 + int(ow / 256)); put8(ow % 256); put8(ocol)
        put8(0); put8(0)
    }
}

# display-end segment (0x80): display/wait/presentation timeouts, all zero =
# hold this composition until the next one arrives
function put_end() {
    seg_begin(128, 3)
    put8(0); put8(0); put8(0)
}

# ---------------------------------------------------------------------- record
# A field-count mistake would silently produce a short segment, so check the
# shape before emitting rather than letting the reader shrug it off.
$1 == "C" {
    if (NF < 2) { print "malformed clear row: " $0 > "/dev/stderr"; nbad++; next }
    pts_ticks = $2 * 90
    put_pcs(0, NR)
    put_end()
    next
}
$1 == "D" {
    if (NF != 8) { print "malformed caption row: " $0 > "/dev/stderr"; nbad++; next }
    pts_ms = $2; oid = $3; ox = $4; oy = $5; ow = $6; oh = $7; ocol = $8
    if (ow < 64 || ox + ow > vw || oy + oh > vh) {
        print "object does not fit the declared size: " $0 > "/dev/stderr"
        nbad++
        next
    }
    pts_ticks = pts_ms * 90
    put_pcs(1, NR)
    put_window()
    put_palette()
    put_object()
    put_end()
    next
}
$1 == "" || $1 ~ /^#/ { next }
{ print "unknown row type: " $0 > "/dev/stderr"; nbad++ }

END { if (nbad) exit 1 }
