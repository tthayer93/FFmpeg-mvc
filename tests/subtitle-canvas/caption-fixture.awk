# Emit a synthetic bitmap-caption track as one demuxable transport stream.
#
# One line of the schedule on stdin is one display set, fields separated by
# blanks:
#
#     D <pts_ms> <obj_id> <x> <y> <w> <h> <colour>   show a caption
#     C <pts_ms>                                     blank the screen
#
# A caption is a complete, self-contained display set - presentation, palette,
# window, object, display-end - with one solid opaque rectangle, so nothing in it
# depends on an earlier set.  A clear is the same shape with no object at all,
# which is how this format blanks the screen: the caption decoder hands the
# consumer a subtitle with no rectangles for it, and the transcoder builds a
# blank canvas from that.
#
# The captions carry no end time, because this stream format has none: a caption
# is retired by the next display set.  That is what makes the canvas push
# schedule the transcoder hands the filtergraph a pure function of the packet
# times (see the contract above sub2video_push_ref() in fftools/ffmpeg_filter.c),
# which is the property the harness measures.
#
# Why a transport stream rather than the bare segment stream: nothing in this
# tree can demux a caption elementary stream out of a file, so the smallest thing
# the shipped demuxers recognise is one program with one caption stream.  The
# table bytes follow the in-tree generator for the same format,
# tests/fate/h264-mvc/mvc-mkpgs.pl, including the profile registration descriptor
# the demuxer keys on.  The four tag bytes of that descriptor are written as
# numbers, exactly as that generator does, so this file stays free of the
# spellings a publishable tree must not contain.
#
# Section CRCs are computed here: the MPEG-2 table CRC, polynomial 0x04c11db7
# (79764919 below), initial value 2^32-1, most significant bit first, no final
# complement.  The bit operations are done with arithmetic rather than an awk
# bit-function extension so any awk runs this.  Getting them wrong is not a
# silent failure: the demuxer checks them and refuses the table.
#
# Every packet is stuffed to exactly 188 bytes, so a packet's payload size has to
# be known before its bytes are written.  Each one is computed from the
# fixed-width field layout, which is why the display-set length is summed up
# front and the payload goes out through pw(), which opens packets as it fills
# them.
#
# -v vw / -v vh set the declared video size, which is also the size of the canvas
# the transcoder renders these captions onto; the geometry below is authored in
# that space.  Run it under LC_ALL=C so that "%c" stays one byte above 127.
BEGIN {
    FS = "[ \t]+"
    if (vw == 0)   vw = 640
    if (vh == 0)   vh = 720
    if (ppid == 0) ppid = 4097            # 0x1001, the programme map table
    if (spid == 0) spid = 4352            # 0x1100, the caption stream
    program = 1
    nbad = 0
    last_ms = -1
    write_pat()
    write_pmt()
}

# ------------------------------------------------------------- raw byte output
# Every byte goes through raw8(), which also counts them: the END rule below
# refuses a stream whose length is not a whole number of transport packets, so a
# mistake in any of the sizes computed above is reported instead of demuxing as
# garbage.
function raw8(v) { bytes++; printf "%c", v % 256 }

# put8() is raw8() with the byte folded into the table CRC when one is open.
function put8(v,   b) { b = v % 256; if (crc_on) crc_feed(b); raw8(b) }
function put16(v) { put8(int(v / 256)); put8(v) }
function put24(v) { put8(int(v / 65536)); put16(v) }
function put_crc() {
    put8(int(crc / 16777216)); put8(int(crc / 65536))
    put8(int(crc / 256));      put8(crc)
}

# xor of two values below 2^32, by bit arithmetic: no awk bit-function extension
# is assumed anywhere in this file.
function bxor(a, b,   r, p) {
    r = 0
    for (p = 1; p < 4294967296; p *= 2) {
        if ((a % 2) != (b % 2))
            r += p
        a = int(a / 2); b = int(b / 2)
    }
    return r
}

function crc_feed(v,   k) {
    crc = bxor(crc, v * 16777216)                       # v <<= 24, xor in
    for (k = 0; k < 8; k++)
        if (crc >= 2147483648)
            crc = bxor((crc - 2147483648) * 2, 79764919)
        else
            crc = crc * 2
}

# or of two values below 2^32, for the same reason bxor() is here: the reserved
# bits of a pid field have to be ORed in, and adding them is only the same thing
# while the pid stays under 0x1000.
function bor(a, b,   r, p) {
    r = 0
    for (p = 1; p < 4294967296; p *= 2) {
        if ((a % 2) != 0 || (b % 2) != 0)
            r += p
        a = int(a / 2); b = int(b / 2)
    }
    return r
}

# ---------------------------------------------------------------- packet level
# One 188-byte transport packet carrying plen payload bytes, stuffed so that it
# comes to exactly 188.  The continuity counter is per stream, as it must be.
function packet(pid, pusi, plen,   room, i, c) {
    c = cc[pid]
    cc[pid] = (c + 1) % 16
    room = 184 - plen
    raw8(71)                                           # the sync byte
    raw8(int(pid / 256) % 32 + (pusi ? 64 : 0))
    raw8(pid)
    raw8((room > 0 ? 48 : 16) + c)                     # adaptation, payload
    if (room > 0) {
        raw8(room - 1)                                 # field length
        if (room > 1) raw8(0)                          # no flags set
        for (i = 2; i < room; i++) raw8(255)           # stuffing
    }
}

# One program, pointing at the one programme map table.  A section is its table
# id, the two bytes that carry the syntax indicator and the section length
# together, the body, and the CRC.
function write_pat(   len) {
    len = 1 + 16                                       # pointer field, section
    packet(0, 1, len)
    crc_on = 0; raw8(0)                                # pointer field
    crc_on = 1; crc = 4294967295
    put8(0); put8(176); put8(13)                       # id, syntax + length
    put16(program); put8(193); put8(0); put8(0)        # number, version, tier
    put16(program); put16(bor(57344, ppid))            # this program's table
    crc_on = 0; put_crc()
}

# One caption stream, declared as the profile the demuxer selects the caption
# decoder by.  No PCR stream is announced; the packet timestamps are enough.
function write_pmt(   len) {
    len = 1 + 27
    packet(ppid, 1, len)
    crc_on = 0; raw8(0)
    crc_on = 1; crc = 4294967295
    put8(2); put8(176); put8(24)
    put16(program); put8(193); put8(0); put8(0)
    put16(bor(57344, 8191))                            # no PCR stream
    put16(bor(61440, 6))                               # 6 bytes of program info
    put8(5); put8(4)                                   # registration descriptor
    put8(72); put8(68); put8(77); put8(86)             # the profile's tag bytes
    put8(144)                                          # this caption stream type
    put16(bor(61440, spid))
    put16(61440)                                       # no stream descriptors
    crc_on = 0; put_crc()
}

# ------------------------------------------------------------ packetised level
# Open one packetised elementary stream of 14 + dslen bytes: the 14-byte header
# then dslen bytes of display set.  The packet count and the size of the last
# packet follow from the total, so they are settled here, before any byte of it
# has been written.
function pes_open(pts_ms, dslen,   total) {
    total     = 14 + dslen
    pes_len   = 8 + dslen                              # counted by the field
    pes_pts   = pts_ms * 90                            # 90 kHz, as on the wire
    pk_n      = int((total + 183) / 184)
    pk_tail   = total - 184 * (pk_n - 1)
    pk_i      = 0
    pk_space  = 0                                      # forces the first open
}

# One byte of the packetised elementary stream, opening packets as they fill.
function pw(v,   b) {
    b = v % 256
    if (pk_space <= 0) {
        pk_space = (pk_i == pk_n - 1) ? pk_tail : 184
        packet(spid, pk_i == 0, pk_space)              # only the first one starts
        pk_i++
    }
    raw8(b)
    pk_space--
}

# The header: one presentation timestamp and nothing else.  The timestamp is a
# 33-bit value spread over five bytes with marker bits in the even positions, so
# the fields are assembled here one bit range at a time.  (The in-tree generator
# for this format packs the third byte from bit 14 rather than bit 15, which
# cannot show up in its own stream because that stream only ever carries pts zero;
# the schedule here does not, so the fields are taken from the field layout.)
function put_pes_header() {
    pw(0); pw(0); pw(1); pw(189)
    pw(int(pes_len / 256)); pw(pes_len)
    pw(128); pw(128); pw(5)                            # timestamp only, 5 bytes
    pw(33 + int(int(pes_pts / 1073741824) % 8) * 2)     # '0010' pts[32:30] '1'
    pw(int(pes_pts / 4194304) % 256)                    # pts[29:22]
    pw((int(pes_pts / 32768) % 128) * 2 + 1)            # pts[21:15] '1'
    pw(int(pes_pts / 128) % 256)                        # pts[14:7]
    pw((pes_pts % 128) * 2 + 1)                         # pts[6:0] '1'
}

# ---------------------------------------------------------------- display sets
# The caption colours.  Index 0 is transparent and is what the empty space around
# a caption bitmap is painted with.  Entries are (index, luma, red-difference,
# blue-difference, alpha).
function put_palette() {
    pw(20); pw(0); pw(27)                              # palette segment, 27 bytes
    pw(0); pw(0)                                       # id, version
    pw(0); pw(0);   pw(0);   pw(0);   pw(0)            # 0: transparent
    pw(1); pw(235); pw(128); pw(128); pw(255)          # 1: near white
    pw(2); pw(80);  pw(90);  pw(240); pw(255)          # 2: blue
    pw(3); pw(150); pw(240); pw(60);  pw(255)          # 3: red
    pw(4); pw(60);  pw(60);  pw(90);  pw(255)          # 4: green
}

# The presentation segment: the canvas size, this set's composition, and the one
# object it shows - or no object at all, which is how this format clears the
# screen.  The state byte is 192, "epoch start", which makes each set stand on its
# own: the palette and the object it needs are the ones it carries.
function put_presentation() {
    pw(22); pw(0); pw(11 + 8 * nobj)                   # presentation segment
    pw(int(vw / 256)); pw(vw % 256)
    pw(int(vh / 256)); pw(vh % 256)
    pw(32)                                             # frame rate code
    pw(int(comp / 256)); pw(comp % 256)                # composition number
    pw(192)                                            # epoch start
    pw(0); pw(0); pw(nobj)                             # no palette update
    if (nobj) {
        pw(int(oid / 256)); pw(oid % 256)              # the object shown
        pw(0)                                          # in window 0
        pw(0)                                          # plain composition
        pw(int(ox / 256)); pw(ox % 256)
        pw(int(oy / 256)); pw(oy % 256)
    }
}

# The one window: the whole canvas, so nothing is cropped.
function put_window() {
    pw(23); pw(0); pw(10)                              # window segment, 10 bytes
    pw(0); pw(0)                                       # id, version
    pw(0); pw(0)                                       # x
    pw(0); pw(0)                                       # y
    pw(int(vw / 256)); pw(vw % 256)
    pw(int(vh / 256)); pw(vh % 256)
}

# The object: ow x oh solid colour, one run and one line end per row.  A solid
# run is the cheapest thing this bitmap encoding can say - four bytes for the
# run, two for the end of the row.
function put_object(rle,   i) {
    pw(21); pw(int((11 + rle) / 256)); pw((11 + rle) % 256)
    pw(int(oid / 256)); pw(oid % 256)
    pw(0)                                              # version
    pw(192)                                            # the whole object, one piece
    pw24(rle + 4)                                      # coded size incl. the size
    pw(int(ow / 256)); pw(ow % 256)
    pw(int(oh / 256)); pw(oh % 256)
    for (i = 0; i < oh; i++) {
        pw(0); pw(192 + int(ow / 256)); pw(ow % 256); pw(ocol)
        pw(0); pw(0)
    }
}

function pw24(v) { pw(int(v / 65536)); pw(int(v / 256)); pw(v % 256) }

# The display-end segment, all timeouts zero: hold this composition until the
# next display set arrives, which is the only way this format ends a caption.
function put_end() {
    pw(128); pw(0); pw(4)
    pw(0); pw(0); pw(0); pw(0)
}

# -------------------------------------------------------------- schedule rows
function emit_set(tms,   rle, dslen) {
    rle   = nobj ? 6 * oh : 0
    dslen = (3 + 11 + 8 * nobj) + (3 + 27) + (3 + 10) + (3 + 4)
    if (nobj)
        dslen = dslen + 3 + 11 + rle
    pes_open(tms, dslen)
    put_pes_header()
    put_presentation()
    put_palette()
    put_window()
    if (nobj)
        put_object(rle)
    put_end()
}

# A clear is a display set with no object at all: the hand this stream format
# deals with, and the one that retires a caption in the shipping command. The
# consumer is handed a subtitle carrying no rectangles, and the transcoder paints
# the blank canvas it builds from that - see sub2video_update().
$1 == "C" {
    if (NF < 2) { print "malformed clear row: " $0 > "/dev/stderr"; nbad++; next }
    if ($2 + 0 <= last_ms) {
        print "rows must arrive in increasing time: " $0 > "/dev/stderr"; nbad++; next
    }
    last_ms = $2 + 0
    comp = NR; nobj = 0
    nsets++
    emit_set(last_ms)
    next
}

$1 == "D" {
    if (NF != 8) { print "malformed caption row: " $0 > "/dev/stderr"; nbad++; next }
    if ($2 + 0 <= last_ms) {
        print "rows must arrive in increasing time: " $0 > "/dev/stderr"; nbad++; next
    }
    last_ms = $2 + 0
    nobj = 1
    oid = $3; ox = $4; oy = $5; ow = $6; oh = $7; ocol = $8
    if (ow < 64 || ox < 0 || oy < 0 || ow + ox > vw || oh + oy > vh) {
        print "object does not fit the declared size: " $0 > "/dev/stderr"
        nbad++
        next
    }
    comp = NR
    nsets++
    emit_set(last_ms)
    next
}

$1 == "" || $1 ~ /^#/ { next }
{ print "unknown row type: " $0 > "/dev/stderr"; nbad++ }

END {
    if (nbad)
        exit 1
    if (bytes % 188) {
        printf "wrote %d bytes, which is not a whole number of 188-byte packets\n", \
               bytes > "/dev/stderr"
        exit 1
    }
    printf "%d display sets, %d bytes, %d packets\n", nsets, bytes, bytes / 188 \
           > "/dev/stderr"
}
