/*
 * H.26L/H.264/AVC/JVT/14496-10/... SEI decoding
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 *
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

/**
 * @file
 * H.264 / AVC / MPEG-4 part10 SEI decoding.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "bytestream.h"
#include "get_bits.h"
#include "golomb.h"
#include "h264_ps.h"
#include "h264_sei.h"
#include "sei.h"

#define AVERROR_PS_NOT_FOUND      FFERRTAG(0xF8,'?','P','S')

static const uint8_t sei_num_clock_ts_table[9] = {
    1, 1, 1, 2, 2, 3, 3, 2, 3
};

void ff_h264_sei_uninit(H264SEIContext *h)
{
    h->recovery_point.recovery_frame_cnt = -1;

    h->picture_timing.dpb_output_delay  = 0;
    h->picture_timing.cpb_removal_delay = -1;

    h->picture_timing.present      = 0;
    h->buffering_period.present    = 0;
    h->common.frame_packing.present       = 0;
    h->common.display_orientation.present = 0;

    ff_h2645_sei_reset(&h->common);
}

int ff_h264_sei_process_picture_timing(H264SEIPictureTiming *h, const SPS *sps,
                                       void *logctx)
{
    GetBitContext gb;
    av_unused int ret;

    ret = init_get_bits8(&gb, h->payload, h->payload_size_bytes);
    av_assert1(ret >= 0);

    if (sps->nal_hrd_parameters_present_flag ||
        sps->vcl_hrd_parameters_present_flag) {
        h->cpb_removal_delay = get_bits_long(&gb, sps->cpb_removal_delay_length);
        h->dpb_output_delay  = get_bits_long(&gb, sps->dpb_output_delay_length);
    }
    if (sps->pic_struct_present_flag) {
        unsigned int i, num_clock_ts;

        h->pic_struct = get_bits(&gb, 4);
        h->ct_type    = 0;

        if (h->pic_struct > H264_SEI_PIC_STRUCT_FRAME_TRIPLING)
            return AVERROR_INVALIDDATA;

        num_clock_ts = sei_num_clock_ts_table[h->pic_struct];
        h->timecode_cnt = 0;
        for (i = 0; i < num_clock_ts; i++) {
            if (get_bits(&gb, 1)) {                      /* clock_timestamp_flag */
                H264SEITimeCode *tc = &h->timecode[h->timecode_cnt++];
                unsigned int full_timestamp_flag;
                unsigned int counting_type, cnt_dropped_flag;
                h->ct_type |= 1 << get_bits(&gb, 2);
                skip_bits(&gb, 1);                       /* nuit_field_based_flag */
                counting_type = get_bits(&gb, 5);        /* counting_type */
                full_timestamp_flag = get_bits(&gb, 1);
                skip_bits(&gb, 1);                       /* discontinuity_flag */
                cnt_dropped_flag = get_bits(&gb, 1);      /* cnt_dropped_flag */
                if (cnt_dropped_flag && counting_type > 1 && counting_type < 7)
                    tc->dropframe = 1;
                tc->frame = get_bits(&gb, 8);         /* n_frames */
                if (full_timestamp_flag) {
                    tc->full = 1;
                    tc->seconds = get_bits(&gb, 6); /* seconds_value 0..59 */
                    tc->minutes = get_bits(&gb, 6); /* minutes_value 0..59 */
                    tc->hours = get_bits(&gb, 5);   /* hours_value 0..23 */
                } else {
                    tc->seconds = tc->minutes = tc->hours = tc->full = 0;
                    if (get_bits(&gb, 1)) {             /* seconds_flag */
                        tc->seconds = get_bits(&gb, 6);
                        if (get_bits(&gb, 1)) {         /* minutes_flag */
                            tc->minutes = get_bits(&gb, 6);
                            if (get_bits(&gb, 1))       /* hours_flag */
                                tc->hours = get_bits(&gb, 5);
                        }
                    }
                }

                if (sps->time_offset_length > 0)
                    skip_bits(&gb,
                              sps->time_offset_length); /* time_offset */
            }
        }

        av_log(logctx, AV_LOG_DEBUG, "ct_type:%X pic_struct:%d\n",
               h->ct_type, h->pic_struct);
    }

    return 0;
}

static int decode_picture_timing(H264SEIPictureTiming *h, GetByteContext *gb,
                                 void *logctx)
{
    int size = bytestream2_get_bytes_left(gb);

    if (size > sizeof(h->payload)) {
        av_log(logctx, AV_LOG_ERROR, "Picture timing SEI payload too large\n");
        return AVERROR_INVALIDDATA;
    }
    bytestream2_get_bufferu(gb, h->payload, size);

    h->payload_size_bytes = size;

    h->present = 1;
    return 0;
}

static int decode_recovery_point(H264SEIRecoveryPoint *h, GetBitContext *gb, void *logctx)
{
    unsigned recovery_frame_cnt = get_ue_golomb_long(gb);

    if (recovery_frame_cnt >= (1<<MAX_LOG2_MAX_FRAME_NUM)) {
        av_log(logctx, AV_LOG_ERROR, "recovery_frame_cnt %u is out of range\n", recovery_frame_cnt);
        return AVERROR_INVALIDDATA;
    }

    h->recovery_frame_cnt = recovery_frame_cnt;
    /* 1b exact_match_flag,
     * 1b broken_link_flag,
     * 2b changing_slice_group_idc */
    skip_bits(gb, 4);

    return 0;
}

static int decode_buffering_period(H264SEIBufferingPeriod *h, GetBitContext *gb,
                                   const H264ParamSets *ps, void *logctx)
{
    unsigned int sps_id;
    int sched_sel_idx;
    const SPS *sps;

    sps_id = get_ue_golomb_31(gb);
    if (sps_id > 31 || !ps->sps_list[sps_id]) {
        av_log(logctx, AV_LOG_ERROR,
               "non-existing SPS %d referenced in buffering period\n", sps_id);
        return sps_id > 31 ? AVERROR_INVALIDDATA : AVERROR_PS_NOT_FOUND;
    }
    sps = ps->sps_list[sps_id];

    // NOTE: This is really so duplicated in the standard... See H.264, D.1.1
    if (sps->nal_hrd_parameters_present_flag) {
        for (sched_sel_idx = 0; sched_sel_idx < sps->cpb_cnt; sched_sel_idx++) {
            h->initial_cpb_removal_delay[sched_sel_idx] =
                get_bits_long(gb, sps->initial_cpb_removal_delay_length);
            // initial_cpb_removal_delay_offset
            skip_bits(gb, sps->initial_cpb_removal_delay_length);
        }
    }
    if (sps->vcl_hrd_parameters_present_flag) {
        for (sched_sel_idx = 0; sched_sel_idx < sps->cpb_cnt; sched_sel_idx++) {
            h->initial_cpb_removal_delay[sched_sel_idx] =
                get_bits_long(gb, sps->initial_cpb_removal_delay_length);
            // initial_cpb_removal_delay_offset
            skip_bits(gb, sps->initial_cpb_removal_delay_length);
        }
    }

    h->present = 1;
    return 0;
}

static int decode_green_metadata(H264SEIGreenMetaData *h, GetByteContext *gb)
{
    h->green_metadata_type = bytestream2_get_byte(gb);

    if (h->green_metadata_type == 0) {
        h->period_type = bytestream2_get_byte(gb);

        if (h->period_type == 2)
            h->num_seconds = bytestream2_get_be16(gb);
        else if (h->period_type == 3)
            h->num_pictures = bytestream2_get_be16(gb);

        h->percent_non_zero_macroblocks            = bytestream2_get_byte(gb);
        h->percent_intra_coded_macroblocks         = bytestream2_get_byte(gb);
        h->percent_six_tap_filtering               = bytestream2_get_byte(gb);
        h->percent_alpha_point_deblocking_instance = bytestream2_get_byte(gb);

    } else if (h->green_metadata_type == 1) {
        h->xsd_metric_type  = bytestream2_get_byte(gb);
        h->xsd_metric_value = bytestream2_get_be16(gb);
    }

    return 0;
}

/* ------------------------------------------------------------------------
 * BD3D subtitle-depth offset metadata (the "OFMD" user data unregistered SEI)
 *
 * The BD3D OFMD format per public implementations (LAVFilters msdk_mvc,
 * MPC-BE MSDKDecoder ParseOffsetMetadata, Kodi BlurayOffsetMetadata), which
 * are the de-facto reference for this message: no normative text for its body
 * is freely available.  The field layout, the sign convention and the
 * picture-rate table below are ported from those parsers; the discovery on
 * which this task is based is recorded in the project discovery notes.
 *
 * Transport: the dependent view of a multiview stream carries it once per
 * group of pictures, as a user_data_unregistered (payload type 5) message with
 * the UUID below and the four-byte tag "OFMD", usually - but not always -
 * wrapped in a scalable-nesting (payload type 37) message.  This parser does
 * not walk that nesting: the operation-point header of the wrapper comes in a
 * one-byte and in a two-byte form in the wild, and the message is also seen
 * unwrapped, so the payload of the SEI NAL unit is scanned for the UUID and
 * tag instead (the payload arrives with its emulation prevention bytes
 * already removed, by the NAL unit splitter).
 *
 * Body, immediately after the tag (a 10-byte header, then the table):
 *   0     marker_bit, reserved, frame_rate_code (low nibble, see below)
 *   1-5   reserved, then the 36 bit 90 kHz presentation timestamp of the
 *         described group in 3 + 15 + 15 bits with the marker bits between
 *   6     two marker bits, then sequence_count (6 bits, 1..32)
 *   7     frame_count (8 bits): the pictures of the described group
 *   8-9   opaque to every implementation that reads this message
 *   10..  sequence_count * frame_count bytes: the offset table, sequence-major
 *         and in display order inside a sequence, one byte per picture - bit7
 *         direction_flag (set = behind the screen), bits0..6 magnitude in
 *         native pixels.  0x80 therefore means flat.
 * ------------------------------------------------------------------------ */

static const uint8_t ofmd_uuid[16] = {
    0x17, 0xee, 0x8c, 0x60, 0xf8, 0x4d, 0x11, 0xd9,
    0x8c, 0xd6, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};
static const uint8_t ofmd_tag[4] = { 'O', 'F', 'M', 'D' };

/* frame_rate_code to picture rate, the table the public parsers key the field
 * with (1 = 24000/1001, 2 = 24, 3 = 25, 4 = 30000/1001, 5 = 30, 6 = 50,
 * 7 = 60000/1001), completed over the whole 4-bit field.  A zero numerator
 * means the code carries no rate. */
static const AVRational ofmd_frame_rates[16] = {
    {    0, 1 }, /*  0: no rate */
    {24000, 1001}, {   24, 1 }, {   25, 1 }, {30000, 1001},
    {   30, 1 }, {   50, 1 }, {60000, 1001}, {   60, 1 },
    {  100, 1 }, {  120, 1 }, {  200, 1 }, {  240, 1 }, {  300, 1 },
    {    0, 1 }, /* 14: reserved */
    {    0, 1 }, /* 15: reserved */
};

/* One message, with msg pointing at its UUID. The block is assembled locally
 * and published only when the whole table is there, so a malformed message
 * cannot leave a half-updated block behind. */
static int ofmd_read_message(H264OFMD *dst, const uint8_t *msg, size_t avail,
                             void *logctx)
{
    const uint8_t *hdr = msg + 20;  /* the UUID (16) and the tag (4) precede it */
    H264OFMD parsed;
    GetBitContext gb;
    unsigned sequence_count, frame_count;
    uint64_t pts;
    int hi, marker1, marker2, mid, lo, ret;

    if (avail < 30) {
        av_log(logctx, AV_LOG_WARNING,
               "Truncated subtitle depth metadata (%zu of 30 bytes)\n", avail);
        return AVERROR_INVALIDDATA;
    }

    /* the 36 bit timestamp of the group, in 3 + 15 + 15 bits with the marker
     * bits between, after five reserved bits */
    ret = init_get_bits8(&gb, hdr + 1, 5);
    if (ret < 0)
        return ret;
    skip_bits(&gb, 5);
    hi      = get_bits(&gb, 3);
    marker1 = get_bits1(&gb);
    mid     = get_bits(&gb, 15);
    marker2 = get_bits1(&gb);
    lo      = get_bits(&gb, 15);
    if (!marker1 || !marker2)
        av_log(logctx, AV_LOG_DEBUG,
               "Subtitle depth metadata without marker bits in its timestamp\n");
    pts = ((uint64_t)hi << 30) | ((uint64_t)mid << 15) | (uint64_t)lo;

    sequence_count = hdr[6] & 0x3F;
    frame_count    = hdr[7];
    /* hdr[8] and hdr[9] are opaque: every implementation that reads this
     * message skips them */

    if (sequence_count == 0 || sequence_count > H264_OFMD_MAX_SEQUENCES ||
        frame_count == 0 || frame_count > H264_OFMD_MAX_FRAMES) {
        av_log(logctx, AV_LOG_WARNING,
               "Unsupported subtitle depth metadata shape "
               "(%u sequences x %u frames)\n", sequence_count, frame_count);
        return AVERROR_INVALIDDATA;
    }
    if (10 + (size_t)sequence_count * frame_count > avail) {
        av_log(logctx, AV_LOG_WARNING,
               "Truncated subtitle depth metadata table "
               "(%u x %u entries in %zu bytes)\n",
               sequence_count, frame_count, avail - 10);
        return AVERROR_INVALIDDATA;
    }

    parsed = (H264OFMD) {
        .present       = 1,
        .pts90k        = (int64_t)pts,
        .fps           = ofmd_frame_rates[hdr[0] & 0x0F],
        .sequence_count = sequence_count,
        .frame_count   = frame_count,
    };
    if (!parsed.fps.num)
        av_log(logctx, AV_LOG_WARNING,
               "Subtitle depth metadata with an unusable picture rate "
               "(code %u); ignoring it\n", hdr[0] & 0x0F);
    else
        memcpy(parsed.table, hdr + 10, (size_t)sequence_count * frame_count);

    if (!parsed.fps.num)
        return AVERROR_INVALIDDATA;

    *dst = parsed;

    av_log(logctx, AV_LOG_DEBUG,
           "Subtitle depth metadata: %u sequences x %u frames at pts %lld\n",
           sequence_count, frame_count, (long long)pts);

    return 0;
}

int ff_h264_ofmd_scan(H264OFMD *ofmd, const uint8_t *payload, size_t size,
                      void *logctx)
{
    size_t i;
    int found = 0;

    for (i = 0; i + 30 <= size; i++) {
        if (memcmp(payload + i, ofmd_uuid, sizeof(ofmd_uuid)) ||
            memcmp(payload + i + 16, ofmd_tag, sizeof(ofmd_tag)))
            continue;
        found++;
        /* the last message of a payload wins, as it does everywhere else:
         * one message describes the group that is about to be decoded */
        ofmd_read_message(ofmd, payload + i, size - i, logctx);
    }

    return found;
}

int ff_h264_ofmd_lookup(const H264OFMD *ofmd, int64_t pts90k, int8_t *offsets)
{
    int64_t gop_units, delta, index;
    unsigned seq, frame;

    if (!ofmd->present || !ofmd->fps.num || pts90k == AV_NOPTS_VALUE)
        return 0;

    /* The block describes the group [start, start + duration). A picture
     * outside it is not covered, and the caller then reports no depth at all
     * rather than an invented one. */
    gop_units = av_rescale_rnd((int64_t)90000 * ofmd->frame_count,
                               ofmd->fps.den, ofmd->fps.num, AV_ROUND_NEAR_INF);
    if (pts90k < ofmd->pts90k || pts90k - ofmd->pts90k >= gop_units)
        return 0;

    /* Which picture of the group this display time names, rounded to nearest:
     * container timestamps are integers while a picture rate such as
     * 24000/1001 is not, so the arithmetic is never exact. The rounding is
     * well inside half a picture interval for any authored rate. */
    delta = pts90k - ofmd->pts90k;
    index = av_rescale_rnd(delta, ofmd->fps.num, (int64_t)90000 * ofmd->fps.den,
                           AV_ROUND_NEAR_INF);
    index = FFMIN(FFMAX(index, 0), (int64_t)ofmd->frame_count - 1);
    frame = (unsigned)index;

    for (seq = 0; seq < ofmd->sequence_count; seq++) {
        uint8_t entry = ofmd->table[seq * ofmd->frame_count + frame];
        int value     = entry & 0x7F;

        offsets[seq] = (int8_t)((entry & 0x80) ? -value : value);
    }

    return ofmd->sequence_count;
}

int ff_h264_sei_decode(H264SEIContext *h, GetBitContext *gb,
                       const H264ParamSets *ps, H264OFMD *ofmd, void *logctx)
{
    GetByteContext gbyte;
    int master_ret = 0;

    av_assert1((get_bits_count(gb) % 8) == 0);
    bytestream2_init(&gbyte, gb->buffer + get_bits_count(gb) / 8,
                     get_bits_left(gb) / 8);

    /* A decoder that outputs pictures of a multiview stream collects the
     * subtitle-depth metadata of this access unit; everyone else (the parser,
     * a single-view decode) passes NULL and pays nothing for the look. The
     * scan runs over the whole payload rather than over the messages the walk
     * below recognises, because the message travels wrapped in a nesting whose
     * header form varies - and wrapped in a type this walk does not decode. */
    if (ofmd)
        ff_h264_ofmd_scan(ofmd, gbyte.buffer, bytestream2_get_bytes_left(&gbyte),
                          logctx);

    while (bytestream2_get_bytes_left(&gbyte) > 2 && bytestream2_peek_ne16(&gbyte)) {
        GetByteContext gbyte_payload;
        GetBitContext gb_payload;
        int type = 0;
        unsigned size = 0;
        int ret  = 0;

        do {
            if (bytestream2_get_bytes_left(&gbyte) <= 0)
                return AVERROR_INVALIDDATA;
            type += bytestream2_peek_byteu(&gbyte);
        } while (bytestream2_get_byteu(&gbyte) == 255);

        do {
            if (bytestream2_get_bytes_left(&gbyte) <= 0)
                return AVERROR_INVALIDDATA;
            size += bytestream2_peek_byteu(&gbyte);
        } while (bytestream2_get_byteu(&gbyte) == 255);

        if (size > bytestream2_get_bytes_left(&gbyte)) {
            av_log(logctx, AV_LOG_ERROR, "SEI type %d size %d truncated at %d\n",
                   type, size, bytestream2_get_bytes_left(&gbyte));
            return AVERROR_INVALIDDATA;
        }

        bytestream2_init (&gbyte_payload, gbyte.buffer, size);
        ret = init_get_bits8(&gb_payload, gbyte.buffer, size);
        if (ret < 0)
            return ret;

        switch (type) {
        case SEI_TYPE_PIC_TIMING: // Picture timing SEI
            ret = decode_picture_timing(&h->picture_timing, &gbyte_payload, logctx);
            break;
        case SEI_TYPE_RECOVERY_POINT:
            ret = decode_recovery_point(&h->recovery_point, &gb_payload, logctx);
            break;
        case SEI_TYPE_BUFFERING_PERIOD:
            ret = decode_buffering_period(&h->buffering_period, &gb_payload, ps, logctx);
            break;
        case SEI_TYPE_GREEN_METADATA:
            ret = decode_green_metadata(&h->green_metadata, &gbyte_payload);
            break;
        case SEI_TYPE_MVC_SCALABLE_NESTING:
            /* Authored multiview streams wrap the subtitle-depth metadata (and
             * other messages) in this payload type. Its content is read by the
             * payload scan above; here it only has to be claimed, so that a
             * wrapper is not reported as an unknown message. */
            break;
        default:
            ret = ff_h2645_sei_message_decode(&h->common, type, AV_CODEC_ID_H264,
                                              &gb_payload, &gbyte_payload, logctx);
            if (ret == FF_H2645_SEI_MESSAGE_UNHANDLED)
                av_log(logctx, AV_LOG_DEBUG, "unknown SEI type %d\n", type);
        }
        if (ret < 0 && ret != AVERROR_PS_NOT_FOUND)
            return ret;
        if (ret < 0)
            master_ret = ret;

        if (get_bits_left(&gb_payload) < 0) {
            av_log(logctx, AV_LOG_WARNING, "SEI type %d overread by %d bits\n",
                   type, -get_bits_left(&gb_payload));
        }

        bytestream2_skipu(&gbyte, size);
    }

    return master_ret;
}

const char *ff_h264_sei_stereo_mode(const H2645SEIFramePacking *h)
{
    if (h->arrangement_cancel_flag == 0) {
        switch (h->arrangement_type) {
            case SEI_FPA_H264_TYPE_CHECKERBOARD:
                if (h->content_interpretation_type == 2)
                    return "checkerboard_rl";
                else
                    return "checkerboard_lr";
            case SEI_FPA_H264_TYPE_INTERLEAVE_COLUMN:
                if (h->content_interpretation_type == 2)
                    return "col_interleaved_rl";
                else
                    return "col_interleaved_lr";
            case SEI_FPA_H264_TYPE_INTERLEAVE_ROW:
                if (h->content_interpretation_type == 2)
                    return "row_interleaved_rl";
                else
                    return "row_interleaved_lr";
            case SEI_FPA_TYPE_SIDE_BY_SIDE:
                if (h->content_interpretation_type == 2)
                    return "right_left";
                else
                    return "left_right";
            case SEI_FPA_TYPE_TOP_BOTTOM:
                if (h->content_interpretation_type == 2)
                    return "bottom_top";
                else
                    return "top_bottom";
            case SEI_FPA_TYPE_INTERLEAVE_TEMPORAL:
                if (h->content_interpretation_type == 2)
                    return "block_rl";
                else
                    return "block_lr";
            case SEI_FPA_H264_TYPE_2D:
            default:
                return "mono";
        }
    } else if (h->arrangement_cancel_flag == 1) {
        return "mono";
    } else {
        return NULL;
    }
}
