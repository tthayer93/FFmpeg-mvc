/*
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * The arithmetic behind the mvcsubdepth filter, kept apart from the filter so
 * that it can be read as a page of arithmetic.  Everything here is pure: it
 * turns the two frame markers a composed multiview stream hands a consumer,
 * plus the user's options, into the window each eye must read out of the
 * subtitle canvas.
 *
 * Self-contained on purpose (standard headers plus libavutil/defs only), so a
 * small harness can compile this exact code without the filter around it.
 */

#ifndef AVFILTER_MVCSUBDEPTH_H
#define AVFILTER_MVCSUBDEPTH_H

#include <limits.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Frame side data read here.  The layouts are documented with the enum values
 * in libavutil/frame.h; the constants spell out what a consumer must know.
 * ------------------------------------------------------------------------ */

/* AV_FRAME_DATA_MVC_SS_OFFSETS: [0]=sequence_count, [1]=flags, then
 * sequence_count signed bytes of offsets for this frame, one per depth
 * sequence.  Smallest legal payload is the two header bytes plus one entry. */
#define FF_MVC_SS_HDR_SIZE          2
#define FF_MVC_SS_SEQ_MIN           1
#define FF_MVC_SS_SEQ_MAX           32
#define FF_MVC_SS_FLAG_COVERED      0x01

/* AV_FRAME_DATA_MVC_SUB_PLANE: a fixed 8 bytes naming the depth sequence the
 * subtitle track belongs to and where its caption sits. */
#define FF_MVC_SUB_PLANE_SIZE       8
#define FF_MVC_SUB_PLANE_ID_NONE    0xFF
#define FF_MVC_SUB_PLANE_FLAG_PLANE 0x01
#define FF_MVC_SUB_PLANE_FLAG_BBOX  0x02

/* The eye a window is being cut for.  Input 0 of the filter is the composed
 * frame, whose left half is the left eye. */
#define FF_MVC_SUB_EYE_LEFT         0
#define FF_MVC_SUB_EYE_RIGHT        1

/**
 * Read one depth entry out of an AV_FRAME_DATA_MVC_SS_OFFSETS payload.
 *
 * @param data   the side data bytes (may be NULL for absent side data)
 * @param size   the side data size in bytes
 * @param plane  the depth sequence to look up
 * @return the authored offset in pixels (positive = toward the viewer), or 0
 *         when this payload does not answer that sequence - absent or
 *         malformed side data, a sequence beyond the table, or a frame the
 *         authored block does not cover.  An unknown depth is flat, and flat
 *         is what a consumer must render: never invent one.
 */
static inline int ff_mvc_sub_offset(const uint8_t *data, int size, int plane)
{
    int seq;

    if (!data || size < FF_MVC_SS_HDR_SIZE + FF_MVC_SS_SEQ_MIN || plane < 0)
        return 0;
    seq = data[0];
    if (seq < FF_MVC_SS_SEQ_MIN || seq > FF_MVC_SS_SEQ_MAX)
        return 0;
    if (size < FF_MVC_SS_HDR_SIZE + seq)
        return 0;
    if (!(data[1] & FF_MVC_SS_FLAG_COVERED))
        return 0;
    if (plane >= seq)
        return 0;
    return (int)(int8_t)data[FF_MVC_SS_HDR_SIZE + plane];
}

/**
 * Read the plane id out of an AV_FRAME_DATA_MVC_SUB_PLANE payload.
 *
 * @return the plane in 0..31, or -1 when the frame carries no marker, the
 *         payload is too short, or the marker does not name a plane.
 */
static inline int ff_mvc_sub_plane(const uint8_t *data, int size)
{
    if (!data || size < FF_MVC_SUB_PLANE_SIZE)
        return -1;
    if (!(data[1] & FF_MVC_SUB_PLANE_FLAG_PLANE))
        return -1;
    if (data[0] == FF_MVC_SUB_PLANE_ID_NONE || data[0] > 31)
        return -1;
    return data[0];
}

/**
 * Read the caption's centre-x, in subtitle-canvas pixels, out of an
 * AV_FRAME_DATA_MVC_SUB_PLANE payload.
 *
 * @return the centre-x, or INT_MIN when no bounding box is valid for this
 *         frame.  A frame whose caption fills the canvas horizontally, a frame
 *         with no caption at all and a frame carrying no marker all answer
 *         INT_MIN: in each of them the caption's own position is the only one
 *         there is, which is what the caller is then asked to keep.
 */
static inline int ff_mvc_sub_center_x(const uint8_t *data, int size)
{
    int16_t origin_x;

    if (!data || size < FF_MVC_SUB_PLANE_SIZE)
        return INT_MIN;
    if (!(data[1] & FF_MVC_SUB_PLANE_FLAG_BBOX))
        return INT_MIN;
    origin_x = (int16_t)((unsigned)data[2] | ((unsigned)data[3] << 8));
    return (int)origin_x;
}

/* ---------------------------------------------------------------------------
 * Geometry.  The functions work in the pixels of the padded subtitle canvas
 * the filter composites from: the subtitle frame scaled to one eye, with margin
 * columns of slack added on both sides so a shift has somewhere to go.  A
 * window is one eye's worth of columns read out of that canvas.
 *
 * The unshifted window starts where the subtitle canvas itself starts - the
 * margin - so the caption keeps the horizontal position that the
 * subtitle-to-video path painted for it.  Depth only walks the window away from
 * that start; it never recentres the caption.
 * ------------------------------------------------------------------------ */

/* A window start is a column of the padded canvas, and the canvas has room for
 * exactly eye_w columns plus the two margins: the whole travel a window can be
 * given is therefore the margin, and the far edge of the canvas is 2*margin. */
static inline int ff_mvc_sub_max_start(int margin)
{
    return 2 * margin;
}

static inline int ff_mvc_sub_clamp(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

/**
 * Horizontal offset of an authored depth for one eye.
 *
 * The signed depth is authored once per frame and applies to the pair: a
 * positive offset moves the caption toward the viewer.  In the padded canvas,
 * the eye's window therefore moves the opposite way from the caption: the left
 * eye's window moves left by @p offset, which makes the caption move right by
 * @p offset inside that eye, and the right eye's window moves right, moving
 * its caption left by the same amount.  That is what makes the two views
 * converge for a positive offset; a positive offset is defined to be toward
 * the viewer, so a caption authored in front of the screen ends up in front of
 * the screen - and one authored behind it behind.  Reading the sign the other
 * way round would put every caption at the mirrored depth, which is the single
 * most dangerous mistake this filter can make: it is therefore pinned by a
 * test, not by a comment.
 *
 * @param offset  signed authored (or requested) displacement, in video pixels
 * @param eye     FF_MVC_SUB_EYE_LEFT or FF_MVC_SUB_EYE_RIGHT
 */
static inline int ff_mvc_sub_eye_shift(int offset, int eye)
{
    return eye == FF_MVC_SUB_EYE_LEFT ? -offset : offset;
}

/**
 * The first canvas column one eye reads, clamped into the padded canvas.
 *
 * The unshifted window starts at @p margin, where the subtitle canvas starts.
 * Clamping is the graceful degradation at the extremes: a requested shift
 * larger than the slack added for it cannot be had, so the eye gets the
 * furthest window the canvas can serve and the pair stops being symmetric
 * about the centre.  That is a smaller depth error than losing the caption,
 * and it is visible in the log.
 *
 * @param offset   the signed depth for this frame
 * @param eye      FF_MVC_SUB_EYE_LEFT or FF_MVC_SUB_EYE_RIGHT
 * @param margin   the slack added on each side of the canvas
 */
static inline int ff_mvc_sub_window_start(int offset, int eye, int margin)
{
    int sum = margin + ff_mvc_sub_eye_shift(offset, eye);

    return ff_mvc_sub_clamp(sum, 0, ff_mvc_sub_max_start(margin));
}

/**
 * Map one horizontal coordinate from the subtitle frame to the canvas.
 *
 * The subtitle frame is scaled to one eye, so its pixels are a different
 * distance apart from the video's whenever the two sizes differ.  Rounded
 * division, and no negative coordinates expected or produced: a subtitle frame
 * is never smaller than one pixel wide.
 */
static inline int ff_mvc_sub_scale_x(int x, int src_w, int dst_w)
{
    if (src_w <= 0 || dst_w <= 0)
        return 0;
    if (x <= 0)
        return 0;
    if (x >= src_w)
        return dst_w;
    return (int)(((int64_t)x * dst_w + src_w / 2) / src_w);
}

/* ---------------------------------------------------------------------------
 * The filter's one option, parsed apart from the filter so that the grammar
 * can be tested as a page of arithmetic too.  The option is a string with
 * exactly these spellings, and the filter resolves the result once at init:
 * the frame path never looks at the spelling again, only at the mode.
 * ------------------------------------------------------------------------ */

/* The depth placement modes.  AUTO reads the sequence stamped on the subtitle
 * frame, FLAT ignores any authored depth, SHIFT replaces the authored depth
 * with a fixed displacement, and PLANE names the sequence to read instead of
 * trusting the stamp. */
#define FF_MVC_SUB_DEPTH_AUTO       0
#define FF_MVC_SUB_DEPTH_FLAT       1
#define FF_MVC_SUB_DEPTH_SHIFT      2
#define FF_MVC_SUB_DEPTH_PLANE      3

/* One whole decimal integer filling lo..hi: an optional sign where allowed,
 * at least one digit, and nothing else.  "7", "-7", "+7" are this shape;
 * " 7", "7x", "0x7", "" and anything overflowing the range are not.  No
 * partial consumption: the caller's whole remainder is this number or the
 * answer is no. */
static inline int ff_mvc_sub_parse_int(const char *s, int allow_sign,
                                       int lo, int hi, int *out)
{
    long long v = 0;
    int negative = 0;

    if (!s || !*s)
        return -1;
    if (*s == '-' || *s == '+') {
        if (!allow_sign)
            return -1;
        negative = (*s == '-');
        s++;
    }
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10 + (*s - '0');
        if (v > 4294967295LL) /* past any magnitude a signed int could return */
            return -1;
    }
    v = negative ? -v : v;
    if (v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

/**
 * Read the whole value of the filter's depth option.
 *
 * The accepted spellings are "auto" and its synonym "1" (place at the depth
 * authored for the sequence the subtitle track is marked with), "flat" and
 * its synonym "0" (place on the screen plane), "shift=<pixels>" (a signed
 * constant displacement in the authored sign convention, positive toward the
 * viewer) and "plane=<0..31>" (read that sequence's authored depth instead of
 * trusting the track's mark).  Matching is exact: no leading, trailing or
 * inner whitespace, no partially matching spelling, no number outside its
 * range.  Note that the option system splits an option from its value at the
 * first '=' only, so the plain form of a shift - "depth=shift=-8" - needs no
 * quoting of its own.
 *
 * @param value  the option's string, as stored by the option system
 * @param mode   set to one of FF_MVC_SUB_DEPTH_* on success
 * @param param  the pixels for SHIFT, the sequence for PLANE, 0 otherwise
 * @return 0 when the value spells one of them, -1 when it spells none
 */
static inline int ff_mvc_sub_depth_parse(const char *value, int *mode,
                                         int *param)
{
    static const char shift_kw[] = "shift=";
    static const char plane_kw[] = "plane=";
    int v;

    if (!value)
        return -1;
    if (!strcmp(value, "auto") || !strcmp(value, "1")) {
        *mode  = FF_MVC_SUB_DEPTH_AUTO;
        *param = 0;
        return 0;
    }
    if (!strcmp(value, "flat") || !strcmp(value, "0")) {
        *mode  = FF_MVC_SUB_DEPTH_FLAT;
        *param = 0;
        return 0;
    }
    if (!strncmp(value, shift_kw, sizeof(shift_kw) - 1)) {
        if (ff_mvc_sub_parse_int(value + sizeof(shift_kw) - 1, 1,
                                 INT_MIN, INT_MAX, &v) < 0)
            return -1;
        *mode  = FF_MVC_SUB_DEPTH_SHIFT;
        *param = v;
        return 0;
    }
    if (!strncmp(value, plane_kw, sizeof(plane_kw) - 1)) {
        if (ff_mvc_sub_parse_int(value + sizeof(plane_kw) - 1, 0,
                                 0, FF_MVC_SS_SEQ_MAX - 1, &v) < 0)
            return -1;
        *mode  = FF_MVC_SUB_DEPTH_PLANE;
        *param = v;
        return 0;
    }
    return -1;
}

#endif /* AVFILTER_MVCSUBDEPTH_H */
