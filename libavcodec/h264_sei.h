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

#ifndef AVCODEC_H264_SEI_H
#define AVCODEC_H264_SEI_H

#include "libavutil/frame.h"
#include "libavutil/rational.h"
#include "get_bits.h"
#include "h2645_sei.h"
#include "h264_ps.h"
#include "sei.h"


/**
 * pic_struct in picture timing SEI message
 */
typedef enum {
    H264_SEI_PIC_STRUCT_FRAME             = 0, ///<  0: %frame
    H264_SEI_PIC_STRUCT_TOP_FIELD         = 1, ///<  1: top field
    H264_SEI_PIC_STRUCT_BOTTOM_FIELD      = 2, ///<  2: bottom field
    H264_SEI_PIC_STRUCT_TOP_BOTTOM        = 3, ///<  3: top field, bottom field, in that order
    H264_SEI_PIC_STRUCT_BOTTOM_TOP        = 4, ///<  4: bottom field, top field, in that order
    H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP    = 5, ///<  5: top field, bottom field, top field repeated, in that order
    H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM = 6, ///<  6: bottom field, top field, bottom field repeated, in that order
    H264_SEI_PIC_STRUCT_FRAME_DOUBLING    = 7, ///<  7: %frame doubling
    H264_SEI_PIC_STRUCT_FRAME_TRIPLING    = 8  ///<  8: %frame tripling
} H264_SEI_PicStructType;

typedef struct H264SEITimeCode {
    /* When not continuously receiving full timecodes, we have to reference
       the previous timecode received */
    int full;
    int frame;
    int seconds;
    int minutes;
    int hours;
    int dropframe;
} H264SEITimeCode;

typedef struct H264SEIPictureTiming {
    // maximum size of pic_timing according to the spec should be 274 bits
    uint8_t payload[40];
    int     payload_size_bytes;

    int present;
    H264_SEI_PicStructType pic_struct;

    /**
     * Bit set of clock types for fields/frames in picture timing SEI message.
     * For each found ct_type, appropriate bit is set (e.g., bit 1 for
     * interlaced).
     */
    int ct_type;

    /**
     * dpb_output_delay in picture timing SEI message, see H.264 C.2.2
     */
    int dpb_output_delay;

    /**
     * cpb_removal_delay in picture timing SEI message, see H.264 C.1.2
     */
    int cpb_removal_delay;

    /**
     * Maximum three timecodes in a pic_timing SEI.
     */
    H264SEITimeCode timecode[3];

    /**
     * Number of timecode in use
     */
    int timecode_cnt;
} H264SEIPictureTiming;

typedef struct H264SEIRecoveryPoint {
    /**
     * recovery_frame_cnt
     *
     * Set to -1 if no recovery point SEI message found or to number of frames
     * before playback synchronizes. Frames having recovery point are key
     * frames.
     */
    int recovery_frame_cnt;
} H264SEIRecoveryPoint;

typedef struct H264SEIBufferingPeriod {
    int present;   ///< Buffering period SEI flag
    int initial_cpb_removal_delay[32];  ///< Initial timestamps for CPBs
} H264SEIBufferingPeriod;

typedef struct H264SEIGreenMetaData {
    uint8_t green_metadata_type;
    uint8_t period_type;
    uint16_t num_seconds;
    uint16_t num_pictures;
    uint8_t percent_non_zero_macroblocks;
    uint8_t percent_intra_coded_macroblocks;
    uint8_t percent_six_tap_filtering;
    uint8_t percent_alpha_point_deblocking_instance;
    uint8_t xsd_metric_type;
    uint16_t xsd_metric_value;
} H264SEIGreenMetaData;

typedef struct H264SEIContext {
    H2645SEI common;
    H264SEIPictureTiming picture_timing;
    H264SEIRecoveryPoint recovery_point;
    H264SEIBufferingPeriod buffering_period;
    H264SEIGreenMetaData green_metadata;
} H264SEIContext;

/**
 * BD3D subtitle-depth (offset-metadata) block, as carried by the OFMD
 * user-data SEI of the dependent view of a multiview stream.
 *
 * This is deliberately NOT part of H264SEIContext: the SEI context is reset
 * for every access unit (ff_h264_sei_uninit()), while one of these blocks
 * describes a whole group of pictures and has to outlive the unit that
 * carries it.  The block is decode-session state of the context that parsed
 * it - see the ofmd field of H264Context, its copy in
 * ff_h264_update_thread_context() and ff_h264_flush_change().
 *
 * The table is kept exactly as it arrives on the wire: sequence-major, in
 * display order inside a sequence, one byte per picture - bit7 the
 * direction_flag (set = behind the screen) and bits0..6 the magnitude in
 * native pixels, so 0x80 is the authored flat entry.
 *
 * The timestamp is NOT kept as it arrives: it names the group on the DISC's own
 * 90 kHz timeline, which is not the timeline the pictures of a demuxed file are
 * addressed by (a Matroska or MPEG-TS re-stamps from its own base, and a title
 * authored as several clips restarts that base per clip).  One number carries
 * the difference, base_shift90k, measured from the access unit the message
 * travelled in - see below.
 */
#define H264_OFMD_MAX_SEQUENCES 32   ///< sequence_count is a 6-bit field, <= 32 in practice
#define H264_OFMD_MAX_FRAMES    64   ///< GOP size guard; the authored GOPs seen so far are <= 40

/**
 * Largest base offset a block may be calibrated by, in 90 kHz units: 2 hours.
 *
 * The block's own timestamp and the container timestamp of the access unit that
 * carried it are two readings of ONE instant, so their difference is the stream's
 * base offset - the source's start time, of the order of tens of seconds for a
 * full image.  A shift beyond this bound is not a base offset at all: the anchor
 * does not belong to this block (a stream re-based in a way this decoder cannot
 * follow, or a block that has drifted away from its own group), and looking
 * pictures up through such a shift would answer with the depth of some unrelated
 * group.  Calibration is then refused and the block is used unshifted, which is
 * what it was before calibration existed: possibly no picture matches it, which
 * is a depth the consumer does not get, never a wrong one.
 */
#define H264_OFMD_MAX_BASE_SHIFT90K (2LL * 60 * 60 * 90000)

typedef struct H264OFMD {
    int        present;
    int64_t    pts90k;          ///< 90 kHz timestamp of the described GOP start,
                                ///<   as it arrives on the wire (the disc's time)
    /**
     * What to subtract from pts90k to get the start of the described group on
     * the timeline the pictures are addressed by: the block's disc timestamp
     * minus the container timestamp of the access unit in which the message was
     * parsed.  The SEI rides the start of the group it describes, so that access
     * unit's display time IS the group's start as the container sees it, and the
     * difference is constant across the group (reordering moves a picture's
     * output time, not its place in the group).  Zero when the block could not
     * be anchored: no container timestamp was known, or the measured offset was
     * absurd (see H264_OFMD_MAX_BASE_SHIFT90K).
     */
    int64_t    base_shift90k;
    /**
     * Whether the absurd-offset case above has already been reported through
     * this storage.  Sticky across blocks on purpose - the report is about the
     * session reading the blocks, not about one block - and cleared with the
     * rest of the state on a flush.
     */
    int        base_warned;
    AVRational fps;             ///< picture rate the block was authored at
    unsigned   sequence_count;  ///< 1 .. H264_OFMD_MAX_SEQUENCES
    unsigned   frame_count;     ///< entries per sequence (the GOP size)
    uint8_t    table[H264_OFMD_MAX_SEQUENCES * H264_OFMD_MAX_FRAMES];
} H264OFMD;

struct H264ParamSets;

int ff_h264_sei_decode(H264SEIContext *h, GetBitContext *gb,
                       const struct H264ParamSets *ps, H264OFMD *ofmd,
                       int64_t ofmd_anchor90k, void *logctx);

/**
 * Reset SEI values at the beginning of the frame.
 */
void ff_h264_sei_uninit(H264SEIContext *h);

/**
 * Get stereo_mode string from the h264 frame_packing_arrangement
 */
const char *ff_h264_sei_stereo_mode(const H2645SEIFramePacking *h);

/**
 * Parse the contents of a picture timing message given an active SPS.
 */
int ff_h264_sei_process_picture_timing(H264SEIPictureTiming *h, const SPS *sps,
                                       void *logctx);

/**
 * Look for a BD3D subtitle-depth (OFMD) message in the payload of one SEI NAL
 * unit and store it in *ofmd, replacing any block held there.
 *
 * The payload must have had its emulation prevention bytes removed already.
 * The message is searched for by its UUID and tag rather than by walking the
 * scalable-nesting header it is usually wrapped in, because both the
 * one-byte and the two-byte forms of that header occur on authored discs (and
 * the message also occurs unwrapped).
 *
 * @param anchor90k display time of the access unit this payload was parsed in,
 *                  in 90 kHz units on the timeline the pictures of the stream
 *                  are addressed by, or AV_NOPTS_VALUE when that is not known.
 *                  Every block found here is calibrated against it (see
 *                  H264OFMD.base_shift90k), which is why it is the caller's
 *                  access unit and not the block's own timestamp.
 *
 * @return the number of blocks found in this payload (0, 1 or 2+; more than
 *         one is malformed, the last one then wins) or a negative error code
 *         when the payload holds a message that cannot be read.
 */
int ff_h264_ofmd_scan(H264OFMD *ofmd, const uint8_t *payload, size_t size,
                      int64_t anchor90k, void *logctx);

/**
 * Resolve the subtitle-depth offsets one picture of the described group is
 * entitled to.
 *
 * @param pts90k display time of the picture in 90 kHz units, on the timeline of
 *               the stream being decoded: the block's range is compared here
 *               after moving it off the disc time it was stamped on, so this is
 *               the container's time and not the block's own
 * @param offsets caller-supplied array of H264_OFMD_MAX_SEQUENCES bytes;
 *                filled with one signed offset per offset sequence when this
 *                returns > 0 (positive = toward the viewer, negative = behind
 *                the screen, 0 = flat), left untouched otherwise
 *
 * @return the number of offset sequences written to offsets, i.e. the frame
 *         is covered by the block, or 0 when it is not (no block stored, no
 *         usable timestamp, or a display time outside the described group).
 */
int ff_h264_ofmd_lookup(const H264OFMD *ofmd, int64_t pts90k, int8_t *offsets);

#endif /* AVCODEC_H264_SEI_H */
