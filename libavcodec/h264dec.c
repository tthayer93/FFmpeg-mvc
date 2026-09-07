/*
 * H.26L/H.264/AVC/JVT/14496-10/... decoder
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
 * H.264 / AVC / MPEG-4 part10 codec.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#define UNCHECKED_BITSTREAM_READER 1

#include "config_components.h"

#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/emms.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/stereo3d.h"
#include "libavutil/thread.h"
#include "libavutil/video_enc_params.h"

#include "codec_internal.h"
#include "internal.h"
#include "error_resilience.h"
#include "avcodec.h"
#include "h264.h"
#include "h264dec.h"
#include "h2645_parse.h"
#include "h264data.h"
#include "h264_ps.h"
#include "golomb.h"
#include "hwaccel_internal.h"
#include "hwconfig.h"
#include "mpegutils.h"
#include "profiles.h"
#include "rectangle.h"
#include "libavutil/refstruct.h"
#include "thread.h"
#include "threadframe.h"

const uint16_t ff_h264_mb_sizes[4] = { 256, 384, 512, 768 };

int avpriv_h264_has_num_reorder_frames(AVCodecContext *avctx)
{
    H264Context *h = avctx->priv_data;
    return h && h->ps.sps ? h->ps.sps->num_reorder_frames : 0;
}

static void h264_er_decode_mb(void *opaque, int ref, int mv_dir, int mv_type,
                              int (*mv)[2][4][2],
                              int mb_x, int mb_y, int mb_intra, int mb_skipped)
{
    const H264Context *h = opaque;
    H264SliceContext *sl = &h->slice_ctx[0];

    sl->mb_x = mb_x;
    sl->mb_y = mb_y;
    sl->mb_xy = mb_x + mb_y * h->mb_stride;
    memset(sl->non_zero_count_cache, 0, sizeof(sl->non_zero_count_cache));
    av_assert1(ref >= 0);
    /* FIXME: It is possible albeit uncommon that slice references
     * differ between slices. We take the easy approach and ignore
     * it for now. If this turns out to have any relevance in
     * practice then correct remapping should be added. */
    if (ref >= sl->ref_count[0])
        ref = 0;
    if (!sl->ref_list[0][ref].data[0]) {
        av_log(h->avctx, AV_LOG_DEBUG, "Reference not available for error concealing\n");
        ref = 0;
    }
    if ((sl->ref_list[0][ref].reference&3) != 3) {
        av_log(h->avctx, AV_LOG_DEBUG, "Reference invalid\n");
        return;
    }
    fill_rectangle(&h->cur_pic.ref_index[0][4 * sl->mb_xy],
                   2, 2, 2, ref, 1);
    fill_rectangle(&sl->ref_cache[0][scan8[0]], 4, 4, 8, ref, 1);
    fill_rectangle(sl->mv_cache[0][scan8[0]], 4, 4, 8,
                   pack16to32((*mv)[0][0][0], (*mv)[0][0][1]), 4);
    sl->mb_mbaff =
    sl->mb_field_decoding_flag = 0;
    ff_h264_hl_decode_mb(h, &h->slice_ctx[0]);
}

void ff_h264_draw_horiz_band(const H264Context *h, H264SliceContext *sl,
                             int y, int height)
{
    AVCodecContext *avctx = h->avctx;
    const AVFrame   *src  = h->cur_pic.f;
    const AVPixFmtDescriptor *desc;
    int offset[AV_NUM_DATA_POINTERS];
    int vshift;
    const int field_pic = h->picture_structure != PICT_FRAME;

    if (!avctx->draw_horiz_band)
        return;

    if (field_pic && h->first_field && !(avctx->slice_flags & SLICE_FLAG_ALLOW_FIELD))
        return;

    if (field_pic) {
        height <<= 1;
        y      <<= 1;
    }

    height = FFMIN(height, avctx->height - y);

    desc   = av_pix_fmt_desc_get(avctx->pix_fmt);
    vshift = desc->log2_chroma_h;

    offset[0] = y * src->linesize[0];
    offset[1] =
    offset[2] = (y >> vshift) * src->linesize[1];
    for (int i = 3; i < AV_NUM_DATA_POINTERS; i++)
        offset[i] = 0;

    emms_c();

    avctx->draw_horiz_band(avctx, src, offset,
                           y, h->picture_structure, height);
}

void ff_h264_free_tables(H264Context *h)
{
    int i;

    av_freep(&h->intra4x4_pred_mode);
    av_freep(&h->chroma_pred_mode_table);
    av_freep(&h->cbp_table);
    av_freep(&h->mvd_table[0]);
    av_freep(&h->mvd_table[1]);
    av_freep(&h->direct_table);
    av_freep(&h->non_zero_count);
    av_freep(&h->slice_table_base);
    h->slice_table = NULL;
    av_freep(&h->list_counts);

    av_freep(&h->mb2b_xy);
    av_freep(&h->mb2br_xy);

    av_refstruct_pool_uninit(&h->qscale_table_pool);
    av_refstruct_pool_uninit(&h->mb_type_pool);
    av_refstruct_pool_uninit(&h->motion_val_pool);
    av_refstruct_pool_uninit(&h->ref_index_pool);

#if CONFIG_ERROR_RESILIENCE
    av_freep(&h->er.mb_index2xy);
    av_freep(&h->er.error_status_table);
    av_freep(&h->er.er_temp_buffer);
    av_freep(&h->dc_val_base);
#endif

    for (i = 0; i < h->nb_slice_ctx; i++) {
        H264SliceContext *sl = &h->slice_ctx[i];

        av_freep(&sl->bipred_scratchpad);
        av_freep(&sl->edge_emu_buffer);
        av_freep(&sl->top_borders[0]);
        av_freep(&sl->top_borders[1]);

        sl->bipred_scratchpad_allocated = 0;
        sl->edge_emu_buffer_allocated   = 0;
        sl->top_borders_allocated[0]    = 0;
        sl->top_borders_allocated[1]    = 0;
    }
}

int ff_h264_alloc_tables(H264Context *h)
{
    ERContext *const er = &h->er;
    const int big_mb_num = h->mb_stride * (h->mb_height + 1);
    const int row_mb_num = 2*h->mb_stride*FFMAX(h->nb_slice_ctx, 1);
    const int st_size = big_mb_num + h->mb_stride;
    int x, y;

    if (!FF_ALLOCZ_TYPED_ARRAY(h->intra4x4_pred_mode,     row_mb_num * 8)  ||
        !FF_ALLOCZ_TYPED_ARRAY(h->non_zero_count,         big_mb_num)      ||
        !FF_ALLOCZ_TYPED_ARRAY(h->slice_table_base,       st_size)         ||
        !FF_ALLOCZ_TYPED_ARRAY(h->cbp_table,              big_mb_num)      ||
        !FF_ALLOCZ_TYPED_ARRAY(h->chroma_pred_mode_table, big_mb_num)      ||
        !FF_ALLOCZ_TYPED_ARRAY(h->mvd_table[0],           row_mb_num * 8)  ||
        !FF_ALLOCZ_TYPED_ARRAY(h->mvd_table[1],           row_mb_num * 8)  ||
        !FF_ALLOCZ_TYPED_ARRAY(h->direct_table,           big_mb_num * 4)  ||
        !FF_ALLOCZ_TYPED_ARRAY(h->list_counts,            big_mb_num)      ||
        !FF_ALLOCZ_TYPED_ARRAY(h->mb2b_xy,                big_mb_num)      ||
        !FF_ALLOCZ_TYPED_ARRAY(h->mb2br_xy,               big_mb_num))
        return AVERROR(ENOMEM);
    h->slice_ctx[0].intra4x4_pred_mode = h->intra4x4_pred_mode;
    h->slice_ctx[0].mvd_table[0] = h->mvd_table[0];
    h->slice_ctx[0].mvd_table[1] = h->mvd_table[1];
    memset(h->slice_table_base, -1,
           st_size * sizeof(*h->slice_table_base));
    h->slice_table = h->slice_table_base + h->mb_stride * 2 + 1;
    for (y = 0; y < h->mb_height; y++)
        for (x = 0; x < h->mb_width; x++) {
            const int mb_xy = x + y * h->mb_stride;
            const int b_xy  = 4 * x + 4 * y * h->b_stride;

            h->mb2b_xy[mb_xy]  = b_xy;
            h->mb2br_xy[mb_xy] = 8 * (FMO ? mb_xy : (mb_xy % (2 * h->mb_stride)));
        }

    if (CONFIG_ERROR_RESILIENCE) {
        int y_size  = (2 * h->mb_width + 1) * (2 * h->mb_height + 1);
        int yc_size = y_size + 2 * big_mb_num;

        /* init ER */
        er->avctx          = h->avctx;
        er->decode_mb      = h264_er_decode_mb;
        er->opaque         = h;
        er->quarter_sample = 1;

        er->mb_num      = h->mb_num;
        er->mb_width    = h->mb_width;
        er->mb_height   = h->mb_height;
        er->mb_stride   = h->mb_stride;
        er->b8_stride   = h->mb_width * 2 + 1;

        // error resilience code looks cleaner with this
        if (!FF_ALLOCZ_TYPED_ARRAY(er->mb_index2xy,        h->mb_num + 1) ||
            !FF_ALLOCZ_TYPED_ARRAY(h->dc_val_base,         yc_size))
            return AVERROR(ENOMEM); // ff_h264_free_tables will clean up for us

        for (y = 0; y < h->mb_height; y++)
            for (x = 0; x < h->mb_width; x++)
                er->mb_index2xy[x + y * h->mb_width] = x + y * h->mb_stride;

        er->mb_index2xy[h->mb_height * h->mb_width] = (h->mb_height - 1) *
                                                      h->mb_stride + h->mb_width;
        er->dc_val[0] = h->dc_val_base + h->mb_width * 2 + 2;
        er->dc_val[1] = h->dc_val_base + y_size + h->mb_stride + 1;
        er->dc_val[2] = er->dc_val[1] + big_mb_num;
        for (int i = 0; i < yc_size; i++)
            h->dc_val_base[i] = 1024;

        return ff_er_init(er);
    }

    return 0;
}

/**
 * Init slice context
 */
void ff_h264_slice_context_init(H264Context *h, H264SliceContext *sl)
{
    sl->ref_cache[0][scan8[5]  + 1] =
    sl->ref_cache[0][scan8[7]  + 1] =
    sl->ref_cache[0][scan8[13] + 1] =
    sl->ref_cache[1][scan8[5]  + 1] =
    sl->ref_cache[1][scan8[7]  + 1] =
    sl->ref_cache[1][scan8[13] + 1] = PART_NOT_AVAILABLE;

    sl->er = &h->er;
}

static int h264_init_pic(H264Picture *pic)
{
    pic->f = av_frame_alloc();
    if (!pic->f)
        return AVERROR(ENOMEM);

    pic->f_grain = av_frame_alloc();
    if (!pic->f_grain)
        return AVERROR(ENOMEM);

    return 0;
}

static int h264_init_context(AVCodecContext *avctx, H264Context *h)
{
    int i, ret;

    h->avctx                 = avctx;
    h->cur_chroma_format_idc = -1;

    h->width_from_caller     = avctx->width;
    h->height_from_caller    = avctx->height;

    h->workaround_bugs       = avctx->workaround_bugs;
    h->flags                 = avctx->flags;
    h->mv_view_id            = -1;
    h->recovery_frame        = -1;
    h->frame_recovered       = 0;
    h->sei.common.frame_packing.arrangement_cancel_flag = -1;
    h->sei.common.unregistered.x264_build = -1;

    /* base view (slot 0) is always registered */
    h->view_count = 1;
    h->cur_view   = 0;
    for (i = 0; i < H264_MAX_MVC_VIEWS; i++)
        h->views[i].view_id = -1;
    h->views[0].view_id       = 0;
    h->views[0].poc.prev_poc_msb = 1 << 16;
    h->views[0].poc.prev_frame_num = -1;
    h->views[0].next_outputed_poc = INT_MIN;
    for (i = 0; i < H264_MAX_MVC_VIEWS; i++)
        for (int j = 0; j < FF_ARRAY_ELEMS(h->views[i].last_pocs); j++)
            h->views[i].last_pocs[j] = INT_MIN;

    ff_h264_sei_uninit(&h->sei);

    if (avctx->active_thread_type & FF_THREAD_FRAME) {
        h->decode_error_flags_pool = av_refstruct_pool_alloc(sizeof(atomic_int), 0);
        if (!h->decode_error_flags_pool)
            return AVERROR(ENOMEM);
    }

    h->nb_slice_ctx = (avctx->active_thread_type & FF_THREAD_SLICE) ? avctx->thread_count : 1;
    h->slice_ctx = av_calloc(h->nb_slice_ctx, sizeof(*h->slice_ctx));
    if (!h->slice_ctx) {
        h->nb_slice_ctx = 0;
        return AVERROR(ENOMEM);
    }

    for (i = 0; i < H264_MAX_PICTURE_COUNT; i++) {
        if ((ret = h264_init_pic(&h->DPB[i])) < 0)
            return ret;
    }

    if ((ret = h264_init_pic(&h->cur_pic)) < 0)
        return ret;

    if ((ret = h264_init_pic(&h->last_pic_for_ec)) < 0)
        return ret;

    for (i = 0; i < h->nb_slice_ctx; i++)
        h->slice_ctx[i].h264 = h;

    return 0;
}

static void h264_free_pic(H264Context *h, H264Picture *pic)
{
    ff_h264_unref_picture(pic);
    av_frame_free(&pic->f);
    av_frame_free(&pic->f_grain);
}

/**
 * Export the available multiview view IDs and validate the view_ids
 * option. Return 0, AVERROR(EINVAL) (requested view missing) or
 * AVERROR(ENOMEM).
 */
static int h264_mvc_export(H264Context *h)
{
    const H264MVCSPS *mvc = &h->mvc_sps->mvc;
    int i;

    av_freep(&h->view_ids_available);
    h->nb_view_ids_available = 0;

    // don't export anything in the trivial case (single view)
    if (mvc->num_views < 2)
        return 0;

    h->view_ids_available = av_calloc(mvc->num_views, sizeof(*h->view_ids_available));
    if (!h->view_ids_available)
        return AVERROR(ENOMEM);
    for (i = 0; i < mvc->num_views; i++)
        h->view_ids_available[i] = mvc->view_id[i];
    h->nb_view_ids_available = mvc->num_views;

    // H.264 MVC carries no view position (left/right) information in the
    // bitstream, so view_pos_available is not populated.

    // a single -1 means all views
    if (h->nb_view_ids == 1 && h->view_ids[0] == -1)
        return 0;

    // nothing requested: all views are selected by default, nothing to validate
    if (!h->nb_view_ids)
        return 0;

    for (i = 0; i < h->nb_view_ids; i++) {
        int id = h->view_ids[i], t, found = 0;

        if (id < 0) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "Invalid view ID requested: %d\n", id);
            return AVERROR(EINVAL);
        }
        for (t = 0; t < (int)mvc->num_views; t++)
            if ((int)mvc->view_id[t] == id)
                found = 1;
        if (!found) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "Requested view ID %d not present in the stream\n", id);
            return AVERROR(EINVAL);
        }
    }
    return 0;
}

/**
 * Register the multiview view list of the active SPS with the per-view
 * state; new views are appended in SPS order (the slot index must match
 * the SPS view_id array index, anchor reference indexing depends on it).
 *
 * @return 0 on success, AVERROR(EINVAL) on size or order inconsistency.
 */
static int h264_mvc_view_setup(H264Context *h)
{
    const H264MVCSPS *mvc = &h->mvc_sps->mvc;
    int i;

    if ((int)mvc->num_views > H264_MAX_MVC_VIEWS) {
        av_log(h->avctx, AV_LOG_ERROR,
               "too many views (%d), supported: %d\n",
               (int)mvc->num_views, H264_MAX_MVC_VIEWS);
        return AVERROR(EINVAL);
    }

    for (i = 0; i < (int)mvc->num_views; i++) {
        int slot;

        for (slot = 0; slot < h->view_count; slot++)
            if (h->views[slot].view_id == mvc->view_id[i])
                break;

        if (slot == h->view_count) {
            /* fresh view: start like the base view did */
            slot        = h->view_count++;
            h->views[slot].view_id = mvc->view_id[i];
            h->views[slot].poc.prev_poc_msb = 1 << 16;
            h->views[slot].poc.prev_frame_num = -1;
            h->views[slot].next_outputed_poc  = INT_MIN;
            for (int j = 0; j < FF_ARRAY_ELEMS(h->views[slot].last_pocs); j++)
                h->views[slot].last_pocs[j] = INT_MIN;
        }

        if (slot != i) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "multiview view %d not at expected position %d; "
                   "dependent view decoding may be unreliable\n",
                   (int)mvc->view_id[i], i);
            return AVERROR(EINVAL);
        }
    }
    return 0;
}

/**
 * Adopt the first SPS carrying multiview data; call after SPS parsing,
 * safe to call repeatedly. Return 0, or AVERROR(EINVAL)/AVERROR(ENOMEM)
 * if the requested view_ids cannot be satisfied.
 */
static int h264_mvc_update(H264Context *h)
{
    if (!h->mvc_sps) {
        int i;
        for (i = 0; i < H264_MAX_SPS_COUNT; i++)
            if (h->ps.sps_list[i] && h->ps.sps_list[i]->mvc.present) {
                int ret;
                h->mvc_sps = av_refstruct_ref_c(h->ps.sps_list[i]);
                ret = h264_mvc_export(h);
                if (ret < 0)
                    return ret;
                return h264_mvc_view_setup(h);
            }
    }
    return 0;
}

static av_cold int h264_decode_end(AVCodecContext *avctx)
{
    H264Context *h = avctx->priv_data;
    int i;

    ff_h264_remove_all_refs(h);
    ff_h264_free_tables(h);

    for (i = 0; i < H264_MAX_PICTURE_COUNT; i++) {
        h264_free_pic(h, &h->DPB[i]);
    }
    for (i = 0; i < H264_MAX_MVC_VIEWS; i++)
        memset(h->views[i].delayed_pic, 0, sizeof(h->views[i].delayed_pic));

    h->cur_pic_ptr = NULL;

    av_refstruct_pool_uninit(&h->decode_error_flags_pool);

    av_freep(&h->slice_ctx);
    h->nb_slice_ctx = 0;

    ff_h264_sei_uninit(&h->sei);
    ff_h264_ps_uninit(&h->ps);

    av_refstruct_unref(&h->mvc_sps);
    /* only the context that allocated the shared delivery watermark frees it;
     * other contexts hold NULL here, so the else-branch is a no-op for them */
    if (h->mvc_out_dts_owned) {
        av_freep(&h->mvc_out_dts);
        h->mvc_out_dts_owned = 0;
    } else
        h->mvc_out_dts = NULL;
    av_freep(&h->view_ids_available);
    h->nb_view_ids_available = 0;
    av_freep(&h->view_pos_available);
    h->nb_view_pos_available = 0;

    ff_h2645_packet_uninit(&h->pkt);

    h264_free_pic(h, &h->cur_pic);
    h264_free_pic(h, &h->last_pic_for_ec);

    return 0;
}

static AVOnce h264_vlc_init = AV_ONCE_INIT;

static av_cold int h264_decode_init(AVCodecContext *avctx)
{
    H264Context *h = avctx->priv_data;
    int ret;

    ret = h264_init_context(avctx, h);
    if (ret < 0)
        return ret;

    ret = ff_thread_once(&h264_vlc_init, ff_h264_decode_init_vlc);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "pthread_once has failed.");
        return AVERROR_UNKNOWN;
    }

    if (!avctx->internal->is_copy) {
        if (avctx->extradata_size > 0 && avctx->extradata) {
            ret = ff_h264_decode_extradata(avctx->extradata, avctx->extradata_size,
                                           &h->ps, &h->is_avc, &h->nal_length_size,
                                           avctx->err_recognition, avctx);
           if (ret < 0) {
                int explode = avctx->err_recognition & AV_EF_EXPLODE;
                av_log(avctx, explode ? AV_LOG_ERROR: AV_LOG_WARNING,
                       "Error decoding the extradata\n");
                if (explode) {
                    return ret;
                }
                ret = 0;
            }
        }

        // the global header may carry a multiview SPS
        ret = h264_mvc_update(h);
        if (ret < 0)
            return ret;
    }

    if (h->ps.sps && h->ps.sps->bitstream_restriction_flag &&
        h->avctx->has_b_frames < h->ps.sps->num_reorder_frames) {
        h->avctx->has_b_frames = h->ps.sps->num_reorder_frames;
    }

    ff_h264_flush_change(h);

    if (h->enable_er < 0 && (avctx->active_thread_type & FF_THREAD_SLICE))
        h->enable_er = 0;

    if (h->enable_er && (avctx->active_thread_type & FF_THREAD_SLICE)) {
        av_log(avctx, AV_LOG_WARNING,
               "Error resilience with slice threads is enabled. It is unsafe and unsupported and may crash. "
               "Use it at your own risk\n");
    }

    return 0;
}

/**
 * instantaneous decoder refresh (all registered views).
 */
static void idr(H264Context *h)
{
    ff_h264_remove_all_refs(h);
    for (int i = 0; i < h->view_count; i++) {
        H264ViewState *w = &h->views[i];
        w->poc.prev_frame_num        = 0;
        w->poc.prev_frame_num_offset = 0;
        w->poc.prev_poc_msb          = 1<<16;
        w->poc.prev_poc_lsb          = -1;
        for (int j = 0; j < FF_ARRAY_ELEMS(w->last_pocs); j++)
            w->last_pocs[j] = INT_MIN;
    }
}

/* forget old pics after a seek */
void ff_h264_flush_change(H264Context *h)
{
    int i, j;

    h->prev_interlaced_frame = 1;
    idr(h);

    for (i = 0; i < h->view_count; i++) {
        h->views[i].next_outputed_poc = INT_MIN;
        h->views[i].poc.prev_frame_num = -1;
        h->views[i].parked_pic = NULL;
    }
    if (h->cur_pic_ptr) {
        H264ViewState *v = &h->views[h->cur_pic_ptr->view_idx];
        h->cur_pic_ptr->reference = 0;
        for (j=i=0; v->delayed_pic[i]; i++)
            if (v->delayed_pic[i] != h->cur_pic_ptr)
                v->delayed_pic[j++] = v->delayed_pic[i];
        v->delayed_pic[j] = NULL;
    }
    ff_h264_unref_picture(&h->last_pic_for_ec);

    h->first_field = 0;
    h->recovery_frame = -1;
    h->frame_recovered = 0;
    h->current_slice = 0;
    h->mmco_reset = 1;
    h->last_in_dts = 0;
    h->last_out_dts = AV_NOPTS_VALUE;
    /* The shared delivery watermark is per decode session: reset the single
     * canonical instance (workers are parked during avcodec_flush_buffers,
     * so no claim can interleave). The frame-threaded user-facing context
     * has h->avctx NULL and never reaches here: ff_thread_flush() routes
     * the flush to the workers. */
    if (h->avctx) {
        H264Context *shared = ff_thread_shared_priv_data(h->avctx);

        if (shared && shared->mvc_out_dts)
            __atomic_store_n(shared->mvc_out_dts, AV_NOPTS_VALUE, __ATOMIC_RELAXED);
    }
}

static av_cold void h264_decode_flush(AVCodecContext *avctx)
{
    H264Context *h = avctx->priv_data;
    int i;

    for (i = 0; i < H264_MAX_MVC_VIEWS; i++) {
        memset(h->views[i].delayed_pic, 0, sizeof(h->views[i].delayed_pic));
        h->views[i].parked_pic = NULL;
    }

    ff_h264_flush_change(h);
    ff_h264_sei_uninit(&h->sei);

    for (i = 0; i < H264_MAX_PICTURE_COUNT; i++)
        ff_h264_unref_picture(&h->DPB[i]);
    h->cur_pic_ptr = NULL;
    ff_h264_unref_picture(&h->cur_pic);

    h->mb_y = 0;
    h->non_gray = 0;

    ff_h264_free_tables(h);
    h->context_initialized = 0;

    if (FF_HW_HAS_CB(avctx, flush))
        FF_HW_SIMPLE_CALL(avctx, flush);
}

static int get_last_needed_nal(H264Context *h)
{
    int nals_needed = 0;
    int slice_type = 0;
    int picture_intra_only = 1;
    int first_slice = 0;
    int i, ret;

    for (i = 0; i < h->pkt.nb_nals; i++) {
        H2645NAL *nal = &h->pkt.nals[i];
        GetBitContext gb;

        /* packets can sometimes contain multiple PPS/SPS,
         * e.g. two PAFF field pictures in one packet, or a demuxer
         * which splits NALs strangely if so, when frame threading we
         * can't start the next thread until we've read all of them */
        switch (nal->type) {
        case H264_NAL_SPS:
        case H264_NAL_SUB_SPS:
        case H264_NAL_PPS:
            nals_needed = i;
            break;
        case H264_NAL_DPA:
        case H264_NAL_IDR_SLICE:
        case H264_NAL_SLICE:
        case H264_NAL_AUXILIARY_SLICE:
        case H264_NAL_EXTEN_SLICE:
        case H264_NAL_DEPTH_EXTEN_SLICE:
        case H264_NAL_RESERVED22:
        case H264_NAL_RESERVED23: {
            /* slice extension NAL units (7.3.1) carry a 24-bit
             * multiview extension after the 8-bit NAL header */
            int ext = nal->type >= H264_NAL_AUXILIARY_SLICE &&
                      nal->type <= H264_NAL_RESERVED23;

            /* header (plus the 3-byte extension) must fit before the
             * payload, else init_get_bits8 gets a negative size */
            if (nal->size < 1 + 3 * ext) {
                av_log(h->avctx, AV_LOG_ERROR,
                       "Invalid undersized VCL NAL unit\n");
                if (h->avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
                break;
            }
            ret = init_get_bits8(&gb, nal->data + 1 + 3 * ext,
                                 nal->size - 1 - 3 * ext);
            if (ret < 0) {
                av_log(h->avctx, AV_LOG_ERROR, "Invalid zero-sized VCL NAL unit\n");
                if (h->avctx->err_recognition & AV_EF_EXPLODE)
                    return ret;

                break;
            }
            if (!get_ue_golomb_long(&gb) ||  // first_mb_in_slice
                !first_slice ||
                first_slice != nal->type)
                nals_needed = i;
            slice_type = get_ue_golomb_31(&gb);
            if (slice_type > 9)
                slice_type = 0;
            if (slice_type > 4)
                slice_type -= 5;

            slice_type = ff_h264_golomb_to_pict_type[slice_type];
            picture_intra_only &= (slice_type & 3) == AV_PICTURE_TYPE_I;
            if (!first_slice)
                first_slice = nal->type;
        }
        }
    }

    h->picture_intra_only = picture_intra_only;

    return nals_needed;
}

static void debug_green_metadata(const H264SEIGreenMetaData *gm, void *logctx)
{
    av_log(logctx, AV_LOG_DEBUG, "Green Metadata Info SEI message\n");
    av_log(logctx, AV_LOG_DEBUG, "  green_metadata_type: %d\n", gm->green_metadata_type);

    if (gm->green_metadata_type == 0) {
        av_log(logctx, AV_LOG_DEBUG, "  green_metadata_period_type: %d\n", gm->period_type);

        if (gm->period_type == 2)
            av_log(logctx, AV_LOG_DEBUG, "  green_metadata_num_seconds: %d\n", gm->num_seconds);
        else if (gm->period_type == 3)
            av_log(logctx, AV_LOG_DEBUG, "  green_metadata_num_pictures: %d\n", gm->num_pictures);

        av_log(logctx, AV_LOG_DEBUG, "  SEI GREEN Complexity Metrics: %f %f %f %f\n",
               (float)gm->percent_non_zero_macroblocks/255,
               (float)gm->percent_intra_coded_macroblocks/255,
               (float)gm->percent_six_tap_filtering/255,
               (float)gm->percent_alpha_point_deblocking_instance/255);

    } else if (gm->green_metadata_type == 1) {
        av_log(logctx, AV_LOG_DEBUG, "  xsd_metric_type: %d\n", gm->xsd_metric_type);

        if (gm->xsd_metric_type == 0)
            av_log(logctx, AV_LOG_DEBUG, "  xsd_metric_value: %f\n",
                   (float)gm->xsd_metric_value/100);
    }
}

/* The pixel format is fully determined by the SPS. Publish it as soon as
 * the multiview SPS is known, so a delta-only stream (all pictures
 * dropped) still exposes its format to stream probing instead of
 * failing with "unspecified pixel format". */
static void h264_publish_default_pix_fmt(H264Context *h)
{
    const SPS *sps = h->mvc_sps;
    enum AVPixelFormat fmt;

    if (!sps || h->avctx->pix_fmt != AV_PIX_FMT_NONE)
        return;

    switch (sps->bit_depth_luma) {
    case 8:
        fmt = sps->chroma_format_idc == 2 ? AV_PIX_FMT_YUV444P :
              sps->chroma_format_idc == 1 ? AV_PIX_FMT_YUV422P :
                                            AV_PIX_FMT_YUV420P;
        break;
    case 9:
        fmt = sps->chroma_format_idc == 2 ? AV_PIX_FMT_YUV444P9 :
              sps->chroma_format_idc == 1 ? AV_PIX_FMT_YUV422P9 :
                                            AV_PIX_FMT_YUV420P9;
        break;
    case 10:
        fmt = sps->chroma_format_idc == 2 ? AV_PIX_FMT_YUV444P10 :
              sps->chroma_format_idc == 1 ? AV_PIX_FMT_YUV422P10 :
                                            AV_PIX_FMT_YUV420P10;
        break;
    case 12:
        fmt = sps->chroma_format_idc == 2 ? AV_PIX_FMT_YUV444P12 :
              sps->chroma_format_idc == 1 ? AV_PIX_FMT_YUV422P12 :
                                            AV_PIX_FMT_YUV420P12;
        break;
    case 14:
        fmt = sps->chroma_format_idc == 2 ? AV_PIX_FMT_YUV444P14 :
              sps->chroma_format_idc == 1 ? AV_PIX_FMT_YUV422P14 :
                                            AV_PIX_FMT_YUV420P14;
        break;
    default:
        return;
    }
    h->avctx->pix_fmt = fmt;
}

/**
 * Whether the picture named by @p nal belongs to a delta-only view (its
 * SPS-declared inter-view references are absent from the stream); such
 * pictures cannot be reconstructed and are dropped before the slice
 * header parse. Depends only on the NAL header, the active SPS and the
 * per-view DPB state, so it is safe to call before the slice header.
 */
static int mvc_picture_missing_view(const H264Context *h, const H2645NAL *nal)
{
    /* h->ps.sps is only set while a slice header is parsed, so it is NULL
     * in the NAL loop of the first access unit; the adopted multiview SPS
     * is known as soon as the SPS NAL itself was parsed. */
    const SPS *sps = h->mvc_sps ? h->mvc_sps : h->ps.sps;

    if (!sps || !sps->mvc.present)
        return 0;

    /* Plain (non-extended) slice NALs belong to the base view (slot 0),
     * which lists no inter-view references. */
    if (nal->type < H264_NAL_AUXILIARY_SLICE || nal->type > H264_NAL_RESERVED23)
        return 0;

    int slot = 0;
    for (int i = 0; i < h->view_count; i++)
        if (h->views[i].view_id == nal->mv_view_id) {
            slot = i;
            break;
        }
    if (slot == 0)
        return 0;

    const H264MVCSPS *mvc = &sps->mvc;
    const int anchor = nal->mv_ext_parsed && nal->mv_anchor_pic;
    const uint8_t (*refv[2])[H264_MAX_MVC_VIEWS] = {
        anchor ? mvc->anchor_refs_l0  : mvc->non_anchor_refs_l0,
        anchor ? mvc->anchor_refs_l1  : mvc->non_anchor_refs_l1,
    };
    const int n[2] = {
        anchor ? mvc->num_anchor_refs_l0[slot]  : mvc->num_non_anchor_refs_l0[slot],
        anchor ? mvc->num_anchor_refs_l1[slot]  : mvc->num_non_anchor_refs_l1[slot],
    };

    for (int list = 0; list < 2; list++)
        for (int k = 0; k < n[list]; k++) {
            int r = refv[list][slot][k];
            if (r >= mvc->num_views ||
                (!h->views[r].short_ref_count && !h->views[r].long_ref_count &&
                 !h->views[r].delayed_pic[0] && !h->views[r].parked_pic))
                return 1;
        }
    return 0;
}

/**
 * Attach the partitions B and C immediately following the partition A at idx.
 * Arbitrary slice order may separate them (7.4.1.2.5); that is not supported.
 *
 * @return index of the last NAL absorbed, or idx if there were none.
 */
static int h264_attach_partitions(const H264Context *h, H264SliceContext *sl,
                                  int idx)
{
    while (idx + 1 < h->pkt.nb_nals) {
        const H2645NAL *nal = &h->pkt.nals[idx + 1];

        if (nal->type != H264_NAL_DPB && nal->type != H264_NAL_DPC)
            break;
        if (ff_h264_attach_slice_partition(h, sl, nal) < 0)
            break;
        idx++;
    }

    return idx;
}

static int decode_nal_units(H264Context *h, AVBufferRef *buf_ref,
                            const uint8_t *buf, int buf_size)
{
    AVCodecContext *const avctx = h->avctx;
    int nals_needed = 0; ///< number of NALs that need decoding before the next frame thread starts
    int idr_cleared=0;
    int dp_attached_to = -1; ///< index of the last partition B/C claimed
    int i, ret = 0;

    h->has_slice = 0;
    h->drop_view_slices = 0;
    h->view_sel_skipped = 0;
    h->nal_unit_type= 0;

    if (!(avctx->flags2 & AV_CODEC_FLAG2_CHUNKS)) {
        h->current_slice = 0;
        if (!h->first_field) {
            h->cur_pic_ptr = NULL;
            ff_h264_sei_uninit(&h->sei);
        }
    }

    if (h->nal_length_size == 4) {
        if (buf_size > 8 && AV_RB32(buf) == 1 && AV_RB32(buf+5) > (unsigned)buf_size) {
            h->is_avc = 0;
        }else if(buf_size > 3 && AV_RB32(buf) > 1 && AV_RB32(buf) <= (unsigned)buf_size)
            h->is_avc = 1;
    }

    ret = ff_h2645_packet_split(&h->pkt, buf, buf_size, avctx, h->nal_length_size,
                                avctx->codec_id, !!h->is_avc * H2645_FLAG_IS_NALFF);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Error splitting the input into NAL units.\n");
        return ret;
    }

    if (avctx->active_thread_type & FF_THREAD_FRAME)
        nals_needed = get_last_needed_nal(h);
    if (nals_needed < 0)
        return nals_needed;

    for (i = 0; i < h->pkt.nb_nals; i++) {
        H2645NAL *nal = &h->pkt.nals[i];
        H264SliceContext *queued;
        int max_slice_ctx, err;

        if (avctx->skip_frame >= AVDISCARD_NONREF &&
            nal->ref_idc == 0 && nal->type != H264_NAL_SEI)
            continue;

        // FIXME these should stop being context-global variables
        h->nal_ref_idc   = nal->ref_idc;
        h->nal_unit_type = nal->type;

        err = 0;
        switch (nal->type) {
        case H264_NAL_IDR_SLICE:
            if ((nal->data[1] & 0xFC) == 0x98) {
                av_log(h->avctx, AV_LOG_ERROR, "Invalid inter IDR frame\n");
                h->views[0].next_outputed_poc = INT_MIN;
                ret = -1;
                goto end;
            }
            if(!idr_cleared) {
                idr(h); // FIXME ensure we don't lose some frames if there is reordering
            }
            idr_cleared = 1;
            h->has_recovery_point = 1;
            av_fallthrough;
        case H264_NAL_SLICE:
        case H264_NAL_DPA:
        case H264_NAL_AUXILIARY_SLICE:
        case H264_NAL_EXTEN_SLICE:
        case H264_NAL_DEPTH_EXTEN_SLICE:
        case H264_NAL_RESERVED22:
        case H264_NAL_RESERVED23:
            /* a slice extension NAL without a parsed multiview header
             * (e.g. SVC) is unsupported; skip its payload */
            if (nal->type >= H264_NAL_AUXILIARY_SLICE &&
                nal->type <= H264_NAL_RESERVED23 && !nal->mv_ext_parsed) {
                break;
            }

            if (h->drop_view_slices || mvc_picture_missing_view(h, nal)) {
                if (!h->drop_view_slices) {
                    int slot = 0;
                    for (int i = 0; i < h->view_count; i++)
                        if (h->views[i].view_id == nal->mv_view_id) {
                            slot = i;
                            break;
                        }
                    h->drop_view_slices = 1;
                    if (!h->mvc_missing_view_warned) {
                        h->mvc_missing_view_warned = 1;
                        av_log(h->avctx, AV_LOG_WARNING,
                               "MVC: view %d references view(s) missing from the "
                               "stream; pictures of this view are dropped\n",
                               (int)h->views[slot].view_id);
                    }
                }
                break;
            }

            h->has_slice = 1;

            if (nal->type == H264_NAL_DPA) {
                /* partitioned streams all come from JM or a derivative */
                if (h->workaround_bugs & FF_BUG_AUTODETECT)
                    h->workaround_bugs |= FF_BUG_H264_DP_NNZ;

                /* hwaccels take one self-contained slice NAL, not three */
                if (avctx->hwaccel) {
                    avpriv_request_sample(avctx, "hardware accelerated data partitioning");
                    ret = AVERROR_PATCHWELCOME;
                    goto end;
                }
                /* the lookahead needs all three partitions in one packet */
                if (avctx->flags2 & AV_CODEC_FLAG2_CHUNKS) {
                    av_log(avctx, AV_LOG_ERROR, "Decoding in chunks is not "
                           "supported for partitioned slices\n");
                    ret = AVERROR(ENOSYS);
                    goto end;
                }
            }

            if ((err = ff_h264_queue_decode_slice(h, nal, &queued))) {
                H264SliceContext *sl = h->slice_ctx + h->nb_slice_ctx_queued;
                sl->ref_count[0] = sl->ref_count[1] = 0;
                break;
            }

            if (nal->type == H264_NAL_DPA && queued)
                dp_attached_to = h264_attach_partitions(h, queued, i);

            if (h->current_slice == 1) {
                if (avctx->active_thread_type & FF_THREAD_FRAME &&
                    i >= nals_needed && !h->setup_finished && h->cur_pic_ptr &&
                    /* Multiview: setup completes only once a non-base view's
                     * picture has started, so its frame_start() runs in SETUP */
                    (h->view_count <= 1 || h->cur_view > 0)) {
                    ff_thread_finish_setup(avctx);
                    h->setup_finished = 1;
                }

                if (h->avctx->hwaccel &&
                    (ret = FF_HW_CALL(h->avctx, start_frame, buf_ref,
                                      buf, buf_size)) < 0)
                    goto end;
            }

            max_slice_ctx = avctx->hwaccel ? 1 : h->nb_slice_ctx;
            if (h->nb_slice_ctx_queued == max_slice_ctx) {
                if (h->avctx->hwaccel) {
                    ret = FF_HW_CALL(avctx, decode_slice, nal->raw_data, nal->raw_size);
                    h->nb_slice_ctx_queued = 0;
                } else
                    ret = ff_h264_execute_decode_slices(h);
                if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
                    goto end;
            }
            break;
        case H264_NAL_DPB:
        case H264_NAL_DPC:
            /* not claimed by the lookahead above, so it has no partition A */
            if (i > dp_attached_to)
                av_log(avctx, AV_LOG_WARNING, "Ignoring slice data partition "
                       "%c without a matching partition A\n",
                       nal->type == H264_NAL_DPB ? 'B' : 'C');
            break;
        case H264_NAL_SEI:
            if (h->setup_finished) {
                avpriv_request_sample(avctx, "Late SEI");
                break;
            }
            ret = ff_h264_sei_decode(&h->sei, &nal->gb, &h->ps, avctx);
            h->has_recovery_point = h->has_recovery_point || h->sei.recovery_point.recovery_frame_cnt != -1;
            if (avctx->debug & FF_DEBUG_GREEN_MD)
                debug_green_metadata(&h->sei.green_metadata, h->avctx);
            if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
                goto end;
            break;
        case H264_NAL_SPS:
        case H264_NAL_SUB_SPS: {
            GetBitContext tmp_gb = nal->gb;
            // only plain SPS units are known to the hwaccel SPS callbacks;
            // subset SPS multiview data is handled in software
            if (nal->type == H264_NAL_SPS && FF_HW_HAS_CB(avctx, decode_params)) {
                ret = FF_HW_CALL(avctx, decode_params,
                                 nal->type, nal->raw_data, nal->raw_size);
                if (ret < 0)
                    goto end;
            }
            if (ff_h264_decode_seq_parameter_set(&tmp_gb, avctx, &h->ps, 0) >= 0) {
                ret = h264_mvc_update(h);
                if (ret < 0)
                    goto end;
                h264_publish_default_pix_fmt(h);
                break;
            }
            av_log(h->avctx, AV_LOG_DEBUG,
                   "SPS decoding failure, trying again with the complete NAL\n");
            init_get_bits8(&tmp_gb, nal->raw_data + 1, nal->raw_size - 1);
            if (ff_h264_decode_seq_parameter_set(&tmp_gb, avctx, &h->ps, 0) >= 0) {
                ret = h264_mvc_update(h);
                if (ret < 0)
                    goto end;
                h264_publish_default_pix_fmt(h);
                break;
            }
            // last-resort lenient re-parse (overread tail warns instead of
            // failing); on success propagate the multiview SPS state like
            // the strict attempts - the lenient flag only relaxes the
            // trailing overread check, which runs after the mvc extension parse
            if (ff_h264_decode_seq_parameter_set(&nal->gb, avctx, &h->ps, 1) >= 0) {
                ret = h264_mvc_update(h);
                if (ret < 0)
                    goto end;
                h264_publish_default_pix_fmt(h);
            }
            break;
        }
        case H264_NAL_PPS:
            if (FF_HW_HAS_CB(avctx, decode_params)) {
                ret = FF_HW_CALL(avctx, decode_params,
                                 nal->type, nal->raw_data, nal->raw_size);
                if (ret < 0)
                    goto end;
            }
            ret = ff_h264_decode_picture_parameter_set(&nal->gb, avctx, &h->ps,
                                                       nal->size_bits);
            if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
                goto end;
            break;
        case H264_NAL_AUD:
        case H264_NAL_END_SEQUENCE:
        case H264_NAL_END_STREAM:
        case H264_NAL_FILLER_DATA:
        case H264_NAL_SPS_EXT:
        case H264_NAL_UNSPECIFIED24:
            /* the affected multiview elementary streams emit an
             * unspecified-24 NAL at each access unit start; no payload */
            break;
        default:
            av_log(avctx, AV_LOG_DEBUG, "Unknown NAL code: %d (%d bits)\n",
                   nal->type, nal->size_bits);
        }

        if (err < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE)) {
            av_log(h->avctx, AV_LOG_ERROR, "decode_slice_header error\n");
            ret = err;
            goto end;
        }
    }

    ret = ff_h264_execute_decode_slices(h);
    if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
        goto end;

    /* A truncated 2D+delta dependent slice may leave a run of macroblocks
     * undecoded yet unflagged; complete it before the error flags below. */
    ff_h264_complete_truncated_region(h);

    // set decode_error_flags to allow users to detect concealed decoding errors
    if ((ret < 0 || h->er.error_occurred) && h->cur_pic_ptr) {
        if (h->cur_pic_ptr->decode_error_flags) {
            /* Frame-threading in use */
            atomic_int *decode_error = h->cur_pic_ptr->decode_error_flags;
            /* Using atomics here is not supposed to provide synchronisation;
             * they are merely used to allow to set decode_error from both
             * decoding threads in case of coded slices. */
            atomic_fetch_or_explicit(decode_error, FF_DECODE_ERROR_DECODE_SLICES,
                                     memory_order_relaxed);
        } else
            h->cur_pic_ptr->f->decode_error_flags |= FF_DECODE_ERROR_DECODE_SLICES;
    }

    ret = 0;
end:

#if CONFIG_ERROR_RESILIENCE
    /*
     * FIXME: Error handling code does not seem to support interlaced
     * when slices span multiple rows
     * The ff_er_add_slice calls don't work right for bottom
     * fields; they cause massive erroneous error concealing
     * Error marking covers both fields (top and bottom).
     * This causes a mismatched s->error_count
     * and a bad error table. Further, the error count goes to
     * INT_MAX when called for bottom field, because mb_y is
     * past end by one (callers fault) and resync_mb_y != 0
     * causes problems for the first MB line, too.
     */
    if (!FIELD_PICTURE(h) && h->current_slice && h->enable_er &&
        !ff_h264_skip_all_pixels(h->avctx)) {

        H264SliceContext *sl = h->slice_ctx;
        int use_last_pic = h->last_pic_for_ec.f->buf[0] && !sl->ref_count[0];
        int decode_error_flags = 0;

        ff_h264_set_erpic(&h->er.cur_pic, h->cur_pic_ptr);

        if (use_last_pic) {
            ff_h264_set_erpic(&h->er.last_pic, &h->last_pic_for_ec);
            sl->ref_list[0][0].parent = &h->last_pic_for_ec;
            memcpy(sl->ref_list[0][0].data, h->last_pic_for_ec.f->data, sizeof(sl->ref_list[0][0].data));
            memcpy(sl->ref_list[0][0].linesize, h->last_pic_for_ec.f->linesize, sizeof(sl->ref_list[0][0].linesize));
            sl->ref_list[0][0].reference = h->last_pic_for_ec.reference;
        } else if (sl->ref_count[0]) {
            ff_h264_set_erpic(&h->er.last_pic, sl->ref_list[0][0].parent);
        } else
            ff_h264_set_erpic(&h->er.last_pic, NULL);

        if (sl->ref_count[1])
            ff_h264_set_erpic(&h->er.next_pic, sl->ref_list[1][0].parent);

        ff_er_frame_end(&h->er, &decode_error_flags);
        if (decode_error_flags) {
            if (h->cur_pic_ptr->decode_error_flags) {
                atomic_int *decode_error = h->cur_pic_ptr->decode_error_flags;
                atomic_fetch_or_explicit(decode_error, decode_error_flags,
                                         memory_order_relaxed);
            } else
                h->cur_pic_ptr->f->decode_error_flags |= decode_error_flags;
        }
        if (use_last_pic)
            memset(&sl->ref_list[0][0], 0, sizeof(sl->ref_list[0][0]));
    }
#endif /* CONFIG_ERROR_RESILIENCE */
    /* clean up */
    if (h->cur_pic_ptr && !h->droppable && h->has_slice) {
        ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX,
                                  h->picture_structure == PICT_BOTTOM_FIELD);
    }

    return (ret < 0) ? ret : buf_size;
}

static int h264_export_enc_params(AVFrame *f, const H264Picture *p)
{
    AVVideoEncParams *par;
    unsigned int nb_mb = p->mb_height * p->mb_width;
    unsigned int x, y;

    par = av_video_enc_params_create_side_data(f, AV_VIDEO_ENC_PARAMS_H264, nb_mb);
    if (!par)
        return AVERROR(ENOMEM);

    par->qp = p->pps->init_qp;

    par->delta_qp[1][0] = p->pps->chroma_qp_index_offset[0];
    par->delta_qp[1][1] = p->pps->chroma_qp_index_offset[0];
    par->delta_qp[2][0] = p->pps->chroma_qp_index_offset[1];
    par->delta_qp[2][1] = p->pps->chroma_qp_index_offset[1];

    for (y = 0; y < p->mb_height; y++)
        for (x = 0; x < p->mb_width; x++) {
            const unsigned int block_idx = y * p->mb_width + x;
            const unsigned int     mb_xy = y * p->mb_stride + x;
            AVVideoBlockParams *b = av_video_enc_params_block(par, block_idx);

            b->src_x = x * 16;
            b->src_y = y * 16;
            b->w     = 16;
            b->h     = 16;

            b->delta_qp = p->qscale_table[mb_xy] - par->qp;
        }

    return 0;
}

static int output_frame(H264Context *h, AVFrame *dst, H264Picture *srcp)
{
    int ret;

    ret = av_frame_ref(dst, srcp->needs_fg ? srcp->f_grain : srcp->f);
    if (ret < 0)
        return ret;

    if (srcp->needs_fg && (ret = av_frame_copy_props(dst, srcp->f)) < 0)
        return ret;

    if (srcp->decode_error_flags) {
        atomic_int *decode_error = srcp->decode_error_flags;
        /* The following is not supposed to provide synchronisation at all:
         * given that srcp has already finished decoding, decode_error
         * has already been set to its final value. */
        dst->decode_error_flags |= atomic_load_explicit(decode_error, memory_order_relaxed);
    }

    av_dict_set(&dst->metadata, "stereo_mode", ff_h264_sei_stereo_mode(&h->sei.common.frame_packing), 0);

    /* tag multiview frames with the stream-wide view id so the views
     * of one access unit can be told apart by the consumer */
    if (h->view_count > 1)
        av_dict_set_int(&dst->metadata, "view_id", srcp->view_id, 0);

    if (srcp->sei_recovery_frame_cnt == 0)
        dst->flags |= AV_FRAME_FLAG_KEY;

    if (h->avctx->export_side_data & AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS) {
        ret = h264_export_enc_params(dst, srcp);
        if (ret < 0)
            goto fail;
    }

    if (!(h->avctx->export_side_data & AV_CODEC_EXPORT_DATA_FILM_GRAIN))
        av_frame_remove_side_data(dst, AV_FRAME_DATA_FILM_GRAIN_PARAMS);

    return 0;
fail:
    av_frame_unref(dst);
    return ret;
}

static int is_avcc_extradata(const uint8_t *buf, int buf_size)
{
    int cnt= buf[5]&0x1f;
    const uint8_t *p= buf+6;
    if (!cnt)
        return 0;
    while(cnt--){
        int nalsize= AV_RB16(p) + 2;
        if(nalsize > buf_size - (p-buf) || (p[2] & 0x9F) != 7)
            return 0;
        p += nalsize;
    }
    cnt = *(p++);
    if(!cnt)
        return 0;
    while(cnt--){
        int nalsize= AV_RB16(p) + 2;
        if(nalsize > buf_size - (p-buf) || (p[2] & 0x9F) != 8)
            return 0;
        p += nalsize;
    }
    return 1;
}

/* The shared multiview delivery watermark. Canonically stored in the
 * first decoding context's H264Context (ff_thread_shared_priv_data) and
 * never copied by ff_h264_update_thread_context(), so every context
 * resolves the same slot. Allocated on first use (CAS, the loser drops
 * its copy); starts at AV_NOPTS_VALUE so the first claim keeps its
 * candidate unchanged. */
static int64_t *mvc_out_dts_instance(H264Context *h)
{
    H264Context *shared = ff_thread_shared_priv_data(h->avctx);
    int64_t *expect = NULL;

    if (!shared)
        return NULL;
    if (!shared->mvc_out_dts) {
        int64_t *wm = av_mallocz(sizeof(*wm));

        if (!wm)
            return NULL;
        *wm = AV_NOPTS_VALUE;
        if (!__atomic_compare_exchange_n(&shared->mvc_out_dts, &expect, wm, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            av_free(wm);
        } else {
            h->mvc_out_dts = wm;
            h->mvc_out_dts_owned = 1;
        }
    }
    return shared->mvc_out_dts;
}

/* Claim the next delivery pkt_dts from the shared watermark: the result
 * is strictly greater than every value claimed before it (a candidate at
 * or below the watermark is bumped to watermark + 1). Called from the
 * main thread at delivery time, so claims follow the delivery order. */
static int64_t mvc_out_dts_claim(H264Context *h, int64_t candidate)
{
    int64_t *wm = mvc_out_dts_instance(h);

    if (!wm)
        return candidate;
    for (;;) {
        int64_t cur  = __atomic_load_n(wm, __ATOMIC_RELAXED);
        int64_t next = (cur == AV_NOPTS_VALUE || candidate > cur) ?
                       candidate : cur + 1;
        if (__atomic_compare_exchange_n(wm, &cur, next, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return next;
    }
}

/* Delivery-order claim for the shared multiview watermark, wired as the
 * FFCodec post_receive_frame callback (main thread, once per delivered
 * frame, before best_effort_timestamp is guessed). finalize_frame() only
 * monotonizes against the decoding worker's own deliveries, but under
 * frame threading the delivery order is not the decode order; claiming
 * here instead of at commit time makes every delivered pkt_dts strictly
 * increasing in delivery order. It can only raise the per-worker value,
 * so single-thread decodes stay bit-identical. No-op for non-multiview
 * streams or invalid dts. The shared instance is resolved with
 * ff_thread_shared_priv_data(): the frame-threaded user-facing context
 * has no initialized H264Context. */
static void h264_post_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    H264Context *shared;
    int64_t dts;

    shared = ff_thread_shared_priv_data(avctx);
    if (!shared || shared->view_count <= 1)
        return;
    dts = frame->pkt_dts;
    if (dts < 0)
        return;
    frame->pkt_dts = mvc_out_dts_claim(shared, dts);
}

static int finalize_frame(H264Context *h, AVFrame *dst, H264Picture *out, int *got_frame)
{
    int ret;

    if (((h->avctx->flags & AV_CODEC_FLAG_OUTPUT_CORRUPT) ||
         (h->avctx->flags2 & AV_CODEC_FLAG2_SHOW_ALL) ||
         out->recovered)) {

        if (h->skip_gray > 0 &&
            h->non_gray && out->gray &&
            !(h->avctx->flags2 & AV_CODEC_FLAG2_SHOW_ALL)
        )
            return 0;

        if (!h->avctx->hwaccel && !ff_h264_skip_all_pixels(h->avctx) &&
            (out->field_poc[0] == INT_MAX ||
             out->field_poc[1] == INT_MAX)
           ) {
            int p;
            AVFrame *f = out->f;
            int field = out->field_poc[0] == INT_MAX;
            uint8_t *dst_data[4];
            int linesizes[4];
            const uint8_t *src_data[4];

            av_log(h->avctx, AV_LOG_DEBUG, "Duplicating field %d to fill missing\n", field);

            for (p = 0; p<4; p++) {
                dst_data[p] = f->data[p] + (field^1)*f->linesize[p];
                src_data[p] = f->data[p] +  field   *f->linesize[p];
                linesizes[p] = 2*f->linesize[p];
            }

            av_image_copy(dst_data, linesizes, src_data, linesizes,
                          f->format, f->width, f->height>>1);
        }

        /* The decoder owns the delivered frame's pkt_dts
         * (FF_CODEC_CAP_SETS_PKT_DTS keeps decode.c from overwriting
         * it), so the single-view case must reproduce decode.c's
         * baseline exactly: the dts of the packet being decoded
         * (h->pkt_dts, latched in h264_decode_frame), verbatim -
         * AV_NOPTS_VALUE for end-of-stream flush frames.
         *
         * Multiview: the two views of one access unit share the access
         * unit's dts (and the pts fallback), so a per-packet dts alone
         * would make the muxer drop the second view. Monotonize the
         * delivered pkt_dts - AV_NOPTS_VALUE and the negative values
         * seen after an input seek included - so every delivered frame
         * strictly advances last_out_dts. Rewrite an unset pkt_dts
         * only when the frame carries a real pts: on untimed streams a
         * fabricated dts would leak into the pts through the
         * pts-from-pkt_dts fallback and collapse the frame timing.
         *
         * Known limitation: the "non monotonically increasing dts"
         * lines a two-view mux can still print are structural: the
         * ffmpeg output path never consults frame->pkt_dts, so the
         * muxed packet dts is derived from the frame pts and the
         * demuxer dts estimate and clamped against last_mux_dts, and
         * the two views share the access unit's pts by construction.
         * Fixing it would mean view-aware dts derivation in fftools,
         * out of scope here.
         *
         * This per-worker pass only orders a frame against the
         * deliveries of the worker that decodes it; under frame
         * threading the delivery order is not the decode order, so
         * h264_post_receive_frame() completes the delivery-order
         * guarantee by claiming the shared watermark on the main
         * thread. It can only raise the value produced here, so
         * single-thread decodes stay bit-identical. */
        if (h->view_count > 1) {
            if (out->f->pts != AV_NOPTS_VALUE &&
                out->f->pkt_dts == AV_NOPTS_VALUE) {
                if (h->last_out_dts != AV_NOPTS_VALUE)
                    out->f->pkt_dts = h->last_out_dts + 1;
                else
                    out->f->pkt_dts = out->f->pts - FFMAX(out->f->duration, 1);
            }
            if (h->last_out_dts != AV_NOPTS_VALUE &&
                out->f->pkt_dts != AV_NOPTS_VALUE &&
                out->f->pkt_dts <= h->last_out_dts)
                out->f->pkt_dts = h->last_out_dts + 1;
            if (out->f->pkt_dts != AV_NOPTS_VALUE)
                h->last_out_dts = out->f->pkt_dts;
        } else {
            out->f->pkt_dts = h->pkt_dts;
        }

        ret = output_frame(h, dst, out);
        if (ret < 0)
            return ret;

        *got_frame = 1;

        if (CONFIG_MPEGVIDEODEC) {
            ff_print_debug_info2(h->avctx, dst,
                                 out->mb_type,
                                 out->qscale_table,
                                 out->motion_val,
                                 out->mb_width, out->mb_height, out->mb_stride, 1);
        }
    }

    return 0;
}

static int h264_sbs_process(H264Context *h, AVFrame *pict,
                            H264Picture *out, int *got_frame);

static int send_next_delayed_frame(H264Context *h, AVFrame *dst_frame,
                                   int *got_frame, int buf_index)
{
    int ret, vs, i, out_idx = 0, out_vs = 0;
    H264Picture *out;

    h->cur_pic_ptr = NULL;
    h->first_field = 0;

    /* In multiview streams the delayed lists of all views may hold
     * pictures; emit the global lowest-POC one each call (base view
     * first on ties) so the views interleave in display order. */
    for (;;) {
        out = NULL;

        for (vs = 0; vs < h->view_count; vs++) {
            H264ViewState *v = &h->views[vs];
            H264Picture *cand = v->delayed_pic[0];
            int cand_idx = 0;

            if (!h264_view_selected(h, vs) || !cand)
                continue;
            if (h->view_count > 1) {
                /* A queued picture can be an already-delivered stale
                 * alias (this context's views[] copy predates another
                 * worker delivering it); compact so only undelivered
                 * pictures remain. Single pass: terminates even if a
                 * slot is queued twice. Log once. */
                int kept = 0;

                for (i = 0;
                     i < FF_ARRAY_ELEMS(v->delayed_pic) && v->delayed_pic[i];
                     i++) {
                    H264Picture *p = v->delayed_pic[i];

                    if (p->output_delivered) {
                        static int delivered_queue_logged;

                        if (!delivered_queue_logged) {
                            delivered_queue_logged = 1;
                            av_log(h->avctx, AV_LOG_INFO,
                                   "multiview view %d: dropped a stale "
                                   "queue entry for an already delivered "
                                   "picture (poc %d)\n",
                                   vs, p->poc);
                        }
                    } else {
                        if (kept != i)
                            v->delayed_pic[kept] = p;
                        kept++;
                    }
                }
                if (i < FF_ARRAY_ELEMS(v->delayed_pic)) {
                    if (kept < i || kept == 0)
                        v->delayed_pic[kept] = NULL;
                } else {
                    /* No terminator: the list is corrupt; reset it so
                     * the flush can proceed on known memory. */
                    av_log(h->avctx, AV_LOG_WARNING,
                           "multiview view %d: delayed picture queue lost "
                           "its terminator while flushing; resetting it\n",
                           vs);
                    for (int k = 0; k < FF_ARRAY_ELEMS(v->delayed_pic); k++)
                        v->delayed_pic[k] = NULL;
                    kept = 0;
                }
                cand = v->delayed_pic[0];
                if (!cand)
                    continue;
            }
            cand_idx = ff_h264_mv_queue_pick(v);
            cand     = v->delayed_pic[cand_idx];
            if (!out || cand->poc < out->poc ||
                (cand->poc == out->poc && vs < out_vs)) {
                out     = cand;
                out_vs  = vs;
                out_idx = cand_idx;
            }
        }
        if (!out)
            break;

        for (i = out_idx; h->views[out_vs].delayed_pic[i]; i++)
            h->views[out_vs].delayed_pic[i] = h->views[out_vs].delayed_pic[i + 1];

        h->frame_recovered |= out->recovered;
        out->recovered |= h->frame_recovered & FRAME_RECOVERED_SEI;

        out->reference &= ~DELAYED_PIC_REF;
        ret = finalize_frame(h, dst_frame, out, got_frame);
        if (ret < 0)
            return ret;
        if (*got_frame) {
            int sbs = h264_sbs_process(h, dst_frame, out, got_frame);
            if (sbs < 0)
                return sbs;
            out->output_delivered = 1;
            if (*got_frame)
                break;
            /* a held base half (SBS): got_frame was cleared, so keep
             * draining this view's queue; the held base is assembled from
             * the dependent half's LATER packet, not from this queue */
        }
    }

    return buf_index;
}

/* ------------------------------------------------------------------------
 * Native side-by-side (SBS) output for 2-view H.264/MVC.
 *
 * When a 2-view MVC stream is decoded with both views selected (the
 * default), the two views of one access unit are assembled into one
 * side-by-side frame (twice the view width) with a single
 * AV_FRAME_DATA_STEREO3D (AV_STEREO3D_SIDEBYSIDE) entry, instead of two
 * interleaved full-size frames.
 *
 * The halves come from two packets, possibly decoded by two frame-threaded
 * workers, so pairing relies only on worker-shared state: the held base
 * half stays in the shared DPB and is located by the access-unit base POC
 * latched in h264_field_start() (au_base_poc), not by the dependent half's
 * own POC (see h264_sbs_find_base()). Both halves make the same
 * deterministic "assemble?" decision from stream-wide information
 * (view_count, view_ids selection, stereo arrangement): the base half
 * (view 0) is kept in the DPB, never delivered standalone; the dependent
 * half (view 1) finds it by POC and delivers the combined frame (left eye
 * on the left, per the arrangement). Single-eye selection, non-repackable
 * arrangements and single-view H.264 keep the existing delivery.
 * -------------------------------------------------------------------- */

static int h264_sbs_should_assemble(H264Context *h, const AVFrame *frame)
{
    if (h->view_count != 2) {
        /* exactly two views required; single-view is the normal no-op,
         * >2 views get a one-time note */
        if (h->view_count > 2) {
            static int sbs_multiview_logged;
            if (!sbs_multiview_logged) {
                sbs_multiview_logged = 1;
                av_log(h->avctx, AV_LOG_WARNING,
                       "multiview: native SBS output is only supported for "
                       "exactly 2 views (got %d); keeping the interleaved "
                       "delivery\n", h->view_count);
            }
        }
        return 0;
    }
    if (!h264_view_selected(h, 0) || !h264_view_selected(h, 1))
        return 0; /* single-eye selection: deliver the selected view(s) */

    /* even view width required: an odd width would leave the last chroma
     * column of the combined frame unwritten (see h264_sbs_assemble); the
     * halves share the SPS, so one frame covers the pair */
    if (frame->width & 1) {
        static int sbs_oddwidth_logged;
        if (!sbs_oddwidth_logged) {
            sbs_oddwidth_logged = 1;
            av_log(h->avctx, AV_LOG_WARNING,
                   "multiview: native SBS output requires an even view width "
                   "(got %d); keeping the interleaved delivery\n",
                   frame->width);
        }
        return 0;
    }

    const AVFrameSideData *sd = av_frame_get_side_data(frame,
                                                       AV_FRAME_DATA_STEREO3D);
    if (!sd)
        return 1; /* no arrangement metadata: default to view 0 = left */

    const AVStereo3D *s3d = (const AVStereo3D *)sd->data;
    /* only frame-sequence or side-by-side arrangements of two full-frame
     * views can be re-packed natively; other tags are sub-images of one
     * frame and keep the interleaved delivery */
    if (s3d->type != AV_STEREO3D_FRAMESEQUENCE &&
        s3d->type != AV_STEREO3D_SIDEBYSIDE) {
        static int sbs_arrangement_logged;
        if (!sbs_arrangement_logged) {
            sbs_arrangement_logged = 1;
            av_log(h->avctx, AV_LOG_WARNING,
                   "multiview: native SBS output cannot re-pack this stereo "
                   "arrangement (type %d); keeping the interleaved delivery\n",
                   s3d->type);
        }
        return 0;
    }
    return 1;
}

/* Locate the held base half of the access unit `dep` belongs to: the base
 * view's picture latched as the access unit's base in h264_field_start().
 * The dependent half's own POC is NOT a reliable key: on some 2D+delta
 * streams its active SPS carries mvc.present == 0 (compatibility SPS), so
 * it keeps a per-view-unwrapped POC offset from the base's, while the
 * base's POC is latched verbatim as au_base_poc. The dependent's POC is
 * only a defensive fallback when no latch is present.
 * Returns NULL when no matching base half is in the DPB. */
static H264Picture *h264_sbs_find_base(H264Context *h,
                                       const H264Picture *dep)
{
    const int target = h->au_base_valid ? h->au_base_poc : dep->poc;

    for (int i = 0; i < H264_MAX_PICTURE_COUNT; i++) {
        H264Picture *p = &h->DPB[i];

        if (p->f && p->f->data[0] &&
            p->view_idx == 0 && p->poc == target)
            return p;
    }
    return NULL;
}

/* Assemble the held base half with the dependent half `dep` into one
 * side-by-side frame in `pict`. Returns 1 on success, 0 when no assembly
 * happened (pict stays the dependent half, delivered standalone) or a
 * negative error code. */
static int h264_sbs_assemble(H264Context *h, AVFrame *pict,
                             const H264Picture *dep)
{
    H264Picture *base;
    const AVFrame *basef, *depf, *leftf, *rightf;
    const AVPixFmtDescriptor *desc;
    AVFrame *sbs = NULL;
    int ret;

    base = h264_sbs_find_base(h, dep);
    if (!base) {
        static int sbs_nobase_logged;
        if (!sbs_nobase_logged) {
            sbs_nobase_logged = 1;
            av_log(h->avctx, AV_LOG_WARNING,
                   "multiview: held base-view picture (poc %d) is gone from "
                   "the DPB; delivering the dependent view standalone\n",
                   dep->poc);
        }
        return 0;
    }

    basef = base->needs_fg ? base->f_grain : base->f;
    depf  = dep->needs_fg  ? dep->f_grain  : dep->f;
    /* even view width required (an odd width would leave the last chroma
     * column unwritten); also check size/format match */
    if (!basef || !depf || !basef->data[0] || !depf->data[0] ||
        (depf->width & 1) ||
        basef->width  != depf->width  || basef->height != depf->height ||
        basef->format != depf->format) {
        static int sbs_mismatch_logged;
        if (!sbs_mismatch_logged) {
            sbs_mismatch_logged = 1;
            av_log(h->avctx, AV_LOG_WARNING,
                   "multiview: the two views of one access unit are not "
                   "assemblable (size/format); delivering the dependent "
                   "view standalone\n");
        }
        /* release the assembly pin so a droppable (non-reference) base is
         * not leaked; normal reference bits, if any, are unaffected */
        base->reference &= ~DELAYED_PIC_REF;
        return 0;
    }

    /* Eye order: the arrangement metadata (present on both halves) names
     * the base (view 0) as the left eye unless INVERT is set; the halves
     * are placed so the left eye is always on the left (plain SBS tag) */
    {
        const AVFrameSideData *sd =
            av_frame_get_side_data(pict, AV_FRAME_DATA_STEREO3D);
        int base_is_left = 1;

        if (sd) {
            const AVStereo3D *s3d = (const AVStereo3D *)sd->data;
            if (s3d->type == AV_STEREO3D_FRAMESEQUENCE ||
                s3d->type == AV_STEREO3D_SIDEBYSIDE)
                base_is_left = !(s3d->flags & AV_STEREO3D_FLAG_INVERT);
        }
        leftf  = base_is_left ? basef : depf;
        rightf = base_is_left ? depf  : basef;
    }

    desc = av_pix_fmt_desc_get(depf->format);
    if (!desc)
        return 0;

    sbs = av_frame_alloc();
    if (!sbs)
        return AVERROR(ENOMEM);
    sbs->format = depf->format;
    sbs->width  = depf->width * 2;
    sbs->height = depf->height;
    ret = av_frame_get_buffer(sbs, 0);
    if (ret < 0)
        goto fail;

    {
        const int bps    = 1 << h->pixel_shift;
        const int xshift = desc->log2_chroma_w;
        const int yshift = desc->log2_chroma_h;
        const int planes = desc->nb_components == 1 ? 1 : 3;

        for (int p = 0; p < planes; p++) {
            const int    shx      = p ? xshift : 0;
            const int    shy      = p ? yshift : 0;
            const int    w_p      = depf->width  >> shx;
            const int    h_p      = depf->height >> shy;
            const size_t rowbytes = (size_t)w_p * bps;

            for (int y = 0; y < h_p; y++) {
                uint8_t *dst = sbs->data[p] + (size_t)y * sbs->linesize[p];

                memcpy(dst,
                       leftf->data[p]  + (size_t)y * leftf->linesize[p],
                       rowbytes);
                memcpy(dst + rowbytes,
                       rightf->data[p] + (size_t)y * rightf->linesize[p],
                       rowbytes);
            }
        }
    }

    /* the base half's pixels are now fully copied into the combined
     * frame: clear the assembly pin so a droppable (non-reference) base
     * can be reclaimed; a base still referenced keeps its normal bits */
    base->reference &= ~DELAYED_PIC_REF;
    /* The base half is consumed by the combined frame: mark it delivered
     * so the committed-picture machinery retires it. Without this it is
     * parked, re-emitted and re-held at every subsequent access unit, and
     * finally orphaned with its DELAYED_PIC_REF pin still set when the
     * view's next frame_start clears next_output_pic. */
    base->output_delivered = 1;

    /* Inherit the dependent half's delivered properties (timestamps,
     * flags, color, metadata, side data), including its decode-data
     * private_ref: DR1 requires the delivered frame to carry one, and
     * FrameDecodeData is a small generic marker, valid on the combined frame. */
    ret = av_frame_copy_props(sbs, pict);
    if (ret < 0)
        goto fail;
    /* ... replace the two-view arrangement tag with the side-by-side one;
     * the per-view id and per-eye arrangement metadata no longer apply to
     * the combined frame */
    av_frame_remove_side_data(sbs, AV_FRAME_DATA_STEREO3D);
    av_frame_remove_side_data(sbs, AV_FRAME_DATA_VIEW_ID);
    {
        AVStereo3D *s3d = av_stereo3d_create_side_data(sbs);
        if (!s3d)
            goto fail;
        s3d->type  = AV_STEREO3D_SIDEBYSIDE;
        s3d->flags = 0;
        s3d->view  = AV_STEREO3D_VIEW_PACKED;
    }
    av_dict_set(&sbs->metadata, "view_id", NULL, 0);
    av_dict_set(&sbs->metadata, "stereo_mode", NULL, 0);

    av_frame_unref(pict);
    ret = av_frame_ref(pict, sbs);
    av_frame_free(&sbs);
    return ret < 0 ? ret : 1;

fail:
    av_frame_free(&sbs);
    return ret;
}

/* Post-process a just-finalized multiview frame for native SBS:
 * 0 = deliver as-is, 1 = base half held (*got_frame 0), 2 = assembled
 * side-by-side frame in `pict`, or a negative error code. */
static int h264_sbs_process(H264Context *h, AVFrame *pict,
                            H264Picture *out, int *got_frame)
{
    if (!h264_sbs_should_assemble(h, pict))
        return 0;

    if (out->view_idx == 0) {
        /* Base half: keep it in the shared DPB (pinned even without its own
         * reference) until the dependent half of the same access unit
         * assembles it; never delivered standalone. */
        out->reference |= DELAYED_PIC_REF;
        av_frame_unref(pict);
        *got_frame = 0;
        return 1;
    }

    if (out->view_idx == 1) {
        int a = h264_sbs_assemble(h, pict, out);
        if (a < 0)
            return a;
        return a ? 2 : 0;
    }

    return 0;
}

static int h264_decode_frame(AVCodecContext *avctx, AVFrame *pict,
                             int *got_frame, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    H264Context *h     = avctx->priv_data;
    int buf_index;
    int ret;

    h->flags = avctx->flags;
    h->setup_finished = 0;
    h->nb_slice_ctx_queued = 0;

    /* Multiview only: latch this packet's dts (clamped non-decreasing, the
     * demuxer may hand non-monotonic dts, e.g. MKV after an input seek) so
     * every picture started from it keeps the dts of its access unit (see
     * h264_frame_start); multiview frames of one POC share timestamps.
     *
     * Single view: pass the dts through verbatim - clamping here would
     * change plain H.264 dts semantics (defined by decode.c, emulated in
     * finalize_frame).
     *
     * The h->au_base_* latch (h264_field_start) is intentionally NOT cleared
     * here: it holds the base view's POC and the access unit's timestamps
     * for the dependent view to adopt (some 3D streams write the dependent
     * view's pic_order_cnt_lsb with a per-view offset the per-view unwrap
     * cannot reconcile); adoption falls back to frame_num matching. */
    if (h->view_count > 1) {
        int64_t dts = avpkt->dts;
        if (dts != AV_NOPTS_VALUE &&
            h->last_in_dts != AV_NOPTS_VALUE && dts < h->last_in_dts)
            dts = h->last_in_dts;
        if (dts != AV_NOPTS_VALUE)
            h->last_in_dts = dts;
        h->pkt_dts = dts;
    } else
        h->pkt_dts = avpkt->dts;

    ff_h264_unref_picture(&h->last_pic_for_ec);

    /* end of stream, output what is still in the buffers */
    if (buf_size == 0) {
        for (int vs = 0; vs < h->view_count; vs++) {
            H264ViewState *v = &h->views[vs];
            int j = 0;
            if (v->parked_pic) {
                while (v->delayed_pic[j])
                    j++;
                av_assert0(j < FF_ARRAY_ELEMS(v->delayed_pic));
                v->delayed_pic[j] = v->parked_pic;
                v->parked_pic = NULL;
            }
        }
        return send_next_delayed_frame(h, pict, got_frame, 0);
    }

    if (av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA, NULL)) {
        size_t side_size;
        uint8_t *side = av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
        ff_h264_decode_extradata(side, side_size,
                                 &h->ps, &h->is_avc, &h->nal_length_size,
                                 avctx->err_recognition, avctx);
    }
    if (h->is_avc && buf_size >= 9 && buf[0]==1 && buf[2]==0 && (buf[4]&0xFC)==0xFC) {
        if (is_avcc_extradata(buf, buf_size))
            return ff_h264_decode_extradata(buf, buf_size,
                                            &h->ps, &h->is_avc, &h->nal_length_size,
                                            avctx->err_recognition, avctx);
    }

    buf_index = decode_nal_units(h, avpkt->buf, buf, buf_size);
    if (buf_index < 0)
        return AVERROR_INVALIDDATA;

    if (!h->cur_pic_ptr && h->nal_unit_type == H264_NAL_END_SEQUENCE) {
        av_assert0(buf_index <= buf_size);
        return send_next_delayed_frame(h, pict, got_frame, buf_index);
    }

    if (!(avctx->flags2 & AV_CODEC_FLAG2_CHUNKS) && (!h->cur_pic_ptr || !h->has_slice)) {
        if (avctx->skip_frame >= AVDISCARD_NONREF ||
            /* picture dropped because its referenced multiview view is
             * missing from the stream; consume silently, like skip_frame */
            (h->drop_view_slices && !h->cur_pic_ptr) ||
            /* all VCL belonged to unselected views (view_ids); consume silently */
            (h->view_sel_skipped && !h->cur_pic_ptr) ||
            buf_size >= 4 && !memcmp("Q264", buf, 4))
            return buf_size;
        av_log(avctx, AV_LOG_ERROR, "no frame!\n");
        return AVERROR_INVALIDDATA;
    }

    if (!(avctx->flags2 & AV_CODEC_FLAG2_CHUNKS) ||
        (h->mb_y >= h->mb_height && h->mb_height)) {
        if ((ret = ff_h264_field_end(h, &h->slice_ctx[0], 0)) < 0)
            return ret;

        /* Wait for second field. Multiview: each view commits its own
         * output picture; emit the lowest-POC one (base view first on
         * ties) so views interleave in display order. */
        {
            H264Picture *out = NULL;
            for (int vs = 0; vs < h->view_count; vs++) {
                H264ViewState *v = &h->views[vs];
                H264Picture *cand = v->next_output_pic;
                if (cand && h->view_count > 1 && cand->output_delivered) {
                    /* Stale alias: this context's copy of the view's
                     * committed-picture pointer predates another worker
                     * delivering that picture (a mid-decode context sync
                     * copied it); emitting would deliver it twice.
                     * Retire the pointer; log the desync once. */
                    static int delivered_alias_logged;

                    if (!delivered_alias_logged) {
                        delivered_alias_logged = 1;
                        av_log(avctx, AV_LOG_INFO,
                               "multiview view %d: retired a stale pointer to "
                               "an already delivered picture (poc %d); frame-"
                               "thread context alias\n",
                               vs, cand->poc);
                    }
                    v->next_output_pic = NULL;
                    cand = NULL;
                }
                if (cand && (!out || cand->poc < out->poc ||
                             (cand->poc == out->poc &&
                              cand->view_idx < out->view_idx)))
                    out = cand;
            }
            if (out) {
                /* Output-band fix: emit one duplicate of the predecessor
                 * (captured at `out`'s commit) ahead of the output-band
                 * dependent picture: the duplicate goes out this packet,
                 * `out` stays committed for the parking loop below, and
                 * the predecessor keeps its delayed-queue entry for its
                 * own reorder slot. */
                H264Picture *xsrc = NULL;
                H264Picture *emit = out;

                if (h->view_count > 1 && out->output_dup_before &&
                    !out->output_dup_done &&
                    (xsrc = out->output_dup_src) &&
                    xsrc->f && xsrc->f->data[0])
                    emit = xsrc;
                ret = finalize_frame(h, pict, emit, got_frame);
                if (ret < 0)
                    return ret;
                if (*got_frame) {
                    static int parked_overwrite_logged; /* one-shot log */
                    /* Native SBS: holds the base half (got_frame -> 0),
                     * replaces pict with the assembled frame for the
                     * dependent half; skipped for the duplicate case */
                    if (emit == out) {
                        int sbs = h264_sbs_process(h, pict, emit, got_frame);
                        if (sbs < 0)
                            return sbs;
                    }
                    if (xsrc) {
                        static int output_dup_logged;  /* one-shot log */

                        if (!output_dup_logged) {
                            output_dup_logged = 1;
                            av_log(h->avctx, AV_LOG_INFO,
                                   "multiview view %d: emitting the expected "
                                   "duplicated predecessor poc %d ahead of "
                                   "poc %d\n",
                                   h->views[out->view_idx].view_id,
                                   xsrc->poc, out->poc);
                        }
                        av_log(h->avctx, AV_LOG_VERBOSE,
                               "view=%d dup_poc=%d dup_fn=%d "
                               "ahead_poc=%d ahead_fn=%d\n",
                               h->views[out->view_idx].view_id,
                               xsrc->poc, xsrc->frame_num,
                               out->poc, out->frame_num);
                        out->output_dup_done = 1;
                        /* `out` is deliberately left in next_output_pic:
                         * the parking loop parks it for re-emission at the
                         * view's next select */
                    } else {
                        h->views[out->view_idx].next_output_pic = NULL;
                        out->output_delivered = 1;
                    }
                    /* The other views' committed pictures cannot be emitted
                     * until the next packet: park them in the per-view slot
                     * - committed pictures must not be mixed into the
                     * decode-order delayed list - and h264_select_output_frame()
                     * re-emits each at the view's next select.
                     *
                     * A slot that still holds a picture means the previous
                     * park was never re-emitted (its POC epoch is no longer
                     * reached); losing it is unavoidable, so retire it
                     * unpinned and log the desync once. */
                    for (int vs = 0; vs < h->view_count; vs++) {
                        H264ViewState *v = &h->views[vs];

                        if (!v->next_output_pic)
                            continue;
                        if (v->next_output_pic->output_delivered) {
                            /* Already delivered: either the base half
                             * consumed by the native SBS assembly of the
                             * access unit just delivered (marked in
                             * h264_sbs_assemble()), or a stale alias from
                             * another frame-thread context. Retire it (and
                             * its assembly pin) instead of parking it - a
                             * parked consumed base would be re-emitted and
                             * re-held at every subsequent access unit, then
                             * orphaned with its DELAYED_PIC_REF pin still set. */
                            v->next_output_pic->reference &= ~DELAYED_PIC_REF;
                            v->next_output_pic = NULL;
                            continue;
                        }
                        if (v->parked_pic) {
                            if (!parked_overwrite_logged) {
                                parked_overwrite_logged = 1;
                                av_log(h->avctx, AV_LOG_WARNING,
                                       "multiview view %d: dropping a "
                                       "committed picture (poc %d) that "
                                       "was still parked when its view's "
                                       "next picture committed "
                                       "(poc %d); POC epoch desync\n",
                                       vs, v->parked_pic->poc,
                                       v->next_output_pic->poc);
                            }
                            v->parked_pic->reference &= ~DELAYED_PIC_REF;
                            v->parked_pic = NULL;
                        }
                        v->parked_pic = v->next_output_pic;
                        v->parked_pic->reference |= DELAYED_PIC_REF;
                        v->next_output_pic = NULL;
                    }
                }
            }
        }
    }

    av_assert0(pict->buf[0] || !*got_frame);

    ff_h264_unref_picture(&h->last_pic_for_ec);

    return buf_size;
}

#define OFFSET(x) offsetof(H264Context, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
#define VDX VD | AV_OPT_FLAG_EXPORT
static const AVOption h264_options[] = {
    { "is_avc", "is avc", OFFSET(is_avc), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VDX },
    { "nal_length_size", "nal_length_size", OFFSET(nal_length_size), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 4, VDX },
    { "enable_er", "Enable error resilience on damaged frames (unsafe)", OFFSET(enable_er), AV_OPT_TYPE_BOOL, { .i64 = -1 }, -1, 1, VD },
    { "x264_build", "Assume this x264 version if no x264 version found in any SEI", OFFSET(x264_build), AV_OPT_TYPE_INT, {.i64 = -1}, -1, INT_MAX, VD },
    { "skip_gray", "Do not return gray gap frames", OFFSET(skip_gray), AV_OPT_TYPE_BOOL, {.i64 = 0}, 0, 1, VD },
    { "noref_gray", "Avoid using gray gap frames as references", OFFSET(noref_gray), AV_OPT_TYPE_BOOL, {.i64 = 1}, 0, 1, VD },
    { "view_ids", "Array of view IDs that should be decoded and output; a single -1 to decode all views",
        .offset = OFFSET(view_ids), .type = AV_OPT_TYPE_INT | AV_OPT_TYPE_FLAG_ARRAY,
        .min = -1, .max = INT_MAX, .flags = VD },
    { "view_ids_available", "Array of available view IDs is exported here",
        .offset = OFFSET(view_ids_available), .type = AV_OPT_TYPE_UINT | AV_OPT_TYPE_FLAG_ARRAY,
        .flags = VDX | AV_OPT_FLAG_READONLY },
    { "view_pos_available", "Array of view positions for view_ids_available is exported here, as AVStereo3DView",
        .offset = OFFSET(view_pos_available), .type = AV_OPT_TYPE_UINT | AV_OPT_TYPE_FLAG_ARRAY,
        .flags = VDX | AV_OPT_FLAG_READONLY, .unit = "view_pos" },
        { "unspecified", .type = AV_OPT_TYPE_CONST, .default_val = { .i64 = AV_STEREO3D_VIEW_UNSPEC }, .unit = "view_pos" },
        { "left",        .type = AV_OPT_TYPE_CONST, .default_val = { .i64 = AV_STEREO3D_VIEW_LEFT },   .unit = "view_pos" },
        { "right",       .type = AV_OPT_TYPE_CONST, .default_val = { .i64 = AV_STEREO3D_VIEW_RIGHT },  .unit = "view_pos" },
    { NULL },
};

static const AVClass h264_class = {
    .class_name = "H264 Decoder",
    .item_name  = av_default_item_name,
    .option     = h264_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_decoder = {
    .p.name                = "h264",
    CODEC_LONG_NAME("H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10"),
    .p.type                = AVMEDIA_TYPE_VIDEO,
    .p.id                  = AV_CODEC_ID_H264,
    .priv_data_size        = sizeof(H264Context),
    .init                  = h264_decode_init,
    .close                 = h264_decode_end,
    .post_receive_frame    = h264_post_receive_frame,
    FF_CODEC_DECODE_CB(h264_decode_frame),
    .p.capabilities        = AV_CODEC_CAP_DR1 |
                             AV_CODEC_CAP_DELAY | AV_CODEC_CAP_SLICE_THREADS |
                             AV_CODEC_CAP_FRAME_THREADS,
    .hw_configs            = (const AVCodecHWConfigInternal *const []) {
#if CONFIG_H264_DXVA2_HWACCEL
                               HWACCEL_DXVA2(h264),
#endif
#if CONFIG_H264_D3D11VA_HWACCEL
                               HWACCEL_D3D11VA(h264),
#endif
#if CONFIG_H264_D3D11VA2_HWACCEL
                               HWACCEL_D3D11VA2(h264),
#endif
#if CONFIG_H264_D3D12VA_HWACCEL
                               HWACCEL_D3D12VA(h264),
#endif
#if CONFIG_H264_NVDEC_HWACCEL
                               HWACCEL_NVDEC(h264),
#endif
#if CONFIG_H264_NVDEC_CUARRAY_HWACCEL
                               HWACCEL_NVDEC_CUARRAY(h264),
#endif
#if CONFIG_H264_VAAPI_HWACCEL
                               HWACCEL_VAAPI(h264),
#endif
#if CONFIG_H264_VDPAU_HWACCEL
                               HWACCEL_VDPAU(h264),
#endif
#if CONFIG_H264_VIDEOTOOLBOX_HWACCEL
                               HWACCEL_VIDEOTOOLBOX(h264),
#endif
#if CONFIG_H264_VULKAN_HWACCEL
                               HWACCEL_VULKAN(h264),
#endif
                               NULL
                           },
    .caps_internal         = FF_CODEC_CAP_EXPORTS_CROPPING |
                              FF_CODEC_CAP_INIT_CLEANUP |
                              FF_CODEC_CAP_SETS_PKT_DTS,
    .flush                 = h264_decode_flush,
    UPDATE_THREAD_CONTEXT(ff_h264_update_thread_context),
    UPDATE_THREAD_CONTEXT_FOR_USER(ff_h264_update_thread_context_for_user),
    .p.profiles            = NULL_IF_CONFIG_SMALL(ff_h264_profiles),
    .p.priv_class          = &h264_class,
};
