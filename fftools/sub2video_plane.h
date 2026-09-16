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
 * Helpers shared by the subtitle-to-video transcode path and its unit test for
 * stamping AV_FRAME_DATA_MVC_SUB_PLANE onto subtitle-as-video frames: reading a
 * plane tag strictly and folding caption rectangles into a horizontal union.
 *
 * Kept self-contained (standard headers only) so that a small harness can
 * exercise the exact code the transcode uses without pulling in the transcoder.
 */

#ifndef FFTOOLS_SUB2VIDEO_PLANE_H
#define FFTOOLS_SUB2VIDEO_PLANE_H

#include <limits.h>
#include <stdlib.h>

/* Payload layout of AV_FRAME_DATA_MVC_SUB_PLANE (see libavutil/frame.h):
 * a fixed 8 bytes; the flags byte selects which of the rest is meaningful. */
#define FF_SUB_PLANE_DATA_SIZE   8
#define FF_SUB_PLANE_ID_NONE     0xFF
#define FF_SUB_PLANE_FLAG_PLANE  0x01
#define FF_SUB_PLANE_FLAG_BBOX   0x02

/* Highest plane a tag may name; matches the depth-sequence ceiling the OFMD
 * metadata carries and the plane_id byte's meaningful range. */
#define FF_SUB_PLANE_MAX         31

/**
 * Strictly read a subtitle plane tag value.
 *
 * Accepts optional surrounding blanks or tabs, one non-negative integer, and
 * nothing else.  Anything else (empty, a sign, letters, trailing junk, a value
 * outside 0..31, or a number too large to be a plane) is treated as absent
 * rather than guessed at.
 *
 * @return the plane in 0..FF_SUB_PLANE_MAX, or -1 when the value names none.
 */
static inline int ff_sub_plane_parse(const char *value)
{
    const char *p = value;
    char *end;
    long v;

    if (!value)
        return -1;
    while (*p == ' ' || *p == '\t')
        p++;
    /* a value must begin with a digit: this rejects an empty string, a sign
     * such as "-1", and any non-numeric tag before strtol is consulted */
    if (*p < '0' || *p > '9')
        return -1;
    v = strtol(p, &end, 10);
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end != '\0')          /* trailing junk: "3x", "3 3", "3," ... */
        return -1;
    if (v < 0 || v > FF_SUB_PLANE_MAX) /* out of range, or LONG_MAX on overflow */
        return -1;
    return (int)v;
}

/**
 * Horizontal union of caption rectangles, in subtitle-canvas pixel units.
 *
 * Add each rectangle that is actually painted (its left edge and its width);
 * read back the centre-x and the width of their union.  A caption epoch may
 * carry several rectangles which are merged onto one canvas, so their union -
 * not any single rectangle - is what a consumer must re-place.
 */
typedef struct FFSubBBox {
    int min_x;   /* leftmost edge added   */
    int max_x;   /* rightmost edge (exclusive) added */
    int nb;      /* rectangles added      */
} FFSubBBox;

static inline void ff_sub_bbox_reset(FFSubBBox *b)
{
    b->min_x = INT_MAX;
    b->max_x = INT_MIN;
    b->nb    = 0;
}

static inline void ff_sub_bbox_add(FFSubBBox *b, int x, int w)
{
    if (x < b->min_x)
        b->min_x = x;
    if (x + w > b->max_x)
        b->max_x = x + w;
    b->nb++;
}

/* centre-x of the union as an integer pixel coordinate (the floor of the
 * exact midpoint; undefined when the union is empty - check
 * ff_sub_bbox_valid first) */
static inline int ff_sub_bbox_center_x(const FFSubBBox *b)
{
    return (b->min_x + b->max_x) / 2;
}

/* width of the union, or 0 when empty */
static inline int ff_sub_bbox_width(const FFSubBBox *b)
{
    return b->nb ? b->max_x - b->min_x : 0;
}

/* whether any rectangle was added */
static inline int ff_sub_bbox_valid(const FFSubBBox *b)
{
    return b->nb > 0;
}

#endif /* FFTOOLS_SUB2VIDEO_PLANE_H */
