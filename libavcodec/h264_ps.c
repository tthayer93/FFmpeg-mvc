/*
 * H.26L/H.264/AVC/JVT/14496-10/... parameter set decoding
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
 * H.264 / AVC / MPEG-4 part10 parameter set decoding.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include <inttypes.h>

#include "libavutil/imgutils.h"
#include "mathops.h"
#include "avcodec.h"
#include "h264data.h"
#include "h2645_vui.h"
#include "h264_ps.h"
#include "golomb.h"
#include "libavutil/refstruct.h"

#define MIN_LOG2_MAX_FRAME_NUM    4

static const uint8_t default_scaling4[2][16] = {
    {  6, 13, 20, 28, 13, 20, 28, 32,
      20, 28, 32, 37, 28, 32, 37, 42 },
    { 10, 14, 20, 24, 14, 20, 24, 27,
      20, 24, 27, 30, 24, 27, 30, 34 }
};

static const uint8_t default_scaling8[2][64] = {
    {  6, 10, 13, 16, 18, 23, 25, 27,
      10, 11, 16, 18, 23, 25, 27, 29,
      13, 16, 18, 23, 25, 27, 29, 31,
      16, 18, 23, 25, 27, 29, 31, 33,
      18, 23, 25, 27, 29, 31, 33, 36,
      23, 25, 27, 29, 31, 33, 36, 38,
      25, 27, 29, 31, 33, 36, 38, 40,
      27, 29, 31, 33, 36, 38, 40, 42 },
    {  9, 13, 15, 17, 19, 21, 22, 24,
      13, 13, 17, 19, 21, 22, 24, 25,
      15, 17, 19, 21, 22, 24, 25, 27,
      17, 19, 21, 22, 24, 25, 27, 28,
      19, 21, 22, 24, 25, 27, 28, 30,
      21, 22, 24, 25, 27, 28, 30, 32,
      22, 24, 25, 27, 28, 30, 32, 33,
      24, 25, 27, 28, 30, 32, 33, 35 }
};

/* maximum number of MBs in the DPB for a given level */
static const int level_max_dpb_mbs[][2] = {
    { 10, 396       },
    { 11, 900       },
    { 12, 2376      },
    { 13, 2376      },
    { 20, 2376      },
    { 21, 4752      },
    { 22, 8100      },
    { 30, 8100      },
    { 31, 18000     },
    { 32, 20480     },
    { 40, 32768     },
    { 41, 32768     },
    { 42, 34816     },
    { 50, 110400    },
    { 51, 184320    },
    { 52, 184320    },
};

static void remove_pps(H264ParamSets *s, int id)
{
    av_refstruct_unref(&s->pps_list[id]);
}

static void remove_sps(H264ParamSets *s, int id)
{
#if 0
    int i;
    if (s->sps_list[id]) {
        /* drop all PPS that depend on this SPS */
        for (i = 0; i < FF_ARRAY_ELEMS(s->pps_list); i++)
            if (s->pps_list[i] && s->pps_list[i]->sps_id == id)
                remove_pps(s, i);
    }
#endif
    av_refstruct_unref(&s->sps_list[id]);
}

static inline int decode_hrd_parameters(GetBitContext *gb, void *logctx,
                                        SPS *sps)
{
    int cpb_count, i;
    cpb_count = get_ue_golomb_31(gb) + 1;

    if (cpb_count > 32U) {
        av_log(logctx, AV_LOG_ERROR, "cpb_count %d invalid\n", cpb_count);
        return AVERROR_INVALIDDATA;
    }

    sps->cpr_flag = 0x0;
    sps->bit_rate_scale = get_bits(gb, 4);
    get_bits(gb, 4); /* cpb_size_scale */
    for (i = 0; i < cpb_count; i++) {
        sps->bit_rate_value[i] = get_ue_golomb_long(gb) + 1; /* bit_rate_value_minus1 + 1 */
        sps->cpb_size_value[i] = get_ue_golomb_long(gb) + 1; /* cpb_size_value_minus1 + 1 */
        sps->cpr_flag         |= get_bits1(gb) << i;
    }
    sps->initial_cpb_removal_delay_length = get_bits(gb, 5) + 1;
    sps->cpb_removal_delay_length         = get_bits(gb, 5) + 1;
    sps->dpb_output_delay_length          = get_bits(gb, 5) + 1;
    sps->time_offset_length               = get_bits(gb, 5);
    sps->cpb_cnt                          = cpb_count;
    return 0;
}

static inline int decode_vui_parameters(GetBitContext *gb, void *logctx,
                                        SPS *sps)
{
    ff_h2645_decode_common_vui_params(gb, &sps->vui, logctx);

    if (show_bits1(gb) && get_bits_left(gb) < 10) {
        av_log(logctx, AV_LOG_WARNING, "Truncated VUI (%d)\n", get_bits_left(gb));
        return 0;
    }

    sps->timing_info_present_flag = get_bits1(gb);
    if (sps->timing_info_present_flag) {
        unsigned num_units_in_tick = get_bits_long(gb, 32);
        unsigned time_scale        = get_bits_long(gb, 32);
        if (!num_units_in_tick || !time_scale) {
            av_log(logctx, AV_LOG_ERROR,
                   "time_scale/num_units_in_tick invalid or unsupported (%u/%u)\n",
                   time_scale, num_units_in_tick);
            sps->timing_info_present_flag = 0;
        } else {
            sps->num_units_in_tick = num_units_in_tick;
            sps->time_scale = time_scale;
        }
        sps->fixed_frame_rate_flag = get_bits1(gb);
    }

    sps->nal_hrd_parameters_present_flag = get_bits1(gb);
    if (sps->nal_hrd_parameters_present_flag)
        if (decode_hrd_parameters(gb, logctx, sps) < 0)
            return AVERROR_INVALIDDATA;
    sps->vcl_hrd_parameters_present_flag = get_bits1(gb);
    if (sps->vcl_hrd_parameters_present_flag)
        if (decode_hrd_parameters(gb, logctx, sps) < 0)
            return AVERROR_INVALIDDATA;
    if (sps->nal_hrd_parameters_present_flag ||
        sps->vcl_hrd_parameters_present_flag)
        get_bits1(gb);     /* low_delay_hrd_flag */
    sps->pic_struct_present_flag = get_bits1(gb);
    if (!get_bits_left(gb))
        return 0;
    sps->bitstream_restriction_flag = get_bits1(gb);
    if (sps->bitstream_restriction_flag) {
        get_bits1(gb);     /* motion_vectors_over_pic_boundaries_flag */
        get_ue_golomb_31(gb); /* max_bytes_per_pic_denom */
        get_ue_golomb_31(gb); /* max_bits_per_mb_denom */
        get_ue_golomb_31(gb); /* log2_max_mv_length_horizontal */
        get_ue_golomb_31(gb); /* log2_max_mv_length_vertical */
        sps->num_reorder_frames = get_ue_golomb_31(gb);
        sps->max_dec_frame_buffering = get_ue_golomb_31(gb);

        if (get_bits_left(gb) < 0) {
            sps->num_reorder_frames         = 0;
            sps->bitstream_restriction_flag = 0;
        }

        if (sps->num_reorder_frames > 16U
            /* max_dec_frame_buffering || max_dec_frame_buffering > 16 */) {
            av_log(logctx, AV_LOG_ERROR,
                   "Clipping illegal num_reorder_frames %d\n",
                   sps->num_reorder_frames);
            sps->num_reorder_frames = 16;
            return AVERROR_INVALIDDATA;
        }
    }

    return 0;
}

static int mvc_sps_fail(void *logctx, const char *msg)
{
    av_log(logctx, AV_LOG_ERROR, "MVC SPS extension: %s\n", msg);
    return AVERROR_INVALIDDATA;
}

static int mvc_sps_fail_list(void *logctx, const char *list, const char *what)
{
    av_log(logctx, AV_LOG_ERROR, "MVC SPS extension: invalid %s %s\n", list, what);
    return AVERROR_INVALIDDATA;
}

static int skip_hrd_parameters(GetBitContext *gb)
{
    // Consume hrd_parameters (H.264 E.1.2) without storing it; the
    // multiview VUI per-op HRD sets are unused but must keep the
    // bitstream position exact.
    int cpb_count, i;
    int v = get_ue_golomb_31(gb);
    if (v < 0)
        return AVERROR_INVALIDDATA;
    cpb_count = v + 1;
    if ((unsigned)cpb_count > 32U)
        return AVERROR_INVALIDDATA;
    get_bits(gb, 8); /* bit_rate_scale + cpb_size_scale */
    for (i = 0; i < cpb_count; i++) {
        // Unbounded ue(v) fields (cpb_size_value / bit_rate_value): use
        // get_ue_golomb_long, since the table-based get_ue_golomb_31()
        // mis-reads codes with >= 9 leading zeros (length >= 19).
        get_ue_golomb_long(gb);
        get_ue_golomb_long(gb);
        get_bits1(gb); /* cbr_flag */
    }
    get_bits(gb, 20);
    return 0;
}

static int decode_mvc_sps_data(GetBitContext *gb, void *logctx, SPS *sps)
{
    H264MVCSPS *m = &sps->mvc;
    int i, j, k;

    // bit_equal_to_one
    if (get_bits1(gb) != 1)
        return mvc_sps_fail(logctx, "missing or corrupted");

    i = get_ue_golomb_31(gb); // num_views_minus1
    if (i < 0 || i >= H264_MAX_MVC_VIEWS)
        return mvc_sps_fail(logctx, "invalid num_views_minus1");
    m->num_views = i + 1;
    for (i = 0; i < m->num_views; i++) {
        int vid = get_ue_golomb_31(gb);
        if (vid < 0 || vid > 1023)
            return mvc_sps_fail(logctx, "invalid view_id");
        m->view_id[i] = vid;
    }

    for (i = 1; i < m->num_views; i++) {
        static const char *const list_names[4] = {
            "anchor_refs_l0", "anchor_refs_l1",
            "non_anchor_refs_l0", "non_anchor_refs_l1"
        };
        uint8_t *const nums[4] = {
            &m->num_anchor_refs_l0[i], &m->num_anchor_refs_l1[i],
            &m->num_non_anchor_refs_l0[i], &m->num_non_anchor_refs_l1[i]
        };
        uint8_t (*const ref_lists[4])[H264_MAX_MVC_VIEWS] = {
            &m->anchor_refs_l0[i], &m->anchor_refs_l1[i],
            &m->non_anchor_refs_l0[i], &m->non_anchor_refs_l1[i]
        };
        for (k = 0; k < 4; k++) {
            int n = get_ue_golomb_31(gb);
            // the lists hold indexes into the view_id array
            if (n < 0 || n >= m->num_views)
                return mvc_sps_fail_list(logctx, list_names[k], "count");
            nums[k][0] = n;
            for (j = 0; j < n; j++) {
                int r = get_ue_golomb_31(gb);
                if (r < 0 || r >= m->num_views)
                        return mvc_sps_fail_list(logctx, list_names[k], "view index");
                ref_lists[k][0][j] = r;
            }
        }
    }

    i = get_ue_golomb_31(gb); // num_level_values_signalled_minus1
    if (i < 0 || i >= H264_MAX_MVC_LEVELS)
        return mvc_sps_fail(logctx, "invalid num_level_values_signalled_minus1");
    m->num_levels = i + 1;
    for (i = 0; i < m->num_levels; i++) {
        m->level_idc[i] = get_bits(gb, 8);
        j = get_ue_golomb_31(gb); // num_applicable_ops_minus1
        if (j < 0 || j >= H264_MAX_MVC_OPS)
            return mvc_sps_fail(logctx, "invalid num_applicable_ops_minus1");
        m->num_ops[i] = j + 1;
        for (j = 0; j < m->num_ops[i]; j++) {
            H264MVCOp *op = &m->ops[i][j];
            op->temporal_id = get_bits(gb, 3);
            k = get_ue_golomb_31(gb); // num_target_views_minus1
            if (k < 0 || k >= m->num_views)
                return mvc_sps_fail(logctx, "invalid num_target_views_minus1");
            op->num_target_views = k + 1;
            for (k = 0; k < op->num_target_views; k++) {
                int tv = get_ue_golomb_31(gb);
                if (tv < 0 || tv >= m->num_views)
                    return mvc_sps_fail(logctx, "invalid applicable_op_target_view_id");
                op->target_view[k] = tv;
            }
            k = get_ue_golomb_31(gb); // op_num_views_minus1
            if (k < 0 || k >= H264_MAX_MVC_VIEWS)
                return mvc_sps_fail(logctx, "invalid op_num_views_minus1");
            op->num_views_in_op = k + 1;
        }
    }

    m->mvc_vui_present = get_bits1(gb);
    if (m->mvc_vui_present) {
        int nops = get_ue_golomb_31(gb); // vui_mvc_num_ops_minus1
        if (nops < 0 || nops >= H264_MAX_MVC_OPS)
            return mvc_sps_fail(logctx, "invalid vui_mvc_num_ops_minus1");
        m->vui_num_ops = nops + 1;
        for (i = 0; i < m->vui_num_ops; i++) {
            H264MVCVuiOp *vo = &m->vui_ops[i];
            vo->temporal_id = get_bits(gb, 3);
            j = get_ue_golomb_31(gb); // num_target_output_views_minus1
            if (j < 0 || j >= m->num_views)
                return mvc_sps_fail(logctx, "invalid num_target_output_views_minus1");
            vo->num_target_output_views = j + 1;
            for (k = 0; k < vo->num_target_output_views; k++) {
                int vid, t, found = 0;
                vid = get_ue_golomb_31(gb);
                // identifies an entry of the view_id array by value
                for (t = 0; vid >= 0 && t < m->num_views; t++)
                    if (m->view_id[t] == vid)
                        found = 1;
                if (vid < 0 || !found)
                    return mvc_sps_fail(logctx, "mvc VUI target output view not present in the stream");
                vo->target_output_view[k] = vid;
            }
            vo->timing_present = get_bits1(gb);
            if (vo->timing_present) {
                // get_bits_long (not get_bits) for 32-bit fields: get_bits()
                // with n > 25 loses the LSB on non-byte-aligned reads in
                // this tree (observed: 1001 read as 1000).
                vo->num_units_in_tick = get_bits_long(gb, 32);
                vo->time_scale        = get_bits_long(gb, 32);
                if (!vo->num_units_in_tick || !vo->time_scale) {
                    av_log(logctx, AV_LOG_ERROR,
                           "MVC SPS extension: mvc VUI timing "
                           "num_units_in_tick/time_scale invalid or unsupported (%u/%u)\n",
                           vo->time_scale, vo->num_units_in_tick);
                    vo->timing_present = 0;
                }
                vo->fixed_frame_rate = get_bits1(gb);
            }
            vo->nal_hrd_present = get_bits1(gb);
            if (vo->nal_hrd_present && skip_hrd_parameters(gb) < 0)
                return mvc_sps_fail(logctx, "invalid mvc VUI nal hrd_parameters");
            vo->vcl_hrd_present = get_bits1(gb);
            if (vo->vcl_hrd_present && skip_hrd_parameters(gb) < 0)
                return mvc_sps_fail(logctx, "invalid mvc VUI vcl hrd_parameters");
            if (vo->nal_hrd_present || vo->vcl_hrd_present)
                get_bits1(gb); // low_delay_hrd_flag
            vo->pic_struct_present = get_bits1(gb);
            if (vo->pic_struct_present)
                get_bits1(gb); // pic_struct
        }
    }
    av_log(logctx, AV_LOG_DEBUG,
           "MVC SPS extension: %d views, %d level value(s) (first %d), "
           "mvc vui ops %d, ext2 flag %d\n",
           m->num_views, m->num_levels, m->level_idc[0], m->vui_num_ops,
           m->additional_extension2_flag);
    if (m->mvc_vui_present && m->vui_num_ops) {
        const H264MVCVuiOp *vo = &m->vui_ops[0];
        av_log(logctx, AV_LOG_DEBUG,
               "MVC SPS extension: vui op0 targets %d view(s), "
               "num_units_in_tick/time_scale %u/%u, fixed %d, "
               "nal/vcl hrd %d/%d\n",
               vo->num_target_output_views, vo->num_units_in_tick,
               vo->time_scale, vo->fixed_frame_rate,
               vo->nal_hrd_present, vo->vcl_hrd_present);
    }

    // additional_extension2_flag: the spec ends the extension here, but
    // some 2D+delta encoders write non-spec reserved bits after the flag
    // (or omit the flag itself, hence the > 8 bits guard), so consume a
    // bounded tail to land on the rbsp stop bit.
    m->additional_extension2_flag = 0;
    if (get_bits_left(gb) > 8)
        m->additional_extension2_flag = get_bits1(gb);
    if (m->additional_extension2_flag)
        av_log(logctx, AV_LOG_DEBUG,
               "MVC SPS extension: additional_extension2 present, not parsed "
               "(reserved)\n");
    if (get_bits_left(gb) > 8) {
        int reserved_tail = get_bits_left(gb) - 8;
        if (reserved_tail > 1024)
            return mvc_sps_fail(logctx,
                                "implausible SPS reserved extension length");
        av_log(logctx, AV_LOG_DEBUG,
               "MVC SPS extension: consuming %d bits of non-spec reserved "
               "data after additional_extension2_flag\n", reserved_tail);
        skip_bits(gb, reserved_tail);
    }

    // The extension must end right before the rbsp stop bit: h2645_parse()
    // strips the stop bit and padding (residual 0), the un-stripped retry
    // contexts leave the stop bit plus up to 7 padding bits. Hence the
    // valid residual band is 0..8.
    int bits_left = get_bits_left(gb);
    if (bits_left < 0 || bits_left > 8) {
        av_log(logctx, AV_LOG_ERROR,
               "MVC SPS extension: invalid extension length (%d bits left)\n",
               bits_left);
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int decode_scaling_list(GetBitContext *gb, uint8_t *factors, int size,
                               const uint8_t *jvt_list, const uint8_t *fallback_list,
                               uint16_t *mask, int pos)
{
    int i, last = 8, next = 8;
    const uint8_t *scan = size == 16 ? ff_zigzag_scan : ff_zigzag_direct;
    uint16_t seq_scaling_list_present_flag = get_bits1(gb);
    *mask |= (seq_scaling_list_present_flag << pos);
    if (!seq_scaling_list_present_flag) /* matrix not written, we use the predicted one */
        memcpy(factors, fallback_list, size * sizeof(uint8_t));
    else
        for (i = 0; i < size; i++) {
            if (next) {
                int v = get_se_golomb(gb);
                if (v < -128 || v > 127) {
                    av_log(NULL, AV_LOG_ERROR, "delta scale %d is invalid\n", v);
                    return AVERROR_INVALIDDATA;
                }
                next = (last + v) & 0xff;
            }
            if (!i && !next) { /* matrix not written, we use the preset one */
                memcpy(factors, jvt_list, size * sizeof(uint8_t));
                break;
            }
            last = factors[scan[i]] = next ? next : last;
        }
    return 0;
}

/* returns non zero if the provided SPS scaling matrix has been filled */
static int decode_scaling_matrices(GetBitContext *gb, const SPS *sps,
                                    const PPS *pps, int is_sps,
                                    int present_flag, uint16_t *mask,
                                    uint8_t(*scaling_matrix4)[16],
                                    uint8_t(*scaling_matrix8)[64])
{
    int fallback_sps = !is_sps && sps->scaling_matrix_present;
    const uint8_t *fallback[4] = {
        fallback_sps ? sps->scaling_matrix4[0] : default_scaling4[0],
        fallback_sps ? sps->scaling_matrix4[3] : default_scaling4[1],
        fallback_sps ? sps->scaling_matrix8[0] : default_scaling8[0],
        fallback_sps ? sps->scaling_matrix8[3] : default_scaling8[1]
    };
    int ret = 0;
    *mask = 0x0;
    if (present_flag) {
        ret |= decode_scaling_list(gb, scaling_matrix4[0], 16, default_scaling4[0], fallback[0], mask, 0);        // Intra, Y
        ret |= decode_scaling_list(gb, scaling_matrix4[1], 16, default_scaling4[0], scaling_matrix4[0], mask, 1); // Intra, Cr
        ret |= decode_scaling_list(gb, scaling_matrix4[2], 16, default_scaling4[0], scaling_matrix4[1], mask, 2); // Intra, Cb
        ret |= decode_scaling_list(gb, scaling_matrix4[3], 16, default_scaling4[1], fallback[1], mask, 3);        // Inter, Y
        ret |= decode_scaling_list(gb, scaling_matrix4[4], 16, default_scaling4[1], scaling_matrix4[3], mask, 4); // Inter, Cr
        ret |= decode_scaling_list(gb, scaling_matrix4[5], 16, default_scaling4[1], scaling_matrix4[4], mask, 5); // Inter, Cb
        if (is_sps || pps->transform_8x8_mode) {
            ret |= decode_scaling_list(gb, scaling_matrix8[0], 64, default_scaling8[0], fallback[2], mask, 6); // Intra, Y
            ret |= decode_scaling_list(gb, scaling_matrix8[3], 64, default_scaling8[1], fallback[3], mask, 7); // Inter, Y
            if (sps->chroma_format_idc == 3) {
                ret |= decode_scaling_list(gb, scaling_matrix8[1], 64, default_scaling8[0], scaling_matrix8[0], mask,  8); // Intra, Cr
                ret |= decode_scaling_list(gb, scaling_matrix8[4], 64, default_scaling8[1], scaling_matrix8[3], mask,  9); // Inter, Cr
                ret |= decode_scaling_list(gb, scaling_matrix8[2], 64, default_scaling8[0], scaling_matrix8[1], mask, 10); // Intra, Cb
                ret |= decode_scaling_list(gb, scaling_matrix8[5], 64, default_scaling8[1], scaling_matrix8[4], mask, 11); // Inter, Cb
            }
        }
        if (!ret)
            ret = is_sps;
    }

    return ret;
}

void ff_h264_ps_uninit(H264ParamSets *ps)
{
    int i;

    for (i = 0; i < MAX_SPS_COUNT; i++)
        av_refstruct_unref(&ps->sps_list[i]);

    for (i = 0; i < MAX_PPS_COUNT; i++)
        av_refstruct_unref(&ps->pps_list[i]);

    av_refstruct_unref(&ps->pps);
    ps->sps = NULL;
}

int ff_h264_decode_seq_parameter_set(GetBitContext *gb, AVCodecContext *avctx,
                                     H264ParamSets *ps, int ignore_truncation)
{
    int profile_idc, level_idc, constraint_set_flags = 0;
    unsigned int sps_id;
    int i, log2_max_frame_num_minus4;
    SPS *sps;
    int ret;

    sps = av_refstruct_allocz(sizeof(*sps));
    if (!sps)
        return AVERROR(ENOMEM);

    sps->data_size = get_bits_bytesize(gb, 1);
    if (sps->data_size > sizeof(sps->data)) {
        av_log(avctx, AV_LOG_DEBUG, "Truncating likely oversized SPS\n");
        sps->data_size = sizeof(sps->data);
    }
    memcpy(sps->data, gb->buffer, sps->data_size);

    // Re-add the removed stop bit (may be used by hwaccels).
    if (!(gb->size_in_bits & 7) && sps->data_size < sizeof(sps->data))
        sps->data[sps->data_size++] = 0x80;

    profile_idc           = get_bits(gb, 8);
    constraint_set_flags |= get_bits1(gb) << 0;   // constraint_set0_flag
    constraint_set_flags |= get_bits1(gb) << 1;   // constraint_set1_flag
    constraint_set_flags |= get_bits1(gb) << 2;   // constraint_set2_flag
    constraint_set_flags |= get_bits1(gb) << 3;   // constraint_set3_flag
    constraint_set_flags |= get_bits1(gb) << 4;   // constraint_set4_flag
    constraint_set_flags |= get_bits1(gb) << 5;   // constraint_set5_flag
    skip_bits(gb, 2);                             // reserved_zero_2bits
    level_idc = get_bits(gb, 8);
    sps_id    = get_ue_golomb_31(gb);

    if (sps_id >= MAX_SPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "sps_id %u out of range\n", sps_id);
        goto fail;
    }

    sps->sps_id               = sps_id;
    sps->time_offset_length   = 24;
    sps->profile_idc          = profile_idc;
    sps->constraint_set_flags = constraint_set_flags;
    sps->level_idc            = level_idc;
    sps->vui.video_full_range_flag = -1;

    memset(sps->scaling_matrix4, 16, sizeof(sps->scaling_matrix4));
    memset(sps->scaling_matrix8, 16, sizeof(sps->scaling_matrix8));
    sps->scaling_matrix_present = 0;
    sps->vui.matrix_coeffs = AVCOL_SPC_UNSPECIFIED;

    if (sps->profile_idc == 100 ||  // High profile
        sps->profile_idc == 110 ||  // High10 profile
        sps->profile_idc == 122 ||  // High422 profile
        sps->profile_idc == 244 ||  // High444 Predictive profile
        sps->profile_idc ==  44 ||  // Cavlc444 profile
        sps->profile_idc ==  83 ||  // Scalable Constrained High profile (SVC)
        sps->profile_idc ==  86 ||  // Scalable High Intra profile (SVC)
        sps->profile_idc == 118 ||  // Stereo High profile (MVC)
        sps->profile_idc == 128 ||  // Multiview High profile (MVC)
        sps->profile_idc == 138 ||  // Multiview Depth High profile (MVCD)
        sps->profile_idc == 144) {  // old High444 profile
        sps->chroma_format_idc = get_ue_golomb_31(gb);
        if (sps->chroma_format_idc > 3U) {
            avpriv_request_sample(avctx, "chroma_format_idc %u",
                                  sps->chroma_format_idc);
            goto fail;
        } else if (sps->chroma_format_idc == 3) {
            sps->residual_color_transform_flag = get_bits1(gb);
            if (sps->residual_color_transform_flag) {
                av_log(avctx, AV_LOG_ERROR, "separate color planes are not supported\n");
                goto fail;
            }
        }
        sps->bit_depth_luma   = get_ue_golomb_31(gb) + 8;
        sps->bit_depth_chroma = get_ue_golomb_31(gb) + 8;
        if (sps->bit_depth_chroma != sps->bit_depth_luma) {
            avpriv_request_sample(avctx,
                                  "Different chroma and luma bit depth");
            goto fail;
        }
        if (sps->bit_depth_luma   < 8 || sps->bit_depth_luma   > 14 ||
            sps->bit_depth_chroma < 8 || sps->bit_depth_chroma > 14) {
            av_log(avctx, AV_LOG_ERROR, "illegal bit depth value (%d, %d)\n",
                   sps->bit_depth_luma, sps->bit_depth_chroma);
            goto fail;
        }
        sps->transform_bypass = get_bits1(gb);
        ret = decode_scaling_matrices(gb, sps, NULL, 1, get_bits1(gb),
                                      &sps->scaling_matrix_present_mask,
                                      sps->scaling_matrix4, sps->scaling_matrix8);
        if (ret < 0)
            goto fail;
        sps->scaling_matrix_present |= ret;
    } else {
        sps->chroma_format_idc = 1;
        sps->bit_depth_luma    = 8;
        sps->bit_depth_chroma  = 8;
    }

    log2_max_frame_num_minus4 = get_ue_golomb_31(gb);
    if (log2_max_frame_num_minus4 < MIN_LOG2_MAX_FRAME_NUM - 4 ||
        log2_max_frame_num_minus4 > MAX_LOG2_MAX_FRAME_NUM - 4) {
        av_log(avctx, AV_LOG_ERROR,
               "log2_max_frame_num_minus4 out of range (0-12): %d\n",
               log2_max_frame_num_minus4);
        goto fail;
    }
    sps->log2_max_frame_num = log2_max_frame_num_minus4 + 4;

    sps->poc_type = get_ue_golomb_31(gb);

    if (sps->poc_type == 0) { // FIXME #define
        unsigned t = get_ue_golomb_31(gb);
        if (t>12) {
            av_log(avctx, AV_LOG_ERROR, "log2_max_poc_lsb (%d) is out of range\n", t);
            goto fail;
        }
        sps->log2_max_poc_lsb = t + 4;
    } else if (sps->poc_type == 1) { // FIXME #define
        sps->delta_pic_order_always_zero_flag = get_bits1(gb);
        sps->offset_for_non_ref_pic           = get_se_golomb_long(gb);
        sps->offset_for_top_to_bottom_field   = get_se_golomb_long(gb);

        if (   sps->offset_for_non_ref_pic         == INT32_MIN
            || sps->offset_for_top_to_bottom_field == INT32_MIN
        ) {
            av_log(avctx, AV_LOG_ERROR,
                   "offset_for_non_ref_pic or offset_for_top_to_bottom_field is out of range\n");
            goto fail;
        }

        sps->poc_cycle_length                 = get_ue_golomb(gb);

        if ((unsigned)sps->poc_cycle_length >=
            FF_ARRAY_ELEMS(sps->offset_for_ref_frame)) {
            av_log(avctx, AV_LOG_ERROR,
                   "poc_cycle_length overflow %d\n", sps->poc_cycle_length);
            goto fail;
        }

        for (i = 0; i < sps->poc_cycle_length; i++) {
            sps->offset_for_ref_frame[i] = get_se_golomb_long(gb);
            if (sps->offset_for_ref_frame[i] == INT32_MIN) {
                av_log(avctx, AV_LOG_ERROR,
                       "offset_for_ref_frame is out of range\n");
                goto fail;
            }
        }
    } else if (sps->poc_type != 2) {
        av_log(avctx, AV_LOG_ERROR, "illegal POC type %d\n", sps->poc_type);
        goto fail;
    }

    sps->ref_frame_count = get_ue_golomb_31(gb);
    if (avctx->codec_tag == MKTAG('S', 'M', 'V', '2'))
        sps->ref_frame_count = FFMAX(2, sps->ref_frame_count);
    if (sps->ref_frame_count > H264_MAX_DPB_FRAMES) {
        av_log(avctx, AV_LOG_ERROR,
               "too many reference frames %d\n", sps->ref_frame_count);
        goto fail;
    }
    sps->gaps_in_frame_num_allowed_flag = get_bits1(gb);
    sps->mb_width                       = get_ue_golomb(gb) + 1;
    sps->mb_height                      = get_ue_golomb(gb) + 1;

    sps->frame_mbs_only_flag = get_bits1(gb);

    if (sps->mb_height >= INT_MAX / 2U) {
        av_log(avctx, AV_LOG_ERROR, "height overflow\n");
        goto fail;
    }
    sps->mb_height *= 2 - sps->frame_mbs_only_flag;

    if (!sps->frame_mbs_only_flag)
        sps->mb_aff = get_bits1(gb);
    else
        sps->mb_aff = 0;

    if ((unsigned)sps->mb_width  >= INT_MAX / 16 ||
        (unsigned)sps->mb_height >= INT_MAX / 16 ||
        av_image_check_size(16 * sps->mb_width,
                            16 * sps->mb_height, 0, avctx)) {
        av_log(avctx, AV_LOG_ERROR, "mb_width/height overflow\n");
        goto fail;
    }

    sps->direct_8x8_inference_flag = get_bits1(gb);

    sps->crop = get_bits1(gb);
    if (sps->crop) {
        unsigned int crop_left   = get_ue_golomb(gb);
        unsigned int crop_right  = get_ue_golomb(gb);
        unsigned int crop_top    = get_ue_golomb(gb);
        unsigned int crop_bottom = get_ue_golomb(gb);
        int width  = 16 * sps->mb_width;
        int height = 16 * sps->mb_height;

        if (avctx->flags2 & AV_CODEC_FLAG2_IGNORE_CROP) {
            av_log(avctx, AV_LOG_DEBUG, "discarding sps cropping, original "
                                           "values are l:%d r:%d t:%d b:%d\n",
                   crop_left, crop_right, crop_top, crop_bottom);

            sps->crop_left   =
            sps->crop_right  =
            sps->crop_top    =
            sps->crop_bottom = 0;
        } else {
            int vsub   = (sps->chroma_format_idc == 1) ? 1 : 0;
            int hsub   = (sps->chroma_format_idc == 1 ||
                          sps->chroma_format_idc == 2) ? 1 : 0;
            int step_x = 1 << hsub;
            int step_y = (2 - sps->frame_mbs_only_flag) << vsub;

            if (crop_left  > (unsigned)INT_MAX / 4 / step_x ||
                crop_right > (unsigned)INT_MAX / 4 / step_x ||
                crop_top   > (unsigned)INT_MAX / 4 / step_y ||
                crop_bottom> (unsigned)INT_MAX / 4 / step_y ||
                (crop_left + crop_right ) * step_x >= width ||
                (crop_top  + crop_bottom) * step_y >= height
            ) {
                av_log(avctx, AV_LOG_ERROR, "crop values invalid %d %d %d %d / %d %d\n", crop_left, crop_right, crop_top, crop_bottom, width, height);
                goto fail;
            }

            sps->crop_left   = crop_left   * step_x;
            sps->crop_right  = crop_right  * step_x;
            sps->crop_top    = crop_top    * step_y;
            sps->crop_bottom = crop_bottom * step_y;
        }
    } else {
        sps->crop_left   =
        sps->crop_right  =
        sps->crop_top    =
        sps->crop_bottom =
        sps->crop        = 0;
    }

    /* SPS is zero-initialized; set the default unspecified SAR here so
     * ff_set_sar() gets {0,1} when VUI is absent. decode_vui_parameters()
     * overwrites it if VUI provides explicit SAR. */
    sps->vui.sar = (AVRational){ 0, 1 };

    sps->vui_parameters_present_flag = get_bits1(gb);
    if (sps->vui_parameters_present_flag) {
        ret = decode_vui_parameters(gb, avctx, sps);
        if (ret < 0)
            goto fail;
    }

    // Multiview (Annex E) SPSes carry mvc_sps_data after the VUI; for
    // those profiles the extension is mandatory, so parse it unconditionally.
    if (sps->profile_idc == 118 ||  // Stereo High profile (MVC)
        sps->profile_idc == 128 ||  // Multiview High profile (MVC)
        sps->profile_idc == 138) {  // Multiview Depth High profile (MVCD)
        ret = decode_mvc_sps_data(gb, avctx, sps);
        if (ret < 0)
            goto fail;
        av_log(avctx, AV_LOG_DEBUG,
               "SPS %d: sequence_parameter_set_mvc_extension parsed "
               "(%d views)\n",
               sps_id, sps->mvc.num_views);
        sps->mvc.present = 1;
        if (avctx->debug & FF_DEBUG_PICT_INFO) {
            av_log(avctx, AV_LOG_DEBUG,
                   "sps:%u mvc: views=%d",
                   sps_id, (int)sps->mvc.num_views);
            for (i = 0; i < sps->mvc.num_views; i++)
                av_log(avctx, AV_LOG_DEBUG, " %d", (int)sps->mvc.view_id[i]);
            av_log(avctx, AV_LOG_DEBUG, "\n");
        }
    }

    if (get_bits_left(gb) < 0) {
        av_log_once(avctx, ignore_truncation ? AV_LOG_WARNING : AV_LOG_ERROR, AV_LOG_DEBUG,
                    &ps->overread_warning_printed[sps->vui_parameters_present_flag],
               "Overread %s by %d bits\n", sps->vui_parameters_present_flag ? "VUI" : "SPS", -get_bits_left(gb));
        if (!ignore_truncation)
            goto fail;
    }

    /* if the maximum delay is not stored in the SPS, derive it based on the
     * level */
    if (!sps->bitstream_restriction_flag &&
        (sps->ref_frame_count || avctx->strict_std_compliance >= FF_COMPLIANCE_STRICT)) {
        sps->num_reorder_frames = H264_MAX_DPB_FRAMES - 1;
        for (i = 0; i < FF_ARRAY_ELEMS(level_max_dpb_mbs); i++) {
            if (level_max_dpb_mbs[i][0] == sps->level_idc) {
                sps->num_reorder_frames = FFMIN(level_max_dpb_mbs[i][1] / (sps->mb_width * sps->mb_height),
                                                sps->num_reorder_frames);
                break;
            }
        }
    }

    if (avctx->debug & FF_DEBUG_PICT_INFO) {
        static const char csp[4][5] = { "Gray", "420", "422", "444" };
        av_log(avctx, AV_LOG_DEBUG,
               "sps:%u profile:%d/%d poc:%d ref:%d %dx%d %s %s crop:%u/%u/%u/%u %s %s %"PRId32"/%"PRId32" b%d reo:%d\n",
               sps_id, sps->profile_idc, sps->level_idc,
               sps->poc_type,
               sps->ref_frame_count,
               sps->mb_width, sps->mb_height,
               sps->frame_mbs_only_flag ? "FRM" : (sps->mb_aff ? "MB-AFF" : "PIC-AFF"),
               sps->direct_8x8_inference_flag ? "8B8" : "",
               sps->crop_left, sps->crop_right,
               sps->crop_top, sps->crop_bottom,
               sps->vui_parameters_present_flag ? "VUI" : "",
               csp[sps->chroma_format_idc],
               sps->timing_info_present_flag ? sps->num_units_in_tick : 0,
               sps->timing_info_present_flag ? sps->time_scale : 0,
               sps->bit_depth_luma,
               sps->bitstream_restriction_flag ? sps->num_reorder_frames : -1
               );
    }

    /* check if this is a repeat of an already parsed SPS, then keep the
     * original one.
     * otherwise drop all PPSes that depend on it */
    if (ps->sps_list[sps_id] &&
        !memcmp(ps->sps_list[sps_id], sps, sizeof(*sps))) {
        av_refstruct_unref(&sps);
    } else {
        remove_sps(ps, sps_id);
        ps->sps_list[sps_id] = sps;
    }

    return 0;

fail:
    av_refstruct_unref(&sps);
    return AVERROR_INVALIDDATA;
}

static void init_dequant8_coeff_table(PPS *pps, const SPS *sps)
{
    int i, j, q, x;
    const int max_qp = 51 + 6 * (sps->bit_depth_luma - 8);

    for (i = 0; i < 6; i++) {
        pps->dequant8_coeff[i] = pps->dequant8_buffer[i];
        for (j = 0; j < i; j++)
            if (!memcmp(pps->scaling_matrix8[j], pps->scaling_matrix8[i],
                        64 * sizeof(uint8_t))) {
                pps->dequant8_coeff[i] = pps->dequant8_buffer[j];
                break;
            }
        if (j < i)
            continue;

        for (q = 0; q < max_qp + 1; q++) {
            int shift = ff_h264_quant_div6[q];
            int idx   = ff_h264_quant_rem6[q];
            for (x = 0; x < 64; x++)
                pps->dequant8_coeff[i][q][(x >> 3) | ((x & 7) << 3)] =
                    ((uint32_t)ff_h264_dequant8_coeff_init[idx][ff_h264_dequant8_coeff_init_scan[((x >> 1) & 12) | (x & 3)]] *
                     pps->scaling_matrix8[i][x]) << shift;
        }
    }
}

static void init_dequant4_coeff_table(PPS *pps, const SPS *sps)
{
    int i, j, q, x;
    const int max_qp = 51 + 6 * (sps->bit_depth_luma - 8);
    for (i = 0; i < 6; i++) {
        pps->dequant4_coeff[i] = pps->dequant4_buffer[i];
        for (j = 0; j < i; j++)
            if (!memcmp(pps->scaling_matrix4[j], pps->scaling_matrix4[i],
                        16 * sizeof(uint8_t))) {
                pps->dequant4_coeff[i] = pps->dequant4_buffer[j];
                break;
            }
        if (j < i)
            continue;

        for (q = 0; q < max_qp + 1; q++) {
            int shift = ff_h264_quant_div6[q] + 2;
            int idx   = ff_h264_quant_rem6[q];
            for (x = 0; x < 16; x++)
                pps->dequant4_coeff[i][q][(x >> 2) | ((x << 2) & 0xF)] =
                    ((uint32_t)ff_h264_dequant4_coeff_init[idx][(x & 1) + ((x >> 2) & 1)] *
                     pps->scaling_matrix4[i][x]) << shift;
        }
    }
}

static void init_dequant_tables(PPS *pps, const SPS *sps)
{
    int i, x;
    init_dequant4_coeff_table(pps, sps);
    memset(pps->dequant8_coeff, 0, sizeof(pps->dequant8_coeff));

    if (pps->transform_8x8_mode)
        init_dequant8_coeff_table(pps, sps);
    if (sps->transform_bypass) {
        for (i = 0; i < 6; i++)
            for (x = 0; x < 16; x++)
                pps->dequant4_coeff[i][0][x] = 1 << 6;
        if (pps->transform_8x8_mode)
            for (i = 0; i < 6; i++)
                for (x = 0; x < 64; x++)
                    pps->dequant8_coeff[i][0][x] = 1 << 6;
    }
}

static void build_qp_table(PPS *pps, int t, int index, const int depth)
{
    int i;
    const int max_qp = 51 + 6 * (depth - 8);
    for (i = 0; i < max_qp + 1; i++)
        pps->chroma_qp_table[t][i] =
            ff_h264_chroma_qp[depth - 8][av_clip(i + index, 0, max_qp)];
}

static int more_rbsp_data_in_pps(const SPS *sps, void *logctx)
{
    int profile_idc = sps->profile_idc;

    if ((profile_idc == 66 || profile_idc == 77 ||
         profile_idc == 88) && (sps->constraint_set_flags & 7)) {
        av_log(logctx, AV_LOG_VERBOSE,
               "Current profile doesn't provide more RBSP data in PPS, skipping\n");
        return 0;
    }

    return 1;
}

static void pps_free(AVRefStructOpaque unused, void *obj)
{
    PPS *pps = obj;

    av_refstruct_unref(&pps->sps);
}

int ff_h264_decode_picture_parameter_set(GetBitContext *gb, AVCodecContext *avctx,
                                         H264ParamSets *ps, int bit_length)
{
    const SPS *sps;
    unsigned int pps_id = get_ue_golomb(gb);
    PPS *pps;
    int qp_bd_offset;
    int bits_left;
    int ret;

    if (pps_id >= MAX_PPS_COUNT) {
        av_log(avctx, AV_LOG_ERROR, "pps_id %u out of range\n", pps_id);
        return AVERROR_INVALIDDATA;
    }

    pps = av_refstruct_alloc_ext(sizeof(*pps), 0, NULL, pps_free);
    if (!pps)
        return AVERROR(ENOMEM);

    pps->data_size = get_bits_bytesize(gb, 1);
    if (pps->data_size > sizeof(pps->data)) {
        av_log(avctx, AV_LOG_DEBUG, "Truncating likely oversized PPS "
               "(%zu > %zu)\n",
               pps->data_size, sizeof(pps->data));
        pps->data_size = sizeof(pps->data);
    }
    memcpy(pps->data, gb->buffer, pps->data_size);

    // Re-add the removed stop bit (may be used by hwaccels).
    if (!(bit_length & 7) && pps->data_size < sizeof(pps->data))
        pps->data[pps->data_size++] = 0x80;

    pps->pps_id = pps_id;
    pps->sps_id = get_ue_golomb_31(gb);
    if ((unsigned)pps->sps_id >= MAX_SPS_COUNT ||
        !ps->sps_list[pps->sps_id]) {
        av_log(avctx, AV_LOG_ERROR, "sps_id %u out of range\n", pps->sps_id);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }
    pps->sps = av_refstruct_ref_c(ps->sps_list[pps->sps_id]);
    sps      = pps->sps;

    if (sps->bit_depth_luma > 14) {
        av_log(avctx, AV_LOG_ERROR,
               "Invalid luma bit depth=%d\n",
               sps->bit_depth_luma);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    } else if (sps->bit_depth_luma == 11 || sps->bit_depth_luma == 13) {
        avpriv_report_missing_feature(avctx,
               "Unimplemented luma bit depth=%d",
               sps->bit_depth_luma);
        ret = AVERROR_PATCHWELCOME;
        goto fail;
    }

    pps->cabac             = get_bits1(gb);
    pps->pic_order_present = get_bits1(gb);
    pps->slice_group_count = get_ue_golomb(gb) + 1;
    if (pps->slice_group_count > 1) {
        pps->mb_slice_group_map_type = get_ue_golomb(gb);
        avpriv_report_missing_feature(avctx, "FMO");
        ret = AVERROR_PATCHWELCOME;
        goto fail;
    }
    pps->ref_count[0] = get_ue_golomb(gb) + 1;
    pps->ref_count[1] = get_ue_golomb(gb) + 1;
    if (pps->ref_count[0] - 1 > 32 - 1 || pps->ref_count[1] - 1 > 32 - 1) {
        av_log(avctx, AV_LOG_ERROR, "reference overflow (pps)\n");
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    qp_bd_offset = 6 * (sps->bit_depth_luma - 8);

    pps->weighted_pred                        = get_bits1(gb);
    pps->weighted_bipred_idc                  = get_bits(gb, 2);
    pps->init_qp                              = get_se_golomb(gb) + 26U + qp_bd_offset;
    pps->init_qs                              = get_se_golomb(gb) + 26U + qp_bd_offset;
    pps->chroma_qp_index_offset[0]            = get_se_golomb(gb);
    if (pps->chroma_qp_index_offset[0] < -12 || pps->chroma_qp_index_offset[0] > 12) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    pps->deblocking_filter_parameters_present = get_bits1(gb);
    pps->constrained_intra_pred               = get_bits1(gb);
    pps->redundant_pic_cnt_present            = get_bits1(gb);

    pps->transform_8x8_mode = 0;
    memcpy(pps->scaling_matrix4, sps->scaling_matrix4,
           sizeof(pps->scaling_matrix4));
    memcpy(pps->scaling_matrix8, sps->scaling_matrix8,
           sizeof(pps->scaling_matrix8));

    bits_left = bit_length - get_bits_count(gb);
    if (bits_left > 0 && more_rbsp_data_in_pps(sps, avctx)) {
        pps->transform_8x8_mode = get_bits1(gb);
        pps->pic_scaling_matrix_present_flag = get_bits1(gb);
        ret = decode_scaling_matrices(gb, sps, pps, 0,
                                pps->pic_scaling_matrix_present_flag,
                                &pps->pic_scaling_matrix_present_mask,
                                pps->scaling_matrix4, pps->scaling_matrix8);
        if (ret < 0)
            goto fail;
        // second_chroma_qp_index_offset
        pps->chroma_qp_index_offset[1] = get_se_golomb(gb);
        if (pps->chroma_qp_index_offset[1] < -12 || pps->chroma_qp_index_offset[1] > 12) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
    } else {
        pps->chroma_qp_index_offset[1] = pps->chroma_qp_index_offset[0];
    }

    build_qp_table(pps, 0, pps->chroma_qp_index_offset[0],
                   sps->bit_depth_luma);
    build_qp_table(pps, 1, pps->chroma_qp_index_offset[1],
                   sps->bit_depth_luma);

    init_dequant_tables(pps, sps);

    if (pps->chroma_qp_index_offset[0] != pps->chroma_qp_index_offset[1])
        pps->chroma_qp_diff = 1;

    if (avctx->debug & FF_DEBUG_PICT_INFO) {
        av_log(avctx, AV_LOG_DEBUG,
               "pps:%u sps:%u %s slice_groups:%d ref:%u/%u %s qp:%d/%d/%d/%d %s %s %s %s\n",
               pps_id, pps->sps_id,
               pps->cabac ? "CABAC" : "CAVLC",
               pps->slice_group_count,
               pps->ref_count[0], pps->ref_count[1],
               pps->weighted_pred ? "weighted" : "",
               pps->init_qp, pps->init_qs, pps->chroma_qp_index_offset[0], pps->chroma_qp_index_offset[1],
               pps->deblocking_filter_parameters_present ? "LPAR" : "",
               pps->constrained_intra_pred ? "CONSTR" : "",
               pps->redundant_pic_cnt_present ? "REDU" : "",
               pps->transform_8x8_mode ? "8x8DCT" : "");
    }

    remove_pps(ps, pps_id);
    ps->pps_list[pps_id] = pps;

    return 0;

fail:
    av_refstruct_unref(&pps);
    return ret;
}
