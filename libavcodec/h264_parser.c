/*
 * H.26L/H.264/AVC/JVT/14496-10/... parser
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
 * H.264 / AVC / MPEG-4 part10 parser.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#define UNCHECKED_BITSTREAM_READER 1

#include <stdint.h>

#include "libavutil/attributes.h"
#include "libavutil/avutil.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"

#include "avcodec.h"
#include "get_bits.h"
#include "golomb.h"
#include "h264.h"
#include "h264dsp.h"
#include "h264_parse.h"
#include "h264_sei.h"
#include "h264_ps.h"
#include "h2645_parse.h"
#include "h264data.h"
#include "mpegutils.h"
#include "parser.h"
#include "libavutil/refstruct.h"
#include "parser_internal.h"
#include "startcode.h"

typedef struct H264ParseContext {
    ParseContext pc;
    H264ParamSets ps;
    H264DSPContext h264dsp;
    H264POCContext poc;
    H264SEIContext sei;
    int is_avc;
    int nal_length_size;
    int got_first;
    int picture_structure;
    uint8_t parse_history[6];
    int parse_history_count;
    int parse_last_mb;
    /**
     * 1 after a multiview NAL unit (types 15, 19-23) was seen after the
     * current frame start; repeated SPS/PPS/SEI inside such a group must
     * not end the frame there (an AUD still does).
     */
    int mvc_group;
    /**
     * 1 when a plain (base-view) slice NAL was seen in the current frame;
     * in a merged multiview access unit (no AUD between the views) the
     * first dependent-view NAL after it ends the frame (see
     * h264_find_frame_end()).
     */
    int base_slice;
    /**
     * Offset of the start code of the first dependent-view NAL following
     * a plain slice NAL in the current packet, 0 when absent; in
     * PARSER_FLAG_COMPLETE_FRAMES mode the packet is split there so each
     * view decodes as its own frame (see h264_find_mvc_view_split()).
     */
    int mvc_split;
    int64_t reference_dts;
    int last_frame_num, last_picture_structure;
} H264ParseContext;

static int find_start_code(const uint8_t *buf, int buf_size,
                                  int buf_index, int next_avc)
{
    uint32_t state = -1;

    buf_index = avpriv_find_start_code(buf + buf_index, buf + next_avc + 1, &state) - buf - 1;

    return FFMIN(buf_index, buf_size);
}

static int h264_find_frame_end(H264ParseContext *p, const uint8_t *buf,
                               int buf_size, void *logctx)
{
    int i, j;
    uint32_t state;
    ParseContext *pc = &p->pc;

    int next_avc = p->is_avc ? 0 : buf_size;
//    mb_addr= pc->mb_addr - 1;
    state = pc->state;
    if (state > 13)
        state = 7;

    if (p->is_avc && !p->nal_length_size)
        av_log(logctx, AV_LOG_ERROR, "AVC-parser: nal length size invalid\n");

    for (i = 0; i < buf_size; i++) {
        if (i >= next_avc) {
            int64_t nalsize = 0;
            i = next_avc;
            for (j = 0; j < p->nal_length_size; j++)
                nalsize = (nalsize << 8) | buf[i++];
            if (!nalsize || nalsize > buf_size - i) {
                av_log(logctx, AV_LOG_ERROR, "AVC-parser: nal size %"PRId64" "
                       "remaining %d\n", nalsize, buf_size - i);
                return buf_size;
            }
            next_avc = i + nalsize;
            state    = 5;
        }

        if (state == 7) {
            i += p->h264dsp.startcode_find_candidate(buf + i, next_avc - i);
            if (i < next_avc)
                state = 2;
        } else if (state <= 2) {
            if (buf[i] == 1)
                state ^= 5;            // 2->7, 1->4, 0->5
            else if (buf[i])
                state = 7;
            else
                state >>= 1;           // 2->1, 1->0, 0->0
        } else if (state <= 5) {
            int nalu_type = buf[i] & 0x1F;
            if (nalu_type == H264_NAL_SEI || nalu_type == H264_NAL_SPS ||
                nalu_type == H264_NAL_PPS || nalu_type == H264_NAL_AUD ||
                nalu_type == H264_NAL_UNSPECIFIED24) {
                /* An AUD, or an unspecified-24 NAL unit (which these
                 * multiview elementary streams emit at the start of
                 * every access unit), always ends the previous
                 * frame; SEI/SPS/PPS do so unless we are in the middle
                 * of a multiview view group. */
                if (pc->frame_start_found &&
                    (nalu_type == H264_NAL_AUD ||
                     nalu_type == H264_NAL_UNSPECIFIED24 ||
                     !p->mvc_group)) {
                    i++;
                    goto found;
                }
            } else if (nalu_type == H264_NAL_SLICE || nalu_type == H264_NAL_DPA ||
                       nalu_type == H264_NAL_IDR_SLICE) {
                p->base_slice = 1;
                state += 8;
                continue;
            } else if (nalu_type == H264_NAL_SUB_SPS ||
                       nalu_type >= H264_NAL_AUXILIARY_SLICE &&
                       nalu_type <= H264_NAL_RESERVED23) {
                /* Slice extension NALs (dependent views) follow the base
                 * view's picture in the same access unit; the first one
                 * starts a frame (delta-only streams still get one frame
                 * per access unit) but its first_mb_in_slice is left
                 * unparsed - the 24-bit extension in between would
                 * confuse the slice continuation logic. In a merged
                 * access unit (no AUD between the views), end the frame
                 * at the first dependent-view NAL so each view decodes
                 * as its own frame. */
                if (pc->frame_start_found && p->base_slice &&
                    nalu_type >= H264_NAL_AUXILIARY_SLICE &&
                    p->ps.sps && p->ps.sps->mvc.present) {
                    i++;
                    goto found;
                }
                if (pc->frame_start_found)
                    p->mvc_group = 1;
                else if (nalu_type >= H264_NAL_AUXILIARY_SLICE)
                    pc->frame_start_found = 1;
            }
            state = 7;
        } else {
            unsigned int mb, last_mb = p->parse_last_mb;
            GetBitContext gb;
            p->parse_history[p->parse_history_count++] = buf[i];

            init_get_bits(&gb, p->parse_history, 8*p->parse_history_count);
            mb= get_ue_golomb_long(&gb);
            if (get_bits_left(&gb) > 0 || p->parse_history_count > 5) {
                p->parse_last_mb = mb;
                if (pc->frame_start_found) {
                    if (mb <= last_mb) {
                        i -= p->parse_history_count - 1;
                        p->parse_history_count = 0;
                        goto found;
                    }
                } else
                    pc->frame_start_found = 1;
                p->parse_history_count = 0;
                state = 7;
            }
        }
    }
    pc->state = state;
    if (p->is_avc)
        return next_avc;
    return END_NOT_FOUND;

found:
    pc->state             = 7;
    pc->frame_start_found = 0;
    p->mvc_group           = 0;
    p->base_slice          = 0;
    if (p->is_avc)
        return next_avc;
    return i - (state & 5);
}

static int scan_mmco_reset(AVCodecParserContext *s, GetBitContext *gb,
                           void *logctx, int mv_anchor)
{
    H264PredWeightTable pwt;
    int slice_type_nos = s->pict_type & 3;
    H264ParseContext *p = s->priv_data;
    int list_count, ref_count[2];
    int mvc = p->ps.sps && p->ps.sps->mvc.present;


    if (p->ps.pps->redundant_pic_cnt_present)
        get_ue_golomb(gb); // redundant_pic_count

    if (slice_type_nos == AV_PICTURE_TYPE_B)
        get_bits1(gb); // direct_spatial_mv_pred

    if (ff_h264_parse_ref_count(&list_count, ref_count, gb, p->ps.pps,
                                slice_type_nos, p->picture_structure,
                                logctx, mvc && mv_anchor, NULL) < 0)
        return AVERROR_INVALIDDATA;

    if (slice_type_nos != AV_PICTURE_TYPE_I) {
        int list;
        for (list = 0; list < list_count; list++) {
            if (get_bits1(gb)) { // ref_pic_list_modification_flag_l[01]
                int index;
                for (index = 0; ; index++) {
                    unsigned int reordering_of_pic_nums_idc = get_ue_golomb_31(gb);
                    if (reordering_of_pic_nums_idc == 3)
                        break;

                    /* The dependent *anchor* slices of some 2D+delta MVC
                     * streams terminate this list with a non-conformant
                     * ue code (e.g. 15, or 31/32 from get_ue_golomb_31()
                     * overflow) carrying no following value: treat it as
                     * end of list, mirroring
                     * ff_h264_decode_ref_pic_list_reordering(). */
                    if (reordering_of_pic_nums_idc >= 6 && mvc && mv_anchor)
                        break;

                    if (index >= ref_count[list]) {
                        /* The extra ops above only address slots the
                         * decoder never applies; consume them, up to the
                         * storage limit, to keep the bitstream in sync. */
                        if (index >= 32 || !mvc || !mv_anchor) {
                            av_log(logctx, AV_LOG_ERROR,
                                   "reference count %d overflow\n", index);
                            return AVERROR_INVALIDDATA;
                        }
                        get_ue_golomb_long(gb);
                        continue;
                    } else if (reordering_of_pic_nums_idc > 2 &&
                               !(reordering_of_pic_nums_idc <= 5 && mvc)) {
                        /* modification_of_pic_nums_idc 4 and 5
                         * (inter-view references) are only legal on
                         * multiview streams */
                        av_log(logctx, AV_LOG_ERROR,
                               "illegal reordering_of_pic_nums_idc %d\n",
                               reordering_of_pic_nums_idc);
                        return AVERROR_INVALIDDATA;
                    }
                    get_ue_golomb_long(gb);
                }
            }
        }
    }

    if ((p->ps.pps->weighted_pred && slice_type_nos == AV_PICTURE_TYPE_P) ||
        (p->ps.pps->weighted_bipred_idc == 1 && slice_type_nos == AV_PICTURE_TYPE_B))
        ff_h264_pred_weight_table(gb, p->ps.sps, ref_count, slice_type_nos,
                                  &pwt, p->picture_structure, logctx,
                                  mvc && mv_anchor, NULL);

    if (get_bits1(gb)) { // adaptive_ref_pic_marking_mode_flag
        int i;
        for (i = 0; i < H264_MAX_MMCO_COUNT; i++) {
            if (get_bits_left(gb) < 1)
                return (mvc && mv_anchor) ? 0 : AVERROR_INVALIDDATA;

            MMCOOpcode opcode = get_ue_golomb_31(gb);
            if (opcode > (unsigned) MMCO_LONG) {
                /* The dependent *anchor* slices of some 2D+delta MVC
                 * streams terminate this list with a non-conformant ue
                 * code carrying no following fields (see the
                 * reordering-list tolerance above): treat the list as
                 * ended, mirroring ff_h264_decode_ref_pic_marking(). */
                if (mvc && mv_anchor)
                    return 0;
                av_log(logctx, AV_LOG_ERROR,
                       "illegal memory management control operation %d\n",
                       opcode);
                return AVERROR_INVALIDDATA;
            }
            if (opcode == MMCO_END)
               return 0;
            else if (opcode == MMCO_RESET)
                return 1;

            if (opcode == MMCO_SHORT2UNUSED || opcode == MMCO_SHORT2LONG)
                get_ue_golomb_long(gb); // difference_of_pic_nums_minus1
            if (opcode == MMCO_SHORT2LONG || opcode == MMCO_LONG2UNUSED ||
                opcode == MMCO_LONG || opcode == MMCO_SET_MAX_LONG)
                get_ue_golomb_31(gb);
        }
    }

    return 0;
}

/**
 * Parse NAL units of found picture and decode some basic information.
 *
 * @param s parser context.
 * @param avctx codec context.
 * @param buf buffer with field/frame data.
 * @param buf_size size of the buffer.
 */
static inline int parse_nal_units(AVCodecParserContext *s,
                                  AVCodecContext *avctx,
                                  const uint8_t * const buf, int buf_size)
{
    H264ParseContext *p = s->priv_data;
    H2645RBSP rbsp = { NULL };
    H2645NAL nal = { NULL };
    int buf_index, next_avc;
    unsigned int pps_id;
    unsigned int slice_type;
    int state = -1, got_reset = 0, got_slice = 0;
    int mv_anchor = 0;
    int q264 = buf_size >=4 && !memcmp("Q264", buf, 4);
    int field_poc[2];
    int ret;

    /* set some sane default values */
    s->pict_type         = AV_PICTURE_TYPE_I;
    s->key_frame         = 0;
    s->picture_structure = AV_PICTURE_STRUCTURE_UNKNOWN;

    ff_h264_sei_uninit(&p->sei);
    p->sei.common.frame_packing.arrangement_cancel_flag = -1;
    p->sei.common.unregistered.x264_build = -1;

    if (!buf_size)
        return 0;

    av_fast_padded_malloc(&rbsp.rbsp_buffer, &rbsp.rbsp_buffer_alloc_size, buf_size);
    if (!rbsp.rbsp_buffer)
        return AVERROR(ENOMEM);

    buf_index     = 0;
    next_avc      = p->is_avc ? 0 : buf_size;
    for (;;) {
        const SPS *sps;
        int src_length, consumed, nalsize = 0;

        mv_anchor = 0;
        if (buf_index >= next_avc) {
            nalsize = get_nalsize(p->nal_length_size, buf, buf_size, &buf_index, avctx);
            if (nalsize < 0)
                break;
            next_avc = buf_index + nalsize;
        } else {
            buf_index = find_start_code(buf, buf_size, buf_index, next_avc);
            if (buf_index >= buf_size)
                break;
            if (buf_index >= next_avc)
                continue;
        }
        src_length = next_avc - buf_index;

        state = buf[buf_index];
        switch (state & 0x1f) {
        case H264_NAL_SLICE:
        case H264_NAL_IDR_SLICE:
        case H264_NAL_AUXILIARY_SLICE:
        case H264_NAL_EXTEN_SLICE:
        case H264_NAL_DEPTH_EXTEN_SLICE:
        case H264_NAL_RESERVED22:
        case H264_NAL_RESERVED23:
            // Do not walk the whole buffer just to decode slice header
            if ((state & 0x1f) == H264_NAL_IDR_SLICE || ((state >> 5) & 0x3) == 0) {
                /* IDR or disposable slice
                 * No need to decode many bytes because MMCOs shall not be present. */
                if (src_length > 60)
                    src_length = 60;
            } else {
                /* To decode up to MMCOs */
                if (src_length > 1000)
                    src_length = 1000;
            }
            break;
        }
        consumed = ff_h2645_extract_rbsp(buf + buf_index, src_length, &rbsp, &nal, 1);
        if (consumed < 0)
            break;

        buf_index += consumed;

        ret = init_get_bits8(&nal.gb, nal.data, nal.size);
        if (ret < 0)
            goto fail;
        get_bits1(&nal.gb);
        nal.ref_idc = get_bits(&nal.gb, 2);
        nal.type    = get_bits(&nal.gb, 5);

        switch (nal.type) {
        case H264_NAL_SPS:
        case H264_NAL_SUB_SPS:
            ff_h264_decode_seq_parameter_set(&nal.gb, avctx, &p->ps, 0);
            break;
        case H264_NAL_PPS:
            ff_h264_decode_picture_parameter_set(&nal.gb, avctx, &p->ps,
                                                 nal.size_bits);
            break;
        case H264_NAL_SEI:
            /* After the first slice only parameter set NAL units matter;
             * later SEIs (e.g. the dependent view's in a merged access
             * unit) must not disturb the consumed picture-timing state. */
            if (!got_slice)
                ff_h264_sei_decode(&p->sei, &nal.gb, &p->ps, avctx);
            break;
        case H264_NAL_AUXILIARY_SLICE:
        case H264_NAL_EXTEN_SLICE:
        case H264_NAL_DEPTH_EXTEN_SLICE:
        case H264_NAL_RESERVED22:
        case H264_NAL_RESERVED23:
            if (got_slice)
                break;
            /* Dependent-view slice NAL (multiview): skip the 24-bit
             * nal_unit_header_mvc_extension() and parse the rest of the
             * slice header like a plain slice, so delta-only streams
             * (no plain slice NAL) still get their picture properties.
             * SVC extension headers are not supported here. */
            if (get_bits_left(&nal.gb) < 24 ||
                get_bits1(&nal.gb) != 0) { // svc_extension_flag
                av_log(avctx, AV_LOG_WARNING,
                       "unsupported mvc/svc slice NAL header (type %d)\n",
                       nal.type);
                break;
            }
            /* nal_unit_header_mvc_extension(): non_idr(1) priority_idc(6)
             * view_id(10) temporal_id(3) anchor(1) inter_view(1)
             * reserved_one(1), as read in h2645_parse.c. Only the anchor
             * flag survives (the MMCO scan tolerates degenerate lists
             * only on anchor slices). */
            skip_bits(&nal.gb, 20);
            mv_anchor    = get_bits1(&nal.gb);
            skip_bits(&nal.gb, 2);
            av_fallthrough;
        case H264_NAL_IDR_SLICE:
            /* Only true IDR slices reset the POC state; fall-through
             * from a dependent-view slice NAL unit must not. */
            if (got_slice)
                break;
            if (nal.type == H264_NAL_IDR_SLICE) {
                s->key_frame = 1;

                p->poc.prev_frame_num        = 0;
                p->poc.prev_frame_num_offset = 0;
                p->poc.prev_poc_msb          =
                p->poc.prev_poc_lsb          = 0;
            }
            av_fallthrough;
        case H264_NAL_SLICE:
            if (got_slice)
                break;
            get_ue_golomb_long(&nal.gb);  // skip first_mb_in_slice
            slice_type   = get_ue_golomb_31(&nal.gb);
            s->pict_type = ff_h264_golomb_to_pict_type[slice_type % 5];
            if (p->sei.recovery_point.recovery_frame_cnt >= 0) {
                /* key frame, since recovery_frame_cnt is set */
                s->key_frame = 1;
            }
            pps_id = get_ue_golomb(&nal.gb);
            if (pps_id >= MAX_PPS_COUNT) {
                av_log(avctx, AV_LOG_ERROR,
                       "pps_id %u out of range\n", pps_id);
                goto fail;
            }
            if (!p->ps.pps_list[pps_id]) {
                av_log(avctx, AV_LOG_ERROR,
                       "non-existing PPS %u referenced\n", pps_id);
                goto fail;
            }

            av_refstruct_replace(&p->ps.pps, p->ps.pps_list[pps_id]);
            p->ps.sps = p->ps.pps->sps;
            sps       = p->ps.sps;

            // heuristic to detect non marked keyframes
            if (p->ps.sps->ref_frame_count <= 1 && p->ps.pps->ref_count[0] <= 1 && s->pict_type == AV_PICTURE_TYPE_I)
                s->key_frame = 1;

            p->poc.frame_num = get_bits(&nal.gb, sps->log2_max_frame_num);

            s->coded_width  = 16 * sps->mb_width;
            s->coded_height = 16 * sps->mb_height;
            s->width        = s->coded_width  - (sps->crop_right + sps->crop_left);
            s->height       = s->coded_height - (sps->crop_top   + sps->crop_bottom);
            if (s->width <= 0 || s->height <= 0) {
                s->width  = s->coded_width;
                s->height = s->coded_height;
            }

            switch (sps->bit_depth_luma) {
            case 9:
                if (sps->chroma_format_idc == 3)      s->format = AV_PIX_FMT_YUV444P9;
                else if (sps->chroma_format_idc == 2) s->format = AV_PIX_FMT_YUV422P9;
                else                                  s->format = AV_PIX_FMT_YUV420P9;
                break;
            case 10:
                if (sps->chroma_format_idc == 3)      s->format = AV_PIX_FMT_YUV444P10;
                else if (sps->chroma_format_idc == 2) s->format = AV_PIX_FMT_YUV422P10;
                else                                  s->format = AV_PIX_FMT_YUV420P10;
                break;
            case 8:
                if (sps->chroma_format_idc == 3)      s->format = AV_PIX_FMT_YUV444P;
                else if (sps->chroma_format_idc == 2) s->format = AV_PIX_FMT_YUV422P;
                else                                  s->format = AV_PIX_FMT_YUV420P;
                break;
            default:
                s->format = AV_PIX_FMT_NONE;
            }

            avctx->profile = ff_h264_get_profile(sps);
            avctx->level   = sps->level_idc;

            if (sps->frame_mbs_only_flag) {
                p->picture_structure = PICT_FRAME;
            } else {
                if (get_bits1(&nal.gb)) { // field_pic_flag
                    p->picture_structure = PICT_TOP_FIELD + get_bits1(&nal.gb); // bottom_field_flag
                } else {
                    p->picture_structure = PICT_FRAME;
                }
            }

            if (nal.type == H264_NAL_IDR_SLICE)
                get_ue_golomb_long(&nal.gb); /* idr_pic_id */
            if (sps->poc_type == 0) {
                p->poc.poc_lsb = get_bits(&nal.gb, sps->log2_max_poc_lsb);

                if (p->ps.pps->pic_order_present == 1 &&
                    p->picture_structure == PICT_FRAME)
                    p->poc.delta_poc_bottom = get_se_golomb(&nal.gb);
            }

            if (sps->poc_type == 1 &&
                !sps->delta_pic_order_always_zero_flag) {
                p->poc.delta_poc[0] = get_se_golomb(&nal.gb);

                if (p->ps.pps->pic_order_present == 1 &&
                    p->picture_structure == PICT_FRAME)
                    p->poc.delta_poc[1] = get_se_golomb(&nal.gb);
            }

            /* Decode POC of this picture.
             * The prev_ values needed for decoding POC of the next picture are not set here. */
            field_poc[0] = field_poc[1] = INT_MAX;
            ret = ff_h264_init_poc(field_poc, &s->output_picture_number, sps,
                             &p->poc, p->picture_structure, nal.ref_idc);
            if (ret < 0)
                goto fail;

            /* Continue parsing to check if MMCO_RESET is present.
             * FIXME: MMCO_RESET could appear in non-first slice.
             *        Maybe, we should parse all undisposable non-IDR slice of this
             *        picture until encountering MMCO_RESET in a slice of it. */
            if (nal.ref_idc && nal.type != H264_NAL_IDR_SLICE) {
                got_reset = scan_mmco_reset(s, &nal.gb, avctx, mv_anchor);
                if (got_reset < 0)
                    goto fail;
            }

            /* Set up the prev_ values for decoding POC of the next picture. */
            p->poc.prev_frame_num        = got_reset ? 0 : p->poc.frame_num;
            p->poc.prev_frame_num_offset = got_reset ? 0 : p->poc.frame_num_offset;
            if (nal.ref_idc != 0) {
                if (!got_reset) {
                    p->poc.prev_poc_msb = p->poc.poc_msb;
                    p->poc.prev_poc_lsb = p->poc.poc_lsb;
                } else {
                    p->poc.prev_poc_msb = 0;
                    p->poc.prev_poc_lsb =
                        p->picture_structure == PICT_BOTTOM_FIELD ? 0 : field_poc[0];
                }
            }

            if (p->sei.picture_timing.present) {
                ret = ff_h264_sei_process_picture_timing(&p->sei.picture_timing,
                                                         sps, avctx);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR, "Error processing the picture timing SEI\n");
                    p->sei.picture_timing.present = 0;
                }
            }

            if (sps->pic_struct_present_flag && p->sei.picture_timing.present) {
                switch (p->sei.picture_timing.pic_struct) {
                case H264_SEI_PIC_STRUCT_TOP_FIELD:
                case H264_SEI_PIC_STRUCT_BOTTOM_FIELD:
                    s->repeat_pict = 0;
                    break;
                case H264_SEI_PIC_STRUCT_FRAME:
                case H264_SEI_PIC_STRUCT_TOP_BOTTOM:
                case H264_SEI_PIC_STRUCT_BOTTOM_TOP:
                    s->repeat_pict = 1;
                    break;
                case H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
                case H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
                    s->repeat_pict = 2;
                    break;
                case H264_SEI_PIC_STRUCT_FRAME_DOUBLING:
                    s->repeat_pict = 3;
                    break;
                case H264_SEI_PIC_STRUCT_FRAME_TRIPLING:
                    s->repeat_pict = 5;
                    break;
                default:
                    s->repeat_pict = p->picture_structure == PICT_FRAME ? 1 : 0;
                    break;
                }
            } else {
                s->repeat_pict = p->picture_structure == PICT_FRAME ? 1 : 0;
            }

            if (p->picture_structure == PICT_FRAME) {
                s->picture_structure = AV_PICTURE_STRUCTURE_FRAME;
                if (sps->pic_struct_present_flag && p->sei.picture_timing.present) {
                    switch (p->sei.picture_timing.pic_struct) {
                    case H264_SEI_PIC_STRUCT_TOP_BOTTOM:
                    case H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
                        s->field_order = AV_FIELD_TT;
                        break;
                    case H264_SEI_PIC_STRUCT_BOTTOM_TOP:
                    case H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
                        s->field_order = AV_FIELD_BB;
                        break;
                    default:
                        s->field_order = AV_FIELD_PROGRESSIVE;
                        break;
                    }
                } else {
                    if (field_poc[0] < field_poc[1])
                        s->field_order = AV_FIELD_TT;
                    else if (field_poc[0] > field_poc[1])
                        s->field_order = AV_FIELD_BB;
                    else
                        s->field_order = AV_FIELD_PROGRESSIVE;
                }
            } else {
                if (p->picture_structure == PICT_TOP_FIELD)
                    s->picture_structure = AV_PICTURE_STRUCTURE_TOP_FIELD;
                else
                    s->picture_structure = AV_PICTURE_STRUCTURE_BOTTOM_FIELD;
                if (p->poc.frame_num == p->last_frame_num &&
                    p->last_picture_structure != AV_PICTURE_STRUCTURE_UNKNOWN &&
                    p->last_picture_structure != AV_PICTURE_STRUCTURE_FRAME &&
                    p->last_picture_structure != s->picture_structure) {
                    if (p->last_picture_structure == AV_PICTURE_STRUCTURE_TOP_FIELD)
                        s->field_order = AV_FIELD_TT;
                    else
                        s->field_order = AV_FIELD_BB;
                } else {
                    s->field_order = AV_FIELD_UNKNOWN;
                }
                p->last_picture_structure = s->picture_structure;
                p->last_frame_num = p->poc.frame_num;
            }
            if (sps->timing_info_present_flag) {
                int64_t den = sps->time_scale;
                if (p->sei.common.unregistered.x264_build < 44U)
                    den *= 2;
                av_reduce(&avctx->framerate.den, &avctx->framerate.num,
                          sps->num_units_in_tick * 2, den, 1 << 30);
            }

            got_slice = 1;
            /* The rbsp buffer is allocated once for the whole frame and
             * reused for the NAL units scanned after the first slice:
             * reset the offset instead of freeing it (released at fail). */
            rbsp.rbsp_buffer_size = 0;
            /* Some multiview streams refresh SPS/PPS inside the same
             * access unit, after the base view's slices: keep scanning
             * so they reach p->ps; without this the parser never learns
             * the dependent view's SUB_SPS/PPS. */
            break;
        }
    }
    if (q264) {
        av_freep(&rbsp.rbsp_buffer);
        return 0;
    }
    if (got_slice)
        ret = 0;
    else {
        /* didn't find a picture! */
        av_log(avctx, AV_LOG_ERROR, "missing picture in access unit with size %d\n", buf_size);
        ret = -1;
    }
fail:
    av_freep(&rbsp.rbsp_buffer);
    return got_slice ? 0 : ret;
}

/*
 * Some multiview streams merge the base view's plain slices and the
 * dependent view's parameter sets and slice extension NALs into one
 * container packet without an AUD between the views. In
 * PARSER_FLAG_COMPLETE_FRAMES mode the whole packet is one frame by
 * definition, so return the offset of the first dependent-view NAL
 * following a plain slice NAL (its start code, or its length field in
 * NALFF); the packet is split there so each view decodes as its own
 * frame. Returns 0 when no such boundary exists. A dependent-view
 * marker is a subset SPS (type 15) or a slice extension NAL (types
 * 19-23), seen only after a plain base-view slice.
 */
static int h264_find_mvc_view_split(const H264ParseContext *p,
                                    const uint8_t *buf, int buf_size,
                                    void *logctx)
{
    int i = 0, base_slice = 0;
    int nalff = 0;

    /* NALFF packets begin with a (1-4 byte) NAL length; Annex B with a
     * start code. The parser context reports is_avc from the container
     * CodecPrivate, so sanity-check the first NAL length as well. */
    if (p->is_avc) {
        int j, len = 0;
        for (j = 0; j < p->nal_length_size && j < 4; j++)
            len = (len << 8) | buf[j];
        if (len > 0 && len < buf_size)
            nalff = 1;
    } else if (buf_size >= 4 && !buf[0] && !buf[1] &&
               (buf[2] == 1 || (!buf[2] && buf[3] == 1))) {
        /* Annex B: nothing to check up front. */
    } else {
        return 0; // neither NALFF nor Annex B shape
    }

    for (;;) {
        int nalu_type, nal_begin;

        if (nalff) {
            int nalsize;

            /* Not enough room for another length: the buffer was a clean
             * NAL sequence (e.g. a re-fed dependent part) - nothing more. */
            if (i > buf_size - p->nal_length_size)
                return 0;
            nal_begin = i;
            nalsize = get_nalsize(p->nal_length_size, buf, buf_size, &i,
                                  logctx);
            if (nalsize < 0 || i + nalsize > buf_size)
                return 0;
            nalu_type = buf[i] & 0x1F;
            i += nalsize;
        } else {
            i = find_start_code(buf, buf_size, i, buf_size);
            if (i >= buf_size)
                return 0;
            if (i < 2 || buf[i - 1] != 1) {
                i++;
                continue;
            }
            nalu_type = buf[i] & 0x1F;
            // walk back over the zeros of the start code
            nal_begin = i - 2;
            while (nal_begin > 0 && buf[nal_begin - 1] == 0)
                nal_begin--;
            i++;
        }

        if (nalu_type == H264_NAL_SLICE || nalu_type == H264_NAL_IDR_SLICE) {
            base_slice = 1;
        } else if (base_slice &&
                   (nalu_type == H264_NAL_SUB_SPS ||
                    (nalu_type >= H264_NAL_AUXILIARY_SLICE &&
                     nalu_type <= H264_NAL_RESERVED23))) {
            return nal_begin;
        }
    }
}

static int h264_parse(AVCodecParserContext *s,
                      AVCodecContext *avctx,
                      const uint8_t **poutbuf, int *poutbuf_size,
                      const uint8_t *buf, int buf_size)
{
    H264ParseContext *p = s->priv_data;
    ParseContext *pc = &p->pc;
    AVRational time_base = { 0, 1 };
    int next, parsed_size = buf_size;

    if (!p->got_first) {
        p->got_first = 1;
        if (avctx->extradata_size) {
            ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                      &p->ps, &p->is_avc, &p->nal_length_size,
                                      avctx->err_recognition, avctx);
        }
    }

    p->mvc_split = 0;
    if (s->flags & PARSER_FLAG_COMPLETE_FRAMES) {
        next = buf_size;
        p->mvc_split = h264_find_mvc_view_split(p, buf, buf_size, avctx);
        if (p->mvc_split > 0 && p->mvc_split < buf_size) {
            next = p->mvc_split;
            parsed_size = p->mvc_split;
        }
    } else {
        next = h264_find_frame_end(p, buf, buf_size, avctx);

        if (ff_combine_frame(pc, next, &buf, &buf_size) < 0) {
            *poutbuf      = NULL;
            *poutbuf_size = 0;
            return buf_size;
        }

        if (next < 0 && next != END_NOT_FOUND) {
            av_assert1(pc->last_index + next >= 0);
            h264_find_frame_end(p, &pc->buffer[pc->last_index + next], -next, avctx); // update state
        }

        /* ff_combine_frame() replaces the pending frame with the combined
         * one and reports its size in buf_size; parse that frame.
         * parsed_size (the incoming packet's size) is only the right
         * range in COMPLETE_FRAMES mode, where no combining happens. */
        parsed_size = buf_size;
    }

    parse_nal_units(s, avctx, buf, parsed_size);

    if (avctx->framerate.num)
        time_base = av_inv_q(av_mul_q(avctx->framerate, (AVRational){2, 1}));
    if (p->sei.picture_timing.cpb_removal_delay >= 0) {
        s->dts_sync_point    = p->sei.buffering_period.present;
        s->dts_ref_dts_delta = p->sei.picture_timing.cpb_removal_delay;
        s->pts_dts_delta     = p->sei.picture_timing.dpb_output_delay;
    } else {
        s->dts_sync_point    = INT_MIN;
        s->dts_ref_dts_delta = INT_MIN;
        s->pts_dts_delta     = INT_MIN;
    }

    if (s->flags & PARSER_FLAG_ONCE) {
        s->flags &= PARSER_FLAG_COMPLETE_FRAMES;
    }

    if (s->dts_sync_point >= 0) {
        int64_t den = time_base.den * (int64_t)avctx->pkt_timebase.num;
        if (den > 0) {
            int64_t num = time_base.num * (int64_t)avctx->pkt_timebase.den;
            if (s->dts != AV_NOPTS_VALUE) {
                // got DTS from the stream, update reference timestamp
                p->reference_dts = av_sat_sub64(s->dts, av_rescale(s->dts_ref_dts_delta, num, den));
            } else if (p->reference_dts != AV_NOPTS_VALUE) {
                // compute DTS based on reference timestamp
                s->dts = av_sat_add64(p->reference_dts, av_rescale(s->dts_ref_dts_delta, num, den));
            }

            if (p->reference_dts != AV_NOPTS_VALUE && s->pts == AV_NOPTS_VALUE) {
                int64_t pts_dts_delta = av_rescale(s->pts_dts_delta, num, den);
                uint64_t pts = (uint64_t)s->dts + pts_dts_delta;
                if (pts == av_sat_add64(s->dts, pts_dts_delta))
                    s->pts = pts;
            }

            if (s->dts_sync_point > 0)
                p->reference_dts = s->dts; // new reference
        }
    }

    if (p->mvc_split > 0 && p->mvc_split < buf_size) {
        /* Emit the base view part now; the demuxer feeds the remainder
         * back in for the dependent view (and any further views). */
        *poutbuf      = buf;
        *poutbuf_size = p->mvc_split;
        return p->mvc_split;
    }

    *poutbuf      = buf;
    *poutbuf_size = buf_size;
    return next;
}

static av_cold void h264_close(AVCodecParserContext *s)
{
    H264ParseContext *p = s->priv_data;
    ParseContext *pc = &p->pc;

    av_freep(&pc->buffer);

    ff_h264_sei_uninit(&p->sei);
    ff_h264_ps_uninit(&p->ps);
}

static av_cold int init(AVCodecParserContext *s)
{
    H264ParseContext *p = s->priv_data;

    p->reference_dts = AV_NOPTS_VALUE;
    p->last_frame_num = INT_MAX;
    p->base_slice     = 0;
    ff_h264dsp_init(&p->h264dsp, 8, 1);
    return 0;
}

const FFCodecParser ff_h264_parser = {
    PARSER_CODEC_LIST(AV_CODEC_ID_H264),
    .priv_data_size = sizeof(H264ParseContext),
    .init           = init,
    .parse          = h264_parse,
    .close          = h264_close,
};
