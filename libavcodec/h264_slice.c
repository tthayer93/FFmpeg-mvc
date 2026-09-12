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

#include "config_components.h"

#include "libavutil/avassert.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/timecode.h"
#include "decode.h"
#include "cabac.h"
#include "cabac_functions.h"
#include "error_resilience.h"
#include "avcodec.h"
#include "h264.h"
#include "h264dec.h"
#include "h264_mvpred.h"
#include "h264data.h"
#include "h264chroma.h"
#include "h264_ps.h"
#include "golomb.h"
#include "mathops.h"
#include "mpegutils.h"
#include "rectangle.h"
#include "libavutil/refstruct.h"
#include "thread.h"
#include "threadframe.h"

static const uint8_t field_scan[16+1] = {
    0 + 0 * 4, 0 + 1 * 4, 1 + 0 * 4, 0 + 2 * 4,
    0 + 3 * 4, 1 + 1 * 4, 1 + 2 * 4, 1 + 3 * 4,
    2 + 0 * 4, 2 + 1 * 4, 2 + 2 * 4, 2 + 3 * 4,
    3 + 0 * 4, 3 + 1 * 4, 3 + 2 * 4, 3 + 3 * 4,
};

static const uint8_t field_scan8x8[64+1] = {
    0 + 0 * 8, 0 + 1 * 8, 0 + 2 * 8, 1 + 0 * 8,
    1 + 1 * 8, 0 + 3 * 8, 0 + 4 * 8, 1 + 2 * 8,
    2 + 0 * 8, 1 + 3 * 8, 0 + 5 * 8, 0 + 6 * 8,
    0 + 7 * 8, 1 + 4 * 8, 2 + 1 * 8, 3 + 0 * 8,
    2 + 2 * 8, 1 + 5 * 8, 1 + 6 * 8, 1 + 7 * 8,
    2 + 3 * 8, 3 + 1 * 8, 4 + 0 * 8, 3 + 2 * 8,
    2 + 4 * 8, 2 + 5 * 8, 2 + 6 * 8, 2 + 7 * 8,
    3 + 3 * 8, 4 + 1 * 8, 5 + 0 * 8, 4 + 2 * 8,
    3 + 4 * 8, 3 + 5 * 8, 3 + 6 * 8, 3 + 7 * 8,
    4 + 3 * 8, 5 + 1 * 8, 6 + 0 * 8, 5 + 2 * 8,
    4 + 4 * 8, 4 + 5 * 8, 4 + 6 * 8, 4 + 7 * 8,
    5 + 3 * 8, 6 + 1 * 8, 6 + 2 * 8, 5 + 4 * 8,
    5 + 5 * 8, 5 + 6 * 8, 5 + 7 * 8, 6 + 3 * 8,
    7 + 0 * 8, 7 + 1 * 8, 6 + 4 * 8, 6 + 5 * 8,
    6 + 6 * 8, 6 + 7 * 8, 7 + 2 * 8, 7 + 3 * 8,
    7 + 4 * 8, 7 + 5 * 8, 7 + 6 * 8, 7 + 7 * 8,
};

static const uint8_t field_scan8x8_cavlc[64+1] = {
    0 + 0 * 8, 1 + 1 * 8, 2 + 0 * 8, 0 + 7 * 8,
    2 + 2 * 8, 2 + 3 * 8, 2 + 4 * 8, 3 + 3 * 8,
    3 + 4 * 8, 4 + 3 * 8, 4 + 4 * 8, 5 + 3 * 8,
    5 + 5 * 8, 7 + 0 * 8, 6 + 6 * 8, 7 + 4 * 8,
    0 + 1 * 8, 0 + 3 * 8, 1 + 3 * 8, 1 + 4 * 8,
    1 + 5 * 8, 3 + 1 * 8, 2 + 5 * 8, 4 + 1 * 8,
    3 + 5 * 8, 5 + 1 * 8, 4 + 5 * 8, 6 + 1 * 8,
    5 + 6 * 8, 7 + 1 * 8, 6 + 7 * 8, 7 + 5 * 8,
    0 + 2 * 8, 0 + 4 * 8, 0 + 5 * 8, 2 + 1 * 8,
    1 + 6 * 8, 4 + 0 * 8, 2 + 6 * 8, 5 + 0 * 8,
    3 + 6 * 8, 6 + 0 * 8, 4 + 6 * 8, 6 + 2 * 8,
    5 + 7 * 8, 6 + 4 * 8, 7 + 2 * 8, 7 + 6 * 8,
    1 + 0 * 8, 1 + 2 * 8, 0 + 6 * 8, 3 + 0 * 8,
    1 + 7 * 8, 3 + 2 * 8, 2 + 7 * 8, 4 + 2 * 8,
    3 + 7 * 8, 5 + 2 * 8, 4 + 7 * 8, 5 + 4 * 8,
    6 + 3 * 8, 6 + 5 * 8, 7 + 3 * 8, 7 + 7 * 8,
};

// zigzag_scan8x8_cavlc[i] = zigzag_scan8x8[(i/4) + 16*(i%4)]
static const uint8_t zigzag_scan8x8_cavlc[64+1] = {
    0 + 0 * 8, 1 + 1 * 8, 1 + 2 * 8, 2 + 2 * 8,
    4 + 1 * 8, 0 + 5 * 8, 3 + 3 * 8, 7 + 0 * 8,
    3 + 4 * 8, 1 + 7 * 8, 5 + 3 * 8, 6 + 3 * 8,
    2 + 7 * 8, 6 + 4 * 8, 5 + 6 * 8, 7 + 5 * 8,
    1 + 0 * 8, 2 + 0 * 8, 0 + 3 * 8, 3 + 1 * 8,
    3 + 2 * 8, 0 + 6 * 8, 4 + 2 * 8, 6 + 1 * 8,
    2 + 5 * 8, 2 + 6 * 8, 6 + 2 * 8, 5 + 4 * 8,
    3 + 7 * 8, 7 + 3 * 8, 4 + 7 * 8, 7 + 6 * 8,
    0 + 1 * 8, 3 + 0 * 8, 0 + 4 * 8, 4 + 0 * 8,
    2 + 3 * 8, 1 + 5 * 8, 5 + 1 * 8, 5 + 2 * 8,
    1 + 6 * 8, 3 + 5 * 8, 7 + 1 * 8, 4 + 5 * 8,
    4 + 6 * 8, 7 + 4 * 8, 5 + 7 * 8, 6 + 7 * 8,
    0 + 2 * 8, 2 + 1 * 8, 1 + 3 * 8, 5 + 0 * 8,
    1 + 4 * 8, 2 + 4 * 8, 6 + 0 * 8, 4 + 3 * 8,
    0 + 7 * 8, 4 + 4 * 8, 7 + 2 * 8, 3 + 6 * 8,
    5 + 5 * 8, 6 + 5 * 8, 6 + 6 * 8, 7 + 7 * 8,
};

/* a picture held for delayed output of any view (delayed list,
 * next_output_pic, parked_pic) must survive until output, even without
 * its own reference marking */
static int h264_pic_held_for_output(const H264Context *h, const H264Picture *pic)
{
    for (int vs = 0; vs < h->view_count; vs++) {
        const H264ViewState *v = &h->views[vs];

        if (v->next_output_pic == pic || v->parked_pic == pic)
            return 1;
        for (int i = 0; v->delayed_pic[i]; i++)
            if (v->delayed_pic[i] == pic)
                return 1;
    }
    /* a half waiting to be paired into a composed side-by-side frame is
     * held for output as well, and is in none of the lists above */
    return ff_h264_pic_held_for_compose(h, pic);
}

/**
 * View ID of the base view of the adopted multiview SPS: the view with ID 0,
 * or - defensively, for a stream carrying no view ID 0 - the first view
 * declared in the SPS. Before any multiview SPS has been adopted the only
 * view there is, is the base one.
 */
int h264_base_view_id(const H264Context *h)
{
    const H264MVCSPS *mvc = h->mvc_sps ? &h->mvc_sps->mvc : NULL;

    if (!mvc)
        return h->views[0].view_id;
    for (int i = 0; i < (int)mvc->num_views; i++)
        if ((int)mvc->view_id[i] == 0)
            return 0;
    return mvc->view_id[0];
}

/**
 * View selection (view_ids option): a view is selected when its id appears in
 * h->view_ids (validated against the SPS view list by h264_mvc_export()), or
 * a single -1 was requested (all views). No selection at all selects the base
 * view only - resolved here, at the decision points, rather than by writing a
 * default into h->view_ids when the multiview SPS is adopted, so that a
 * selection made by the caller later always governs the output (see
 * h264_mvc_export()). Single-view streams always select their only view.
 */
int h264_view_selected(const H264Context *h, int slot)
{
    if (h->view_count <= 1)
        return 1;
    if (!h->nb_view_ids)
        return h->views[slot].view_id == h264_base_view_id(h);
    if (h->nb_view_ids == 1 && h->view_ids[0] == -1)
        return 1;
    for (unsigned i = 0; i < h->nb_view_ids; i++)
        if (h->views[slot].view_id == h->view_ids[i])
            return 1;
    return 0;
}

static void release_unused_pictures(H264Context *h, int remove_current)
{
    int i;

    /* release non reference frames */
    for (i = 0; i < H264_MAX_PICTURE_COUNT; i++) {
        if (h->DPB[i].f->buf[0] && !h->DPB[i].reference &&
            (remove_current || &h->DPB[i] != h->cur_pic_ptr) &&
            !h264_pic_held_for_output(h, &h->DPB[i])) {
            ff_h264_unref_picture(&h->DPB[i]);
        }
    }
}

static int alloc_scratch_buffers(H264SliceContext *sl, int linesize)
{
    const H264Context *h = sl->h264;
    int alloc_size = FFALIGN(FFABS(linesize) + 32, 32);

    av_fast_malloc(&sl->bipred_scratchpad, &sl->bipred_scratchpad_allocated, 16 * 6 * alloc_size);
    // edge emu needs blocksize + filter length - 1
    // (= 21x21 for  H.264)
    av_fast_malloc(&sl->edge_emu_buffer, &sl->edge_emu_buffer_allocated, alloc_size * 2 * 21);

    av_fast_mallocz(&sl->top_borders[0], &sl->top_borders_allocated[0],
                   h->mb_width * 16 * 3 * sizeof(uint8_t) * 2);
    av_fast_mallocz(&sl->top_borders[1], &sl->top_borders_allocated[1],
                   h->mb_width * 16 * 3 * sizeof(uint8_t) * 2);

    if (!sl->bipred_scratchpad || !sl->edge_emu_buffer ||
        !sl->top_borders[0]    || !sl->top_borders[1]) {
        av_freep(&sl->bipred_scratchpad);
        av_freep(&sl->edge_emu_buffer);
        av_freep(&sl->top_borders[0]);
        av_freep(&sl->top_borders[1]);

        sl->bipred_scratchpad_allocated = 0;
        sl->edge_emu_buffer_allocated   = 0;
        sl->top_borders_allocated[0]    = 0;
        sl->top_borders_allocated[1]    = 0;
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int init_table_pools(H264Context *h)
{
    const int big_mb_num    = h->mb_stride * (h->mb_height + 1) + 1;
    const int mb_array_size = h->mb_stride * h->mb_height;
    const int b4_stride     = h->mb_width * 4 + 1;
    const int b4_array_size = b4_stride * h->mb_height * 4;

    h->qscale_table_pool = av_refstruct_pool_alloc(big_mb_num + h->mb_stride, 0);
    h->mb_type_pool      = av_refstruct_pool_alloc((big_mb_num + h->mb_stride) *
                                                   sizeof(uint32_t), 0);
    h->motion_val_pool   = av_refstruct_pool_alloc(2 * (b4_array_size + 4) *
                                                   sizeof(int16_t), 0);
    h->ref_index_pool    = av_refstruct_pool_alloc(4 * mb_array_size, 0);

    if (!h->qscale_table_pool || !h->mb_type_pool || !h->motion_val_pool ||
        !h->ref_index_pool) {
        av_refstruct_pool_uninit(&h->qscale_table_pool);
        av_refstruct_pool_uninit(&h->mb_type_pool);
        av_refstruct_pool_uninit(&h->motion_val_pool);
        av_refstruct_pool_uninit(&h->ref_index_pool);
        return AVERROR(ENOMEM);
    }

    return 0;
}

static int alloc_picture(H264Context *h, H264Picture *pic)
{
    int i, ret = 0;

    av_assert0(!pic->f->data[0]);

    if (h->sei.common.lcevc.info) {
        HEVCSEILCEVC *lcevc = &h->sei.common.lcevc;
        ret = ff_frame_new_side_data_from_buf(h->avctx, pic->f, AV_FRAME_DATA_LCEVC, &lcevc->info);
        if (ret < 0)
            return ret;
    }

    /* Multiview: tag every view's picture with its stream-wide view id
     * as side data before buffer allocation, so get_buffer()
     * implementations can route frames per view (like the HEVC decoder);
     * pic->view_id is set in h264_frame_start() */
    if (h->view_count > 1) {
        AVFrameSideData *sd = av_frame_side_data_new(&pic->f->side_data,
                                                     &pic->f->nb_side_data,
                                                     AV_FRAME_DATA_VIEW_ID,
                                                     sizeof(int), 0);
        if (!sd)
            goto fail;
        *(int*)sd->data = pic->view_id;
    }

    pic->tf.f = pic->f;
    ret = ff_thread_get_ext_buffer(h->avctx, &pic->tf,
                                   pic->reference ? AV_GET_BUFFER_FLAG_REF : 0);
    if (ret < 0)
        goto fail;

    if (pic->needs_fg) {
        pic->f_grain->format = pic->f->format;
        pic->f_grain->width = pic->f->width;
        pic->f_grain->height = pic->f->height;
        ret = ff_thread_get_buffer(h->avctx, pic->f_grain, 0);
        if (ret < 0)
            goto fail;
    }

    ret = ff_hwaccel_frame_priv_alloc(h->avctx, &pic->hwaccel_picture_private);
    if (ret < 0)
        goto fail;

    if (h->decode_error_flags_pool) {
        pic->decode_error_flags = av_refstruct_pool_get(h->decode_error_flags_pool);
        if (!pic->decode_error_flags)
            goto fail;
        atomic_init(pic->decode_error_flags, 0);
    }

    if (CONFIG_GRAY && !h->avctx->hwaccel && h->flags & AV_CODEC_FLAG_GRAY && pic->f->data[2]) {
        int h_chroma_shift, v_chroma_shift;
        av_pix_fmt_get_chroma_sub_sample(pic->f->format,
                                         &h_chroma_shift, &v_chroma_shift);

        for(i=0; i<AV_CEIL_RSHIFT(pic->f->height, v_chroma_shift); i++) {
            memset(pic->f->data[1] + pic->f->linesize[1]*i,
                   0x80, AV_CEIL_RSHIFT(pic->f->width, h_chroma_shift));
            memset(pic->f->data[2] + pic->f->linesize[2]*i,
                   0x80, AV_CEIL_RSHIFT(pic->f->width, h_chroma_shift));
        }
    }

    if (!h->qscale_table_pool) {
        ret = init_table_pools(h);
        if (ret < 0)
            goto fail;
    }

    pic->qscale_table_base = av_refstruct_pool_get(h->qscale_table_pool);
    pic->mb_type_base      = av_refstruct_pool_get(h->mb_type_pool);
    if (!pic->qscale_table_base || !pic->mb_type_base)
        goto fail;

    pic->mb_type      = pic->mb_type_base + 2 * h->mb_stride + 1;
    pic->qscale_table = pic->qscale_table_base + 2 * h->mb_stride + 1;

    for (i = 0; i < 2; i++) {
        pic->motion_val_base[i] = av_refstruct_pool_get(h->motion_val_pool);
        pic->ref_index[i]       = av_refstruct_pool_get(h->ref_index_pool);
        if (!pic->motion_val_base[i] || !pic->ref_index[i])
            goto fail;

        pic->motion_val[i] = pic->motion_val_base[i] + 4;
    }

    pic->pps = av_refstruct_ref_c(h->ps.pps);

    pic->mb_width  = h->mb_width;
    pic->mb_height = h->mb_height;
    pic->mb_stride = h->mb_stride;

    return 0;
fail:
    ff_h264_unref_picture(pic);
    return (ret < 0) ? ret : AVERROR(ENOMEM);
}

static int find_unused_picture(const H264Context *h)
{
    int i;

    for (i = 0; i < H264_MAX_PICTURE_COUNT; i++) {
        if (!h->DPB[i].f->buf[0])
            return i;
    }
    return AVERROR_INVALIDDATA;
}


#define IN_RANGE(a, b, size) (((void*)(a) >= (void*)(b)) && ((void*)(a) < (void*)((b) + (size))))

#define REBASE_PICTURE(pic, new_ctx, old_ctx)             \
    (((pic) && (pic) >= (old_ctx)->DPB &&                       \
      (pic) < (old_ctx)->DPB + H264_MAX_PICTURE_COUNT) ?          \
     &(new_ctx)->DPB[(pic) - (old_ctx)->DPB] : NULL)

static void copy_picture_range(H264Picture **to, H264Picture *const *from, int count,
                               H264Context *new_base, const H264Context *old_base)
{
    int i;

    for (i = 0; i < count; i++) {
        av_assert1(!from[i] ||
                   IN_RANGE(from[i], old_base, 1) ||
                   IN_RANGE(from[i], old_base->DPB, H264_MAX_PICTURE_COUNT));
        to[i] = REBASE_PICTURE(from[i], new_base, old_base);
    }
}

static void color_frame(AVFrame *frame, const int c[4])
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);

    av_assert0(desc->flags & AV_PIX_FMT_FLAG_PLANAR);

    for (int p = 0; p < desc->nb_components; p++) {
        uint8_t *dst = frame->data[p];
        int is_chroma = p == 1 || p == 2;
        int bytes  = is_chroma ? AV_CEIL_RSHIFT(frame->width,  desc->log2_chroma_w) : frame->width;
        int height = is_chroma ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
        if (desc->comp[0].depth >= 9) {
            if (bytes >= 1)
                ((uint16_t*)dst)[0] = c[p];
            if (bytes >= 2)
                av_memcpy_backptr(dst + 2, 2, 2 * (bytes - 1));
            dst += frame->linesize[p];
            for (int y = 1; y < height; y++) {
                memcpy(dst, frame->data[p], 2*bytes);
                dst += frame->linesize[p];
            }
        } else {
            for (int y = 0; y < height; y++) {
                memset(dst, c[p], bytes);
                dst += frame->linesize[p];
            }
        }
    }
}

static int h264_slice_header_init(H264Context *h);

int ff_h264_update_thread_context(AVCodecContext *dst,
                                  const AVCodecContext *src)
{
    H264Context *h = dst->priv_data, *h1 = src->priv_data;
    int inited = h->context_initialized, err = 0;
    int need_reinit = 0;
    int i, ret;

    if (dst == src)
        return 0;

    if (inited && !h1->ps.sps)
        return AVERROR_INVALIDDATA;

    if (inited &&
        (h->width                 != h1->width                 ||
         h->height                != h1->height                ||
         h->mb_width              != h1->mb_width              ||
         h->mb_height             != h1->mb_height             ||
         !h->ps.sps                                            ||
         h->ps.sps->bit_depth_luma    != h1->ps.sps->bit_depth_luma    ||
         h->ps.sps->chroma_format_idc != h1->ps.sps->chroma_format_idc ||
         h->ps.sps->vui.matrix_coeffs != h1->ps.sps->vui.matrix_coeffs)) {
        need_reinit = 1;
    }

    /* copy block_offset since frame_start may not be called */
    memcpy(h->block_offset, h1->block_offset, sizeof(h->block_offset));

    // SPS/PPS
    for (int i = 0; i < FF_ARRAY_ELEMS(h->ps.sps_list); i++)
        av_refstruct_replace(&h->ps.sps_list[i], h1->ps.sps_list[i]);
    for (int i = 0; i < FF_ARRAY_ELEMS(h->ps.pps_list); i++)
        av_refstruct_replace(&h->ps.pps_list[i], h1->ps.pps_list[i]);

    av_refstruct_replace(&h->ps.pps, h1->ps.pps);
    h->ps.sps = h1->ps.sps;

    // multiview SPS (RefStruct reference)
    av_refstruct_replace(&h->mvc_sps, h1->mvc_sps);

    if (need_reinit || !inited) {
        h->width     = h1->width;
        h->height    = h1->height;
        h->mb_height = h1->mb_height;
        h->mb_width  = h1->mb_width;
        h->mb_num    = h1->mb_num;
        h->mb_stride = h1->mb_stride;
        h->b_stride  = h1->b_stride;
        h->x264_build = h1->x264_build;

        if (h->context_initialized || h1->context_initialized) {
            if ((err = h264_slice_header_init(h)) < 0) {
                av_log(h->avctx, AV_LOG_ERROR, "h264_slice_header_init() failed");
                return err;
            }
        }

        /* copy block_offset since frame_start may not be called */
        memcpy(h->block_offset, h1->block_offset, sizeof(h->block_offset));
    }

    h->width_from_caller    = h1->width_from_caller;
    h->height_from_caller   = h1->height_from_caller;
    h->first_field          = h1->first_field;
    h->picture_structure    = h1->picture_structure;
    h->mb_aff_frame         = h1->mb_aff_frame;
    h->droppable            = h1->droppable;

    for (i = 0; i < H264_MAX_PICTURE_COUNT; i++) {
        ret = ff_h264_replace_picture(&h->DPB[i], &h1->DPB[i]);
        if (ret < 0)
            return ret;
    }

    h->cur_pic_ptr = REBASE_PICTURE(h1->cur_pic_ptr, h, h1);
    ret = ff_h264_replace_picture(&h->cur_pic, &h1->cur_pic);
    if (ret < 0)
        return ret;

    h->enable_er       = h1->enable_er;
    h->workaround_bugs = h1->workaround_bugs;
    h->droppable       = h1->droppable;

    // extradata/NAL handling
    h->is_avc = h1->is_avc;
    h->nal_length_size = h1->nal_length_size;

    h->view_count = h1->view_count;
    h->cur_view   = h1->cur_view;
    /* view_ids is an option that can change after the workers exist (the
     * stream-specifier view setup writes it into the master on the first
     * SPS, long after av_opt_copy); without this diff-copy a worker would
     * select with a stale (empty) list. Mirror the HEVC diff-copy. */
    if (h->nb_view_ids != h1->nb_view_ids ||
        (h->nb_view_ids &&
         memcmp(h->view_ids, h1->view_ids,
                sizeof(*h->view_ids) * h->nb_view_ids))) {
        av_freep(&h->view_ids);
        h->nb_view_ids = 0;

        if (h1->nb_view_ids) {
            h->view_ids = av_memdup(h1->view_ids,
                                    h1->nb_view_ids *
                                    sizeof(*h1->view_ids));
            if (!h->view_ids)
                return AVERROR(ENOMEM);
            h->nb_view_ids = h1->nb_view_ids;
        }
    }
    /* Mirror the exported view list too: written only by the context that
     * parses the multiview SPS, but read by whichever context runs the
     * format negotiation, so a worker that did not parse the SPS itself
     * would otherwise see a stale (empty) list. */
    if (h->nb_view_ids_available != h1->nb_view_ids_available ||
        (h->nb_view_ids_available &&
         memcmp(h->view_ids_available, h1->view_ids_available,
                sizeof(*h->view_ids_available) *
                h->nb_view_ids_available))) {
        av_freep(&h->view_ids_available);
        h->nb_view_ids_available = 0;

        if (h1->nb_view_ids_available) {
            h->view_ids_available = av_memdup(h1->view_ids_available,
                                              h1->nb_view_ids_available *
                                              sizeof(*h1->view_ids_available));
            if (!h->view_ids_available)
                return AVERROR(ENOMEM);
            h->nb_view_ids_available = h1->nb_view_ids_available;
        }
    }
    /* Only the first view_count slots carry state (registration only
     * appends; every consumer bounds itself by view_count).
     *
     * next_output_pic and parked_pic are owned by the context that
     * committed the picture and must NOT be cloned: multiview emit defers
     * delivery, so a context sync in between would clone the pointer into
     * a sibling worker, whose later emit would finalize the same picture a
     * second time (the multiview over-delivery). All other per-view fields
     * are shared output-ordering state and keep cloning as before. The
     * destination worker is idle here, so preserving its own pointers is
     * race free. */
    for (i = 0; i < h->view_count; i++) {
        H264Picture *local_next   = h->views[i].next_output_pic;
        H264Picture *local_parked = h->views[i].parked_pic;

        h->views[i] = h1->views[i];
        h->views[i].next_output_pic = local_next;
        h->views[i].parked_pic      = local_parked;
    }

    h->poc_offset        = h1->poc_offset;

    /* AU and packet latches: written only by h264_decode_frame() and
     * h264_field_start(), never re-derived, so a frame-thread worker must
     * inherit them - without the au_base_* values the dependent-view
     * worker loses the base view's POC/pts of the access unit and falls
     * back to the unreliable frame_num matching. */
    h->pkt_dts           = h1->pkt_dts;
    h->last_in_dts       = h1->last_in_dts;
    h->last_out_dts      = h1->last_out_dts;
    h->au_base_poc       = h1->au_base_poc;
    memcpy(h->au_base_field_poc, h1->au_base_field_poc,
           sizeof(h->au_base_field_poc));
    h->au_base_pts       = h1->au_base_pts;
    h->au_base_pkt_dts   = h1->au_base_pkt_dts;
    h->au_base_valid     = h1->au_base_valid;

    memcpy(h->mmco, h1->mmco, sizeof(h->mmco));
    h->nb_mmco         = h1->nb_mmco;
    h->mmco_reset      = h1->mmco_reset;
    h->explicit_ref_marking = h1->explicit_ref_marking;

    for (i = 0; i < h->view_count; i++) {
        copy_picture_range(h->views[i].short_ref, h1->views[i].short_ref, 32, h, h1);
        copy_picture_range(h->views[i].long_ref, h1->views[i].long_ref, 32, h, h1);
        copy_picture_range(h->views[i].delayed_pic, h1->views[i].delayed_pic,
                           FF_ARRAY_ELEMS(h->views[i].delayed_pic), h, h1);
        /* next_output_pic and parked_pic are deliberately NOT cloned or
         * rebased: only the context that committed a picture can emit it
         * (see the view-copy loop above). */
    }

    h->frame_recovered       = h1->frame_recovered;

    ret = ff_h2645_sei_ctx_replace(&h->sei.common, &h1->sei.common);
    if (ret < 0)
        return ret;

    h->sei.common.unregistered.x264_build = h1->sei.common.unregistered.x264_build;

    if (!h->cur_pic_ptr)
        return 0;

    if (!h->droppable) {
        err = ff_h264_execute_ref_pic_marking(h);
    }
    {
        H264ViewState *v = &h->views[h->cur_view];
        v->poc.prev_poc_msb = v->poc.poc_msb;
        v->poc.prev_poc_lsb = v->poc.poc_lsb;
        v->poc.prev_frame_num_offset = v->poc.frame_num_offset;
        v->poc.prev_frame_num        = v->poc.frame_num;
    }

    h->recovery_frame        = h1->recovery_frame;
    h->non_gray              = h1->non_gray;

    return err;
}

int ff_h264_update_thread_context_for_user(AVCodecContext *dst,
                                           const AVCodecContext *src)
{
    H264Context *h = dst->priv_data;
    const H264Context *h1 = src->priv_data;

    h->is_avc = h1->is_avc;
    h->nal_length_size = h1->nal_length_size;

    return 0;
}

static int h264_frame_start(H264Context *h)
{
    H264Picture *pic;
    int i, ret;
    const int pixel_shift = h->pixel_shift;

    if (!ff_thread_can_start_frame(h->avctx)) {
        av_log(h->avctx, AV_LOG_ERROR, "Attempt to start a frame outside SETUP state\n");
        return AVERROR_BUG;
    }

    release_unused_pictures(h, 1);
    h->cur_pic_ptr = NULL;

    i = find_unused_picture(h);
    if (i < 0) {
        av_log(h->avctx, AV_LOG_ERROR, "no frame buffer available\n");
        return i;
    }
    pic = &h->DPB[i];

    H264ViewState *v = &h->views[h->cur_view];

    pic->reference              = h->droppable ? 0 : h->picture_structure;
    pic->field_picture          = h->picture_structure != PICT_FRAME;
    pic->frame_num               = v->poc.frame_num;
    pic->view_id                 = v->view_id;
    pic->view_idx                = h->cur_view;
    /* The picture slot was re-used from the DPB: reset the multiview
     * output latches a previous occupant may have left (the tail unref
     * does not clear them). Give the new occupant a fresh identity token so
     * any stale raw pointer into this slot (the output-band duplicate
     * source) can be recognized instead of following the slot's new owner. */
    pic->output_delivered        = 0;
    pic->flush_old_epoch         = 0;
    pic->output_omit             = 0;
    pic->output_dup_before       = 0;
    pic->output_dup_done         = 0;
    pic->output_dup_src          = NULL;
    pic->output_dup_epoch        = 0;
    pic->slot_epoch              = ++h->pic_slot_epoch;
    /*
     * Zero key_frame here; IDR markings per slice in frame or fields are ORed
     * in later.
     * See decode_nal_units().
     */
    pic->f->flags   &= ~AV_FRAME_FLAG_KEY;
    pic->mmco_reset  = 0;
    pic->recovered   = 0;
    pic->invalid_gap = 0;
    pic->sei_recovery_frame_cnt = h->sei.recovery_point.recovery_frame_cnt;

    pic->f->pict_type = h->slice_ctx[0].slice_type;

    pic->f->crop_left   = h->crop_left;
    pic->f->crop_right  = h->crop_right;
    pic->f->crop_top    = h->crop_top;
    pic->f->crop_bottom = h->crop_bottom;

    pic->needs_fg =
        h->sei.common.film_grain_characteristics &&
        h->sei.common.film_grain_characteristics->present &&
        !h->avctx->hwaccel &&
        !(h->avctx->export_side_data & AV_CODEC_EXPORT_DATA_FILM_GRAIN);

    if ((ret = alloc_picture(h, pic)) < 0)
        return ret;

    /* Multiview: capture the dts of the access unit this picture was
     * decoded from so it survives until the (possibly much later) output
     * of the picture. Single view: leave pkt_dts unset - finalize_frame()
     * reproduces the baseline decode.c behavior (frame->pkt_dts = pkt->dts). */
    if (h->view_count > 1)
        pic->f->pkt_dts = h->pkt_dts;

    h->cur_pic_ptr = pic;

    ff_h264_unref_picture(&h->cur_pic);
    if (CONFIG_ERROR_RESILIENCE) {
        ff_h264_set_erpic(&h->er.cur_pic, NULL);
    }

    if ((ret = ff_h264_ref_picture(&h->cur_pic, h->cur_pic_ptr)) < 0)
        return ret;

    for (i = 0; i < h->nb_slice_ctx; i++) {
        h->slice_ctx[i].linesize   = h->cur_pic_ptr->f->linesize[0];
        h->slice_ctx[i].uvlinesize = h->cur_pic_ptr->f->linesize[1];
    }

    if (CONFIG_ERROR_RESILIENCE && h->enable_er) {
        ff_er_frame_start(&h->er);
        ff_h264_set_erpic(&h->er.last_pic, NULL);
        ff_h264_set_erpic(&h->er.next_pic, NULL);
    }

    for (i = 0; i < 16; i++) {
        h->block_offset[i]           = (4 * ((scan8[i] - scan8[0]) & 7) << pixel_shift) + 4 * pic->f->linesize[0] * ((scan8[i] - scan8[0]) >> 3);
        h->block_offset[48 + i]      = (4 * ((scan8[i] - scan8[0]) & 7) << pixel_shift) + 8 * pic->f->linesize[0] * ((scan8[i] - scan8[0]) >> 3);
    }
    for (i = 0; i < 16; i++) {
        h->block_offset[16 + i]      =
        h->block_offset[32 + i]      = (4 * ((scan8[i] - scan8[0]) & 7) << pixel_shift) + 4 * pic->f->linesize[1] * ((scan8[i] - scan8[0]) >> 3);
        h->block_offset[48 + 16 + i] =
        h->block_offset[48 + 32 + i] = (4 * ((scan8[i] - scan8[0]) & 7) << pixel_shift) + 8 * pic->f->linesize[1] * ((scan8[i] - scan8[0]) >> 3);
    }

    /* We mark the current picture as non-reference after allocating it, so
     * that if we break out due to an error it can be released automatically
     * in the next ff_mpv_frame_start().
     */
    h->cur_pic_ptr->reference = 0;

    h->cur_pic_ptr->field_poc[0] = h->cur_pic_ptr->field_poc[1] = INT_MAX;

    v->next_output_pic = NULL;

    h->postpone_filter = 0;
    h->dep_fill_first  = -1;

    h->mb_aff_frame = h->ps.sps->mb_aff && (h->picture_structure == PICT_FRAME);

    if (h->sei.common.unregistered.x264_build >= 0)
        h->x264_build = h->sei.common.unregistered.x264_build;

    assert(h->cur_pic_ptr->long_ref == 0);

    return 0;
}

static av_always_inline void backup_mb_border(const H264Context *h, H264SliceContext *sl,
                                              const uint8_t *src_y,
                                              const uint8_t *src_cb, const uint8_t *src_cr,
                                              int linesize, int uvlinesize,
                                              int simple)
{
    uint8_t *top_border;
    int top_idx = 1;
    const int pixel_shift = h->pixel_shift;
    int chroma444 = CHROMA444(h);
    int chroma422 = CHROMA422(h);

    src_y  -= linesize;
    src_cb -= uvlinesize;
    src_cr -= uvlinesize;

    if (!simple && FRAME_MBAFF(h)) {
        if (sl->mb_y & 1) {
            if (!MB_MBAFF(sl)) {
                top_border = sl->top_borders[0][sl->mb_x];
                AV_COPY128(top_border, src_y + 15 * linesize);
                if (pixel_shift)
                    AV_COPY128(top_border + 16, src_y + 15 * linesize + 16);
                if (simple || !CONFIG_GRAY || !(h->flags & AV_CODEC_FLAG_GRAY)) {
                    if (chroma444) {
                        if (pixel_shift) {
                            AV_COPY128(top_border + 32, src_cb + 15 * uvlinesize);
                            AV_COPY128(top_border + 48, src_cb + 15 * uvlinesize + 16);
                            AV_COPY128(top_border + 64, src_cr + 15 * uvlinesize);
                            AV_COPY128(top_border + 80, src_cr + 15 * uvlinesize + 16);
                        } else {
                            AV_COPY128(top_border + 16, src_cb + 15 * uvlinesize);
                            AV_COPY128(top_border + 32, src_cr + 15 * uvlinesize);
                        }
                    } else if (chroma422) {
                        if (pixel_shift) {
                            AV_COPY128(top_border + 32, src_cb + 15 * uvlinesize);
                            AV_COPY128(top_border + 48, src_cr + 15 * uvlinesize);
                        } else {
                            AV_COPY64(top_border + 16, src_cb + 15 * uvlinesize);
                            AV_COPY64(top_border + 24, src_cr + 15 * uvlinesize);
                        }
                    } else {
                        if (pixel_shift) {
                            AV_COPY128(top_border + 32, src_cb + 7 * uvlinesize);
                            AV_COPY128(top_border + 48, src_cr + 7 * uvlinesize);
                        } else {
                            AV_COPY64(top_border + 16, src_cb + 7 * uvlinesize);
                            AV_COPY64(top_border + 24, src_cr + 7 * uvlinesize);
                        }
                    }
                }
            }
        } else if (MB_MBAFF(sl)) {
            top_idx = 0;
        } else
            return;
    }

    top_border = sl->top_borders[top_idx][sl->mb_x];
    /* There are two lines saved, the line above the top macroblock
     * of a pair, and the line above the bottom macroblock. */
    AV_COPY128(top_border, src_y + 16 * linesize);
    if (pixel_shift)
        AV_COPY128(top_border + 16, src_y + 16 * linesize + 16);

    if (simple || !CONFIG_GRAY || !(h->flags & AV_CODEC_FLAG_GRAY)) {
        if (chroma444) {
            if (pixel_shift) {
                AV_COPY128(top_border + 32, src_cb + 16 * linesize);
                AV_COPY128(top_border + 48, src_cb + 16 * linesize + 16);
                AV_COPY128(top_border + 64, src_cr + 16 * linesize);
                AV_COPY128(top_border + 80, src_cr + 16 * linesize + 16);
            } else {
                AV_COPY128(top_border + 16, src_cb + 16 * linesize);
                AV_COPY128(top_border + 32, src_cr + 16 * linesize);
            }
        } else if (chroma422) {
            if (pixel_shift) {
                AV_COPY128(top_border + 32, src_cb + 16 * uvlinesize);
                AV_COPY128(top_border + 48, src_cr + 16 * uvlinesize);
            } else {
                AV_COPY64(top_border + 16, src_cb + 16 * uvlinesize);
                AV_COPY64(top_border + 24, src_cr + 16 * uvlinesize);
            }
        } else {
            if (pixel_shift) {
                AV_COPY128(top_border + 32, src_cb + 8 * uvlinesize);
                AV_COPY128(top_border + 48, src_cr + 8 * uvlinesize);
            } else {
                AV_COPY64(top_border + 16, src_cb + 8 * uvlinesize);
                AV_COPY64(top_border + 24, src_cr + 8 * uvlinesize);
            }
        }
    }
}

/**
 * Initialize implicit_weight table.
 * @param field  0/1 initialize the weight for interlaced MBAFF
 *                -1 initializes the rest
 */
static void implicit_weight_table(const H264Context *h, H264SliceContext *sl, int field)
{
    int ref0, ref1, i, cur_poc, ref_start, ref_count0, ref_count1;

    for (i = 0; i < 2; i++) {
        sl->pwt.luma_weight_flag[i]   = 0;
        sl->pwt.chroma_weight_flag[i] = 0;
    }

    if (field < 0) {
        if (h->picture_structure == PICT_FRAME) {
            cur_poc = h->cur_pic_ptr->poc;
        } else {
            cur_poc = h->cur_pic_ptr->field_poc[h->picture_structure - 1];
        }
        if (sl->ref_count[0] == 1 && sl->ref_count[1] == 1 && !FRAME_MBAFF(h) &&
            sl->ref_list[0][0].poc + (int64_t)sl->ref_list[1][0].poc == 2LL * cur_poc) {
            sl->pwt.use_weight        = 0;
            sl->pwt.use_weight_chroma = 0;
            return;
        }
        ref_start  = 0;
        ref_count0 = sl->ref_count[0];
        ref_count1 = sl->ref_count[1];
    } else {
        cur_poc    = h->cur_pic_ptr->field_poc[field];
        ref_start  = 16;
        ref_count0 = 16 + 2 * sl->ref_count[0];
        ref_count1 = 16 + 2 * sl->ref_count[1];
    }

    sl->pwt.use_weight               = 2;
    sl->pwt.use_weight_chroma        = 2;
    sl->pwt.luma_log2_weight_denom   = 5;
    sl->pwt.chroma_log2_weight_denom = 5;

    for (ref0 = ref_start; ref0 < ref_count0; ref0++) {
        int64_t poc0 = sl->ref_list[0][ref0].poc;
        for (ref1 = ref_start; ref1 < ref_count1; ref1++) {
            int w = 32;
            if (!sl->ref_list[0][ref0].parent->long_ref && !sl->ref_list[1][ref1].parent->long_ref) {
                int poc1 = sl->ref_list[1][ref1].poc;
                int td   = av_clip_int8(poc1 - poc0);
                if (td) {
                    int tb = av_clip_int8(cur_poc - poc0);
                    int tx = (16384 + (FFABS(td) >> 1)) / td;
                    int dist_scale_factor = (tb * tx + 32) >> 8;
                    if (dist_scale_factor >= -64 && dist_scale_factor <= 128)
                        w = 64 - dist_scale_factor;
                }
            }
            if (field < 0) {
                sl->pwt.implicit_weight[ref0][ref1][0] =
                sl->pwt.implicit_weight[ref0][ref1][1] = w;
            } else {
                sl->pwt.implicit_weight[ref0][ref1][field] = w;
            }
        }
    }
}

/**
 * initialize scan tables
 */
static void init_scan_tables(H264Context *h)
{
    int i;
    for (i = 0; i < 16; i++) {
#define TRANSPOSE(x) ((x) >> 2) | (((x) << 2) & 0xF)
        h->zigzag_scan[i] = TRANSPOSE(ff_zigzag_scan[i]);
        h->field_scan[i]  = TRANSPOSE(field_scan[i]);
#undef TRANSPOSE
    }
    for (i = 0; i < 64; i++) {
#define TRANSPOSE(x) ((x) >> 3) | (((x) & 7) << 3)
        h->zigzag_scan8x8[i]       = TRANSPOSE(ff_zigzag_direct[i]);
        h->zigzag_scan8x8_cavlc[i] = TRANSPOSE(zigzag_scan8x8_cavlc[i]);
        h->field_scan8x8[i]        = TRANSPOSE(field_scan8x8[i]);
        h->field_scan8x8_cavlc[i]  = TRANSPOSE(field_scan8x8_cavlc[i]);
#undef TRANSPOSE
    }
    if (h->ps.sps->transform_bypass) { // FIXME same ugly
        memcpy(h->zigzag_scan_q0          , ff_zigzag_scan          , sizeof(h->zigzag_scan_q0         ));
        memcpy(h->zigzag_scan8x8_q0       , ff_zigzag_direct        , sizeof(h->zigzag_scan8x8_q0      ));
        memcpy(h->zigzag_scan8x8_cavlc_q0 , zigzag_scan8x8_cavlc    , sizeof(h->zigzag_scan8x8_cavlc_q0));
        memcpy(h->field_scan_q0           , field_scan              , sizeof(h->field_scan_q0          ));
        memcpy(h->field_scan8x8_q0        , field_scan8x8           , sizeof(h->field_scan8x8_q0       ));
        memcpy(h->field_scan8x8_cavlc_q0  , field_scan8x8_cavlc     , sizeof(h->field_scan8x8_cavlc_q0 ));
    } else {
        memcpy(h->zigzag_scan_q0          , h->zigzag_scan          , sizeof(h->zigzag_scan_q0         ));
        memcpy(h->zigzag_scan8x8_q0       , h->zigzag_scan8x8       , sizeof(h->zigzag_scan8x8_q0      ));
        memcpy(h->zigzag_scan8x8_cavlc_q0 , h->zigzag_scan8x8_cavlc , sizeof(h->zigzag_scan8x8_cavlc_q0));
        memcpy(h->field_scan_q0           , h->field_scan           , sizeof(h->field_scan_q0          ));
        memcpy(h->field_scan8x8_q0        , h->field_scan8x8        , sizeof(h->field_scan8x8_q0       ));
        memcpy(h->field_scan8x8_cavlc_q0  , h->field_scan8x8_cavlc  , sizeof(h->field_scan8x8_cavlc_q0 ));
    }
}

static enum AVPixelFormat get_pixel_format(H264Context *h, int force_callback)
{
#define HWACCEL_MAX (CONFIG_H264_DXVA2_HWACCEL + \
                     (CONFIG_H264_D3D11VA_HWACCEL * 2) + \
                     CONFIG_H264_D3D12VA_HWACCEL + \
                     CONFIG_H264_NVDEC_HWACCEL + \
                     CONFIG_H264_VAAPI_HWACCEL + \
                     CONFIG_H264_VIDEOTOOLBOX_HWACCEL + \
                     CONFIG_H264_VDPAU_HWACCEL + \
                     CONFIG_H264_VULKAN_HWACCEL)
    enum AVPixelFormat pix_fmts[HWACCEL_MAX + 2], *fmt = pix_fmts;

    switch (h->ps.sps->bit_depth_luma) {
    case 9:
        if (CHROMA444(h)) {
            if (h->avctx->colorspace == AVCOL_SPC_RGB) {
                *fmt++ = AV_PIX_FMT_GBRP9;
            } else
                *fmt++ = AV_PIX_FMT_YUV444P9;
        } else if (CHROMA422(h))
            *fmt++ = AV_PIX_FMT_YUV422P9;
        else
            *fmt++ = AV_PIX_FMT_YUV420P9;
        break;
    case 10:
#if CONFIG_H264_VIDEOTOOLBOX_HWACCEL
        if (h->avctx->colorspace != AVCOL_SPC_RGB)
            *fmt++ = AV_PIX_FMT_VIDEOTOOLBOX;
#endif
#if CONFIG_H264_VULKAN_HWACCEL
        *fmt++ = AV_PIX_FMT_VULKAN;
#endif
#if CONFIG_H264_NVDEC_HWACCEL
        *fmt++ = AV_PIX_FMT_CUDA;
#endif
        if (CHROMA444(h)) {
            if (h->avctx->colorspace == AVCOL_SPC_RGB) {
                *fmt++ = AV_PIX_FMT_GBRP10;
            } else
                *fmt++ = AV_PIX_FMT_YUV444P10;
        } else if (CHROMA422(h))
            *fmt++ = AV_PIX_FMT_YUV422P10;
        else {
#if CONFIG_H264_VAAPI_HWACCEL
            // Just add as candidate. Whether VAProfileH264High10 usable or
            // not is decided by vaapi_decode_make_config() defined in FFmpeg
            // and vaQueryCodingProfile() defined in libva.
            *fmt++ = AV_PIX_FMT_VAAPI;
#endif
            *fmt++ = AV_PIX_FMT_YUV420P10;
        }
        break;
    case 12:
#if CONFIG_H264_VULKAN_HWACCEL
        *fmt++ = AV_PIX_FMT_VULKAN;
#endif
        if (CHROMA444(h)) {
            if (h->avctx->colorspace == AVCOL_SPC_RGB) {
                *fmt++ = AV_PIX_FMT_GBRP12;
            } else
                *fmt++ = AV_PIX_FMT_YUV444P12;
        } else if (CHROMA422(h))
            *fmt++ = AV_PIX_FMT_YUV422P12;
        else
            *fmt++ = AV_PIX_FMT_YUV420P12;
        break;
    case 14:
        if (CHROMA444(h)) {
            if (h->avctx->colorspace == AVCOL_SPC_RGB) {
                *fmt++ = AV_PIX_FMT_GBRP14;
            } else
                *fmt++ = AV_PIX_FMT_YUV444P14;
        } else if (CHROMA422(h))
            *fmt++ = AV_PIX_FMT_YUV422P14;
        else
            *fmt++ = AV_PIX_FMT_YUV420P14;
        break;
    case 8:
#if CONFIG_H264_VDPAU_HWACCEL
        *fmt++ = AV_PIX_FMT_VDPAU;
#endif
#if CONFIG_H264_VULKAN_HWACCEL
        *fmt++ = AV_PIX_FMT_VULKAN;
#endif
#if CONFIG_H264_NVDEC_HWACCEL
        *fmt++ = AV_PIX_FMT_CUDA;
#endif
#if CONFIG_H264_VIDEOTOOLBOX_HWACCEL
        if (h->avctx->colorspace != AVCOL_SPC_RGB)
            *fmt++ = AV_PIX_FMT_VIDEOTOOLBOX;
#endif
        if (CHROMA444(h)) {
            if (h->avctx->colorspace == AVCOL_SPC_RGB)
                *fmt++ = AV_PIX_FMT_GBRP;
            else if (h->avctx->color_range == AVCOL_RANGE_JPEG)
                *fmt++ = AV_PIX_FMT_YUVJ444P;
            else
                *fmt++ = AV_PIX_FMT_YUV444P;
        } else if (CHROMA422(h)) {
            if (h->avctx->color_range == AVCOL_RANGE_JPEG)
                *fmt++ = AV_PIX_FMT_YUVJ422P;
            else
                *fmt++ = AV_PIX_FMT_YUV422P;
        } else {
#if CONFIG_H264_DXVA2_HWACCEL
            *fmt++ = AV_PIX_FMT_DXVA2_VLD;
#endif
#if CONFIG_H264_D3D11VA_HWACCEL
            *fmt++ = AV_PIX_FMT_D3D11VA_VLD;
            *fmt++ = AV_PIX_FMT_D3D11;
#endif
#if CONFIG_H264_D3D12VA_HWACCEL
            *fmt++ = AV_PIX_FMT_D3D12;
#endif
#if CONFIG_H264_VAAPI_HWACCEL
            *fmt++ = AV_PIX_FMT_VAAPI;
#endif
            if (h->avctx->color_range == AVCOL_RANGE_JPEG)
                *fmt++ = AV_PIX_FMT_YUVJ420P;
            else
                *fmt++ = AV_PIX_FMT_YUV420P;
        }
        break;
    default:
        av_log(h->avctx, AV_LOG_ERROR,
               "Unsupported bit depth %d\n", h->ps.sps->bit_depth_luma);
        return AVERROR_INVALIDDATA;
    }

    /* H.264/MVC is decoded in software only: with a hwaccel requested,
     * strip the hardware formats so no backend can engage (the side-by-side
     * assembly reads software frames). mvc_sps is the "multiview SPS seen"
     * state kept by h264_mvc_update(); in 2D+delta streams the multiview
     * SPS may arrive after the base profile SPS, so the active SPS alone
     * is not the right test. */
    const int mvc_sw_only = h->mvc_sps &&
                            (h->avctx->hw_device_ctx || h->avctx->hwaccel);
    if (mvc_sw_only) {
        enum AVPixelFormat *w = pix_fmts;

        for (enum AVPixelFormat *r = pix_fmts; r < fmt; r++)
            if (!(av_pix_fmt_desc_get(*r)->flags & AV_PIX_FMT_FLAG_HWACCEL))
                *w++ = *r;
        fmt = w;
    }

    *fmt = AV_PIX_FMT_NONE;

    for (int i = 0; pix_fmts[i] != AV_PIX_FMT_NONE; i++)
        if (pix_fmts[i] == h->avctx->pix_fmt && !force_callback)
            return pix_fmts[i];
    if (mvc_sw_only && !h->mvc_hw_fallback_warned) {
        h->mvc_hw_fallback_warned = 1;
        av_log(h->avctx, AV_LOG_WARNING,
               "H.264/MVC: hardware acceleration is not supported, "
               "falling back to software decoding\n");
    }
    return ff_get_format(h->avctx, pix_fmts);
}

/* export coded and cropped frame dimensions to AVCodecContext */
static void init_dimensions(H264Context *h)
{
    const SPS *sps = h->ps.sps;
    int cr = sps->crop_right;
    int cl = sps->crop_left;
    int ct = sps->crop_top;
    int cb = sps->crop_bottom;
    int width  = h->width  - (cr + cl);
    int height = h->height - (ct + cb);
    av_assert0(sps->crop_right + sps->crop_left < (unsigned)h->width);
    av_assert0(sps->crop_top + sps->crop_bottom < (unsigned)h->height);

    /* handle container cropping */
    if (h->width_from_caller > 0 && h->height_from_caller > 0     &&
        !sps->crop_top && !sps->crop_left                         &&
        FFALIGN(h->width_from_caller,  16) == FFALIGN(width,  16) &&
        FFALIGN(h->height_from_caller, 16) == FFALIGN(height, 16) &&
        h->width_from_caller  <= width &&
        h->height_from_caller <= height) {
        width  = h->width_from_caller;
        height = h->height_from_caller;
        cl = 0;
        ct = 0;
        cr = h->width - width;
        cb = h->height - height;
    } else {
        h->width_from_caller  = 0;
        h->height_from_caller = 0;
    }

    h->avctx->coded_width  = h->width;
    h->avctx->coded_height = h->height;
    h->avctx->width        = width;
    h->avctx->height       = height;
    h->crop_right          = cr;
    h->crop_left           = cl;
    h->crop_top            = ct;
    h->crop_bottom         = cb;
}

static int h264_slice_header_init(H264Context *h)
{
    const SPS *sps = h->ps.sps;
    int i, ret;

    if (!sps) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    ff_set_sar(h->avctx, sps->vui.sar);
    av_pix_fmt_get_chroma_sub_sample(h->avctx->pix_fmt,
                                     &h->chroma_x_shift, &h->chroma_y_shift);

    if (sps->timing_info_present_flag) {
        int64_t den = sps->time_scale;
        if (h->x264_build < 44U)
            den *= 2;
        av_reduce(&h->avctx->framerate.den, &h->avctx->framerate.num,
                  sps->num_units_in_tick * 2, den, 1 << 30);
    }

    ff_h264_free_tables(h);

    h->first_field           = 0;
    h->prev_interlaced_frame = 1;

    init_scan_tables(h);
    ret = ff_h264_alloc_tables(h);
    if (ret < 0) {
        av_log(h->avctx, AV_LOG_ERROR, "Could not allocate memory\n");
        goto fail;
    }

    if (sps->bit_depth_luma < 8 || sps->bit_depth_luma > 14 ||
        sps->bit_depth_luma == 11 || sps->bit_depth_luma == 13
    ) {
        av_log(h->avctx, AV_LOG_ERROR, "Unsupported bit depth %d\n",
               sps->bit_depth_luma);
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    h->cur_bit_depth_luma         =
    h->avctx->bits_per_raw_sample = sps->bit_depth_luma;
    h->cur_chroma_format_idc      = sps->chroma_format_idc;
    h->pixel_shift                = sps->bit_depth_luma > 8;
    h->chroma_format_idc          = sps->chroma_format_idc;
    h->bit_depth_luma             = sps->bit_depth_luma;

    ff_h264dsp_init(&h->h264dsp, sps->bit_depth_luma,
                    sps->chroma_format_idc);
    ff_h264chroma_init(&h->h264chroma, sps->bit_depth_chroma);
    ff_h264qpel_init(&h->h264qpel, sps->bit_depth_luma);
    ff_h264_pred_init(&h->hpc, AV_CODEC_ID_H264, sps->bit_depth_luma,
                      sps->chroma_format_idc);
    ff_videodsp_init(&h->vdsp, sps->bit_depth_luma);

    if (!HAVE_THREADS || !(h->avctx->active_thread_type & FF_THREAD_SLICE)) {
        ff_h264_slice_context_init(h, &h->slice_ctx[0]);
    } else {
        for (i = 0; i < h->nb_slice_ctx; i++) {
            H264SliceContext *sl = &h->slice_ctx[i];

            sl->h264               = h;
            sl->intra4x4_pred_mode = h->intra4x4_pred_mode + i * 8 * 2 * h->mb_stride;
            sl->mvd_table[0]       = h->mvd_table[0]       + i * 8 * 2 * h->mb_stride;
            sl->mvd_table[1]       = h->mvd_table[1]       + i * 8 * 2 * h->mb_stride;

            ff_h264_slice_context_init(h, sl);
        }
    }

    h->context_initialized = 1;

    return 0;
fail:
    ff_h264_free_tables(h);
    h->context_initialized = 0;
    return ret;
}

static enum AVPixelFormat non_j_pixfmt(enum AVPixelFormat a)
{
    switch (a) {
    case AV_PIX_FMT_YUVJ420P: return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: return AV_PIX_FMT_YUV444P;
    default:
        return a;
    }
}

static int h264_init_ps(H264Context *h, const H264SliceContext *sl, int first_slice)
{
    const SPS *sps;
    int needs_reinit = 0, must_reinit, ret;

    if (first_slice)
        av_refstruct_replace(&h->ps.pps, h->ps.pps_list[sl->pps_id]);

    if (h->ps.sps != h->ps.pps->sps) {
        h->ps.sps = h->ps.pps->sps;

        if (h->mb_width  != h->ps.sps->mb_width ||
            h->mb_height != h->ps.sps->mb_height ||
            h->cur_bit_depth_luma    != h->ps.sps->bit_depth_luma ||
            h->cur_chroma_format_idc != h->ps.sps->chroma_format_idc
        )
            needs_reinit = 1;

        if (h->bit_depth_luma    != h->ps.sps->bit_depth_luma ||
            h->chroma_format_idc != h->ps.sps->chroma_format_idc)
            needs_reinit         = 1;
    }
    sps = h->ps.sps;

    must_reinit = (h->context_initialized &&
                    (   16*sps->mb_width != h->avctx->coded_width
                     || 16*sps->mb_height != h->avctx->coded_height
                     || h->cur_bit_depth_luma    != sps->bit_depth_luma
                     || h->cur_chroma_format_idc != sps->chroma_format_idc
                     || h->mb_width  != sps->mb_width
                     || h->mb_height != sps->mb_height
                    ));
    if (h->avctx->pix_fmt == AV_PIX_FMT_NONE
        || (non_j_pixfmt(h->avctx->pix_fmt) != non_j_pixfmt(get_pixel_format(h, 0))))
        must_reinit = 1;

    if (first_slice && av_cmp_q(sps->vui.sar, h->avctx->sample_aspect_ratio))
        must_reinit = 1;

    if (!h->setup_finished) {
        h->avctx->profile = ff_h264_get_profile(sps);
        h->avctx->level   = sps->level_idc;
        h->avctx->refs    = sps->ref_frame_count;

        h->mb_width  = sps->mb_width;
        h->mb_height = sps->mb_height;
        h->mb_num    = h->mb_width * h->mb_height;
        h->mb_stride = h->mb_width + 1;

        h->b_stride = h->mb_width * 4;

        h->chroma_y_shift = sps->chroma_format_idc <= 1; // 400 uses yuv420p

        h->width  = 16 * h->mb_width;
        h->height = 16 * h->mb_height;

        init_dimensions(h);

        if (sps->vui.video_signal_type_present_flag) {
            h->avctx->color_range = sps->vui.video_full_range_flag > 0 ? AVCOL_RANGE_JPEG
                                                                       : AVCOL_RANGE_MPEG;
            if (sps->vui.colour_description_present_flag) {
                if (h->avctx->colorspace != sps->vui.matrix_coeffs)
                    needs_reinit = 1;
                h->avctx->color_primaries = sps->vui.colour_primaries;
                h->avctx->color_trc       = sps->vui.transfer_characteristics;
                h->avctx->colorspace      = sps->vui.matrix_coeffs;
            }
        }

        if (h->sei.common.alternative_transfer.present &&
            av_color_transfer_name(h->sei.common.alternative_transfer.preferred_transfer_characteristics) &&
            h->sei.common.alternative_transfer.preferred_transfer_characteristics != AVCOL_TRC_UNSPECIFIED) {
            h->avctx->color_trc = h->sei.common.alternative_transfer.preferred_transfer_characteristics;
        }
    }
    h->avctx->chroma_sample_location = sps->vui.chroma_location;

    if (!h->context_initialized || must_reinit || needs_reinit) {
        int flush_changes = h->context_initialized;
        h->context_initialized = 0;
        if (sl != h->slice_ctx) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "changing width %d -> %d / height %d -> %d on "
                   "slice %d\n",
                   h->width, h->avctx->coded_width,
                   h->height, h->avctx->coded_height,
                   h->current_slice + 1);
            return AVERROR_INVALIDDATA;
        }

        av_assert1(first_slice);

        if (flush_changes)
            ff_h264_flush_change(h);

        if ((ret = get_pixel_format(h, 1)) < 0)
            return ret;
        h->avctx->pix_fmt = ret;

        av_log(h->avctx, AV_LOG_VERBOSE, "Reinit context to %dx%d, "
               "pix_fmt: %s\n", h->width, h->height, av_get_pix_fmt_name(h->avctx->pix_fmt));

        if ((ret = h264_slice_header_init(h)) < 0) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "h264_slice_header_init() failed\n");
            return ret;
        }
    }

    return 0;
}

static int h264_export_frame_props(H264Context *h)
{
    const SPS *sps = h->ps.sps;
    H264Picture *cur = h->cur_pic_ptr;
    AVFrame *out = cur->f;
    int interlaced_frame = 0, top_field_first = 0;
    int ret;

    out->flags &= ~AV_FRAME_FLAG_INTERLACED;
    out->repeat_pict      = 0;

    /* Signal interlacing information externally. */
    /* Prioritize picture timing SEI information over used
     * decoding process if it exists. */
    if (h->sei.picture_timing.present) {
        int ret = ff_h264_sei_process_picture_timing(&h->sei.picture_timing, sps,
                                                     h->avctx);
        if (ret < 0) {
            av_log(h->avctx, AV_LOG_ERROR, "Error processing a picture timing SEI\n");
            if (h->avctx->err_recognition & AV_EF_EXPLODE)
                return ret;
            h->sei.picture_timing.present = 0;
        }
    }

    if (sps->pic_struct_present_flag && h->sei.picture_timing.present) {
        const H264SEIPictureTiming *pt = &h->sei.picture_timing;
        switch (pt->pic_struct) {
        case H264_SEI_PIC_STRUCT_FRAME:
            break;
        case H264_SEI_PIC_STRUCT_TOP_FIELD:
        case H264_SEI_PIC_STRUCT_BOTTOM_FIELD:
            interlaced_frame = 1;
            break;
        case H264_SEI_PIC_STRUCT_TOP_BOTTOM:
        case H264_SEI_PIC_STRUCT_BOTTOM_TOP:
            if (FIELD_OR_MBAFF_PICTURE(h))
                interlaced_frame = 1;
            else
                // try to flag soft telecine progressive
                interlaced_frame = !!h->prev_interlaced_frame;
            break;
        case H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP:
        case H264_SEI_PIC_STRUCT_BOTTOM_TOP_BOTTOM:
            /* Signal the possibility of telecined film externally
             * (pic_struct 5,6). From these hints, let the applications
             * decide if they apply deinterlacing. */
            out->repeat_pict = 1;
            break;
        case H264_SEI_PIC_STRUCT_FRAME_DOUBLING:
            out->repeat_pict = 2;
            break;
        case H264_SEI_PIC_STRUCT_FRAME_TRIPLING:
            out->repeat_pict = 4;
            break;
        }

        if ((pt->ct_type & 3) &&
            pt->pic_struct <= H264_SEI_PIC_STRUCT_BOTTOM_TOP)
            interlaced_frame = ((pt->ct_type & (1 << 1)) != 0);
    } else {
        /* Derive interlacing flag from used decoding process. */
        interlaced_frame = !!FIELD_OR_MBAFF_PICTURE(h);
    }
    h->prev_interlaced_frame = interlaced_frame;

    if (cur->field_poc[0] != cur->field_poc[1]) {
        /* Derive top_field_first from field pocs. */
        top_field_first = (cur->field_poc[0] < cur->field_poc[1]);
    } else {
        if (sps->pic_struct_present_flag && h->sei.picture_timing.present) {
            /* Use picture timing SEI information. Even if it is a
             * information of a past frame, better than nothing. */
            if (h->sei.picture_timing.pic_struct == H264_SEI_PIC_STRUCT_TOP_BOTTOM ||
                h->sei.picture_timing.pic_struct == H264_SEI_PIC_STRUCT_TOP_BOTTOM_TOP)
                top_field_first = 1;
        } else if (interlaced_frame) {
            /* Default to top field first when pic_struct_present_flag
             * is not set but interlaced frame detected */
            top_field_first = 1;
        } // else
            /* Most likely progressive */
    }

    out->flags |= (AV_FRAME_FLAG_INTERLACED * interlaced_frame) |
                  (AV_FRAME_FLAG_TOP_FIELD_FIRST * top_field_first);

    ret = ff_h2645_sei_to_frame(out, &h->sei.common, AV_CODEC_ID_H264, h->avctx,
                                &sps->vui, sps->bit_depth_luma, sps->bit_depth_chroma,
                                cur->poc + (unsigned)(h->poc_offset << 5));
    if (ret < 0)
        return ret;

    if (h->sei.picture_timing.timecode_cnt > 0) {
        uint32_t *tc_sd;
        char tcbuf[AV_TIMECODE_STR_SIZE];
        AVFrameSideData *tcside;
        ret = ff_frame_new_side_data(h->avctx, out, AV_FRAME_DATA_S12M_TIMECODE,
                                     sizeof(uint32_t)*4, &tcside);
        if (ret < 0)
            return ret;

        if (tcside) {
            tc_sd = (uint32_t*)tcside->data;
            tc_sd[0] = h->sei.picture_timing.timecode_cnt;

            for (int i = 0; i < tc_sd[0]; i++) {
                int drop = h->sei.picture_timing.timecode[i].dropframe;
                int   hh = h->sei.picture_timing.timecode[i].hours;
                int   mm = h->sei.picture_timing.timecode[i].minutes;
                int   ss = h->sei.picture_timing.timecode[i].seconds;
                int   ff = h->sei.picture_timing.timecode[i].frame;

                tc_sd[i + 1] = av_timecode_get_smpte(h->avctx->framerate, drop, hh, mm, ss, ff);
                av_timecode_make_smpte_tc_string2(tcbuf, h->avctx->framerate, tc_sd[i + 1], 0, 0);
                av_dict_set(&out->metadata, "timecode", tcbuf, 0);
            }
        }
        h->sei.picture_timing.timecode_cnt = 0;
    }

    return 0;
}

/* Inter-view anchor queue depth for user-unselected views: their most
 * recent pictures stay in delayed_pic so the dependent half of the same
 * access unit can still resolve its Annex E inter-view anchor. */
#define H264_MVC_IV_QUEUE_DEPTH 4

/**
 * Multiview: pick the output picture from a view's delayed picture queue:
 * the candidates are the entries preceding the first AV_FRAME_FLAG_KEY /
 * mmco_reset barrier; within them, pictures flagged flush_old_epoch (the
 * reorder tail of a previous POC epoch, set on a deep wrap by
 * h264_select_output_frame()) win over the new epoch's pictures and are
 * delivered in POC order ahead of it, and within each class the lowest
 * POC wins. A flag-free queue yields the plain lowest-POC entry, so
 * unflagged behaviour stays bit-identical to the original picker.
 *
 * @param v view whose queue is scanned (must be non-empty)
 * @return  index of the chosen entry in v->delayed_pic
 */
int ff_h264_mv_queue_pick(const H264ViewState *v)
{
    int out_idx  = 0;
    int out_rank = v->delayed_pic[0]->flush_old_epoch ? 0 : 1;
    int out_poc  = v->delayed_pic[0]->poc;
    int i;

    for (i = 1; v->delayed_pic[i] &&
                !(v->delayed_pic[i]->f->flags & AV_FRAME_FLAG_KEY) &&
                !v->delayed_pic[i]->mmco_reset;
         i++) {
        int rank = v->delayed_pic[i]->flush_old_epoch ? 0 : 1;

        if (rank < out_rank ||
            (rank == out_rank && v->delayed_pic[i]->poc < out_poc)) {
            out_idx  = i;
            out_rank = rank;
            out_poc  = v->delayed_pic[i]->poc;
        }
    }
    return out_idx;
}

static int h264_select_output_frame(H264Context *h)
{
    const SPS *sps = h->ps.sps;
    H264ViewState *v = &h->views[h->cur_view];
    H264Picture *out = h->cur_pic_ptr;
    H264Picture *cur = h->cur_pic_ptr;
    int i, pics, out_of_order, out_idx;

    cur->mmco_reset = h->mmco_reset;
    h->mmco_reset = 0;

    /* Pictures of an unselected view (view_ids) are never output: park
     * them pinned in the view's delayed queue (bounded by
     * H264_MVC_IV_QUEUE_DEPTH) so the inter-view anchor lookup (Annex E,
     * h264_find_inter_view_ref) can still find the unmarked base picture
     * of the same access unit when the dependent half starts decoding.
     * Reference-marked members survive in the short-term reference list
     * regardless; delivery is unaffected - the output loop skips
     * unselected views. */
    if (h->view_count > 1 && !h264_view_selected(h, h->cur_view)) {
        int count = 0, queued = 0;

        while (v->delayed_pic[count]) {
            if (v->delayed_pic[count] == cur)
                queued = 1;
            count++;
        }
        if (!queued) {
            while (count >= H264_MVC_IV_QUEUE_DEPTH) {
                H264Picture *old = v->delayed_pic[0];

                old->reference &= ~DELAYED_PIC_REF;
                for (i = 0; v->delayed_pic[i]; i++)
                    v->delayed_pic[i] = v->delayed_pic[i + 1];
                count--;
            }
            v->delayed_pic[count]     = cur;
            v->delayed_pic[count + 1] = NULL;
        }
        if (cur->reference == 0)
            cur->reference = DELAYED_PIC_REF;
        return 0;
    }

    if (sps->bitstream_restriction_flag ||
        h->avctx->strict_std_compliance >= FF_COMPLIANCE_STRICT) {
        h->avctx->has_b_frames = FFMAX(h->avctx->has_b_frames, sps->num_reorder_frames);
    }

    for (i = 0; 1; i++) {
        if(i == H264_MAX_DPB_FRAMES || cur->poc < v->last_pocs[i]){
            if(i)
                v->last_pocs[i-1] = cur->poc;
            break;
        } else if(i) {
            v->last_pocs[i-1]= v->last_pocs[i];
        }
    }
    out_of_order = H264_MAX_DPB_FRAMES - i;
    if(   cur->f->pict_type == AV_PICTURE_TYPE_B
       || (v->last_pocs[H264_MAX_DPB_FRAMES-2] > INT_MIN && v->last_pocs[H264_MAX_DPB_FRAMES-1] - (int64_t)v->last_pocs[H264_MAX_DPB_FRAMES-2] > 2))
        out_of_order = FFMAX(out_of_order, 1);
    if (out_of_order == H264_MAX_DPB_FRAMES) {
        av_log(h->avctx, AV_LOG_VERBOSE, "Invalid POC %d<%d\n", cur->poc, v->last_pocs[0]);
        for (i = 1; i < H264_MAX_DPB_FRAMES; i++)
            v->last_pocs[i] = INT_MIN;
        v->last_pocs[0] = cur->poc;
        cur->mmco_reset = 1;
    } else if(h->avctx->has_b_frames < out_of_order && !sps->bitstream_restriction_flag){
        int loglevel = h->avctx->frame_num > 1 ? AV_LOG_WARNING : AV_LOG_VERBOSE;
        av_log(h->avctx, loglevel, "Increasing reorder buffer to %d\n", out_of_order);
        h->avctx->has_b_frames = out_of_order;
    }

    {
        int queued = 0;

        pics = 0;
        while (pics < FF_ARRAY_ELEMS(v->delayed_pic) && v->delayed_pic[pics]) {
            if (v->delayed_pic[pics] == cur)
                queued = 1;
            pics++;
        }
        if (pics == FF_ARRAY_ELEMS(v->delayed_pic)) {
            /* A non-terminated queue means the list lost its NULL
             * terminator (corruption); recover by resetting it. */
            static int corrupt_queue_logged;

            if (!corrupt_queue_logged) {
                corrupt_queue_logged = 1;
                av_log(h->avctx, AV_LOG_WARNING,
                       "multiview view %d: delayed picture queue lost "
                       "its terminator; resetting it\n", h->cur_view);
            }
            for (int k = 0; k < FF_ARRAY_ELEMS(v->delayed_pic); k++)
                v->delayed_pic[k] = NULL;
            pics = 0;
        }
        if (queued) {
            /* The queue already names this DPB slot: inserting a second
             * entry would commit (and deliver) the picture twice. Still
             * pin the slot - it belongs to this picture's occupancy. */
            static int dup_queue_slot_logged;

            if (!dup_queue_slot_logged) {
                dup_queue_slot_logged = 1;
                av_log(h->avctx, AV_LOG_INFO,
                       "multiview view %d: picture already queued in the "
                       "delay list (poc %d); skipping duplicate entry\n",
                       h->cur_view, cur->poc);
            }
        } else {
            v->delayed_pic[pics++] = cur;
        }
        if (cur->reference == 0)
            cur->reference = DELAYED_PIC_REF;
    }

    out_idx = ff_h264_mv_queue_pick(v);
    out     = v->delayed_pic[out_idx];
    if (h->avctx->has_b_frames == 0 &&
        ((v->delayed_pic[0]->f->flags & AV_FRAME_FLAG_KEY) || v->delayed_pic[0]->mmco_reset))
        v->next_outputed_poc = INT_MIN;
    /* Multiview: a new POC epoch (wrap) cannot be expressed against the
     * previous epoch's watermark; without a reset, a view whose anchors
     * carry no AV_FRAME_FLAG_KEY (dependent views of these 3D files)
     * would drop its entire new epoch against the stale watermark.
     * Triggers: the queued minimum sits far below the watermark (the
     * observed wrap gap at stream starts, far above any legal B-frame
     * reorder), or the just-arrived picture is itself the queued minimum
     * and older than the watermark (only a new epoch's first picture can
     * sort that low). */
    {
        /* Wrap depth, measured against the watermark BEFORE the reset: it
         * decides whether the queued pictures are the reorder tail of a
         * previous POC epoch (flush) or ordinary in-epoch reorder. */
        unsigned wrap_gap   = 0;
        int wrap_flushed    = 0;
        if (h->view_count > 1 && v->next_outputed_poc != INT_MIN &&
            out->poc < v->next_outputed_poc &&
            (out == cur ||
             (unsigned)(v->next_outputed_poc - out->poc) >
             (unsigned)H264_MAX_DPB_FRAMES * 8)) {
            wrap_gap        = (unsigned)(v->next_outputed_poc - out->poc);
            v->next_outputed_poc = INT_MIN;
            /* A previous-epoch picture still queued behind the new epoch's
             * minimum would sort above every new-epoch picture and stay
             * committed before the new POC overtakes it. Two cases trip
             * the wrap test: a true epoch restart (gap far above any
             * legal reorder) and a mid-epoch reorder dip (bounded by the
             * reorder depth). Only in the first case are the queued
             * pictures above the new minimum the old epoch's reorder
             * tail: flag them - ff_h264_mv_queue_pick delivers
             * flush-flagged pictures in POC order ahead of the new epoch -
             * instead of dropping real decoded content; in the second
             * case they belong to the current epoch and stay unflagged. */
            if (wrap_gap > (unsigned)h->avctx->has_b_frames + 1) {
                int j = 0;

                while (v->delayed_pic[j]) {
                    H264Picture *p = v->delayed_pic[j];

                    if (p != out && p->poc > out->poc)
                        p->flush_old_epoch = 1;
                    j++;
                }
                /* A parked picture from the previous POC epoch would never
                 * be re-emitted by the plain condition (and the next park
                 * overwrite would drop it): flag it - the re-emit path
                 * honours the flag. */
                if (v->parked_pic && v->parked_pic->poc > out->poc)
                    v->parked_pic->flush_old_epoch = 1;
                wrap_flushed = 1;
            }
        }
        if (wrap_flushed) {
            /* The flush flags were just raised, so the entry that now
             * sorts first is the old epoch's tail, not the pre-flag pick:
             * re-pick so the tail is committed (and emitted) at this field
             * end, ahead of every new-epoch picture (the pre-flag pick
             * would deliver out of pts order). */
            out_idx = ff_h264_mv_queue_pick(v);
            out     = v->delayed_pic[out_idx];
        }
        if (h->view_count > 1 && out->output_delivered) {
            /* Already delivered (from this context or a sibling
             * frame-thread context via the context-sync copy): committing
             * again would deliver the picture twice. Drop the stale entry;
             * the POC was accounted for at first commit, so the watermark
             * stays untouched. */
            static int committed_delivered_logged;

            if (!committed_delivered_logged) {
                committed_delivered_logged = 1;
                av_log(h->avctx, AV_LOG_INFO,
                       "multiview view %d: dropped a queued entry for an "
                       "already delivered picture (poc %d)\n",
                       h->cur_view, out->poc);
            }
            out->reference &= ~DELAYED_PIC_REF;
            for (i = out_idx; v->delayed_pic[i]; i++)
                v->delayed_pic[i] = v->delayed_pic[i + 1];
            return 0;
        }

        if (h->view_count > 1 && out->output_omit && !h264_compose_active(h)) {
            /* Output-band fix: the correct output leaves this base picture
             * out. Retire it without delivering it and without advancing
             * the watermark, so the pictures behind it commit normally.
             * Marking it delivered keeps any stale alias of the queue entry
             * from resurrecting it (the gate decision is logged in
             * ff_h264_build_ref_list).
             *
             * Composed mode is excluded: retiring a base picture here would
             * remove it from the sequence the compose pairing consumes, and
             * the other view has a half for it. The picture is committed
             * normally instead and reaches the pairing queue like every
             * other turn of its view. */
            out->reference &= ~DELAYED_PIC_REF;
            out->output_delivered = 1;
            for (i = out_idx; v->delayed_pic[i]; i++)
                v->delayed_pic[i] = v->delayed_pic[i + 1];
            return 0;
        }

        out_of_order = out->poc < v->next_outputed_poc;

        if (out_of_order) {
            av_log(h->avctx, AV_LOG_DEBUG, "no picture ooo\n");
            out->reference &= ~DELAYED_PIC_REF;
            for (i = out_idx; v->delayed_pic[i]; i++)
                v->delayed_pic[i] = v->delayed_pic[i + 1];
        } else if (pics > h->avctx->has_b_frames) {
            if (v->parked_pic && v->parked_pic->output_delivered) {
                /* Stale alias: this views[] copy of the parked pointer
                 * predates another worker's delivery; re-emitting would
                 * deliver the picture twice. Retire it; log the desync
                 * once. */
                static int delivered_parked_logged;

                if (!delivered_parked_logged) {
                    delivered_parked_logged = 1;
                    av_log(h->avctx, AV_LOG_INFO,
                           "multiview view %d: retired the parked picture "
                           "(poc %d); it was already delivered from another "
                           "frame-thread context\n",
                           h->cur_view, v->parked_pic->poc);
                }
                v->parked_pic->reference &= ~DELAYED_PIC_REF;
                v->parked_pic = NULL;
            }
            if (v->parked_pic &&
                (v->parked_pic->poc <= out->poc ||
                 v->parked_pic->flush_old_epoch)) {
                /* A picture of this view was already committed, emitted
                 * and parked pending emission from a later access unit: it
                 * sorts at or below the current minimum (or is a
                 * flush-old-epoch tail that must reach the caller ahead of
                 * the new epoch), so re-emit it. The watermark already
                 * reflects its POC, so it stays untouched. */
                v->next_output_pic = v->parked_pic;
                v->next_output_pic->reference &= ~DELAYED_PIC_REF;
                v->parked_pic = NULL;
            } else {
                /* A parked picture (if any) sorts above every pending
                 * picture: it belongs to a POC epoch the queue no longer
                 * reaches, and is flushed at end of stream. */
                v->next_output_pic = out;
                out->reference &= ~DELAYED_PIC_REF;
                for (i = out_idx; v->delayed_pic[i]; i++)
                    v->delayed_pic[i] = v->delayed_pic[i + 1];
                if (out_idx == 0 && v->delayed_pic[0] && ((v->delayed_pic[0]->f->flags & AV_FRAME_FLAG_KEY) || v->delayed_pic[0]->mmco_reset)) {
                    v->next_outputed_poc = INT_MIN;
                } else if (!out->flush_old_epoch) {
                    /* A flush-old-epoch picture must not advance the
                     * watermark: its POC belongs to the old epoch's ladder,
                     * and leaving that old maximum in place would make the
                     * new epoch's in-flight pictures look out-of-order (and
                     * trigger a spurious second deep wrap). The new epoch's
                     * own commits advance the watermark. */
                    v->next_outputed_poc = out->poc;
                }

                if (!h264_compose_active(h) &&
                    out->output_dup_before && !out->output_dup_done &&
                    h->cur_pic_ptr && h->cur_pic_ptr != out &&
                    h->cur_pic_ptr->f && h->cur_pic_ptr->f->data[0]) {
                    /* Output-band fix: duplicate the picture currently
                     * being decoded once, immediately before `out`; the
                     * output block of h264_decode_frame() emits it ahead of
                     * `out` (the picture is fully decoded - same access
                     * unit) and the original is still delivered at its own
                     * reorder slot. Keep the source's identity token: if the
                     * dependent half is parked for a later packet, the slot
                     * can be reused before the duplicate is emitted.
                     *
                     * Composed mode is excluded at the capture, so the emit
                     * site never sees a source: an extra frame inserted here
                     * would be one more dependent half than the base view has
                     * halves, and the pairing would carry that skew for the
                     * rest of the run. */
                    out->output_dup_src = h->cur_pic_ptr;
                    out->output_dup_epoch = h->cur_pic_ptr->slot_epoch;
                }

                // We have reached an recovery point and all frames after it in
                // display order are "recovered".
                h->frame_recovered |= out->recovered;

                out->recovered |= h->frame_recovered & FRAME_RECOVERED_SEI;

                if (!out->recovered) {
                    if (!(h->avctx->flags & AV_CODEC_FLAG_OUTPUT_CORRUPT) &&
                        !(h->avctx->flags2 & AV_CODEC_FLAG2_SHOW_ALL)) {
                        v->next_output_pic = NULL;
                    } else {
                        out->f->flags |= AV_FRAME_FLAG_CORRUPT;
                    }
                }
            }
        } else {
            av_log(h->avctx, AV_LOG_DEBUG, "no picture %s\n", out_of_order ? "ooo" : "");
        }
        return 0;
    }
}

/**
 * MVC quirk of some 3D streams: dependent-view slices carry a
 * pic_order_cnt_lsb whose per-view MSB/LSB unwrap lands on a POC offset
 * from the matching base-view frame, which the standard per-view unwrap
 * cannot recover. Annex E view references resolve on exact POC equality
 * and the cross-view output picker compares POCs across views, so the
 * picture is relocated to the base-view POC of the base picture at the
 * same frame_num (the per-view unwrap state is left untouched, so the
 * following slices of the same view still unwrap as before).
 *
 * @return 1 if a matching base-view picture was found, 0 otherwise
 */
static int h264_adopt_base_view_poc(H264Context *h, const H264SliceContext *sl)
{
    const H264MVCSPS *mvc = &h->ps.sps->mvc;
    const int base_view_id = mvc->view_id[0];
    const H264Picture *base = NULL;
    int b_poc = 0, b_fpoc[2] = { 0, 0 }, have_base = 0;
    int64_t b_pts = AV_NOPTS_VALUE, b_pdts = AV_NOPTS_VALUE;

    if (!h->cur_pic_ptr || h->views[h->cur_view].view_id == base_view_id)
        return 0;

    /* Prefer the access-unit latch from h264_field_start(): in these files
     * the dependent NALs' frame_num counter runs offset from the base one,
     * so the frame_num match below could pick a neighbouring base picture. */
    if (h->au_base_valid) {
        b_poc  = h->au_base_poc;
        b_fpoc[0] = h->au_base_field_poc[0];
        b_fpoc[1] = h->au_base_field_poc[1];
        b_pts  = h->au_base_pts;
        b_pdts = h->au_base_pkt_dts;
        have_base = 1;
    }

    for (int vs = 0; vs < h->view_count && !have_base; vs++) {
        const H264ViewState *sv = &h->views[vs];
        if (sv->view_id != base_view_id)
            continue;
        for (int i = 0; i < sv->short_ref_count && !have_base; i++) {
            const H264Picture *p = sv->short_ref[i];
            if (p && p->view_idx == 0 && p->frame_num == sl->frame_num &&
                p->f && p->f->data[0])
                base = p;
        }
        for (int i = 0; sv->delayed_pic[i] && !have_base; i++) {
            const H264Picture *p = sv->delayed_pic[i];
            if (p && p->view_idx == 0 && p->frame_num == sl->frame_num &&
                p->f && p->f->data[0])
                base = p;
        }
        if (!have_base && sv->parked_pic &&
            sv->parked_pic->view_idx == 0 &&
            sv->parked_pic->frame_num == sl->frame_num &&
            sv->parked_pic->f && sv->parked_pic->f->data[0])
            base = sv->parked_pic;
        if (base) {
            b_poc  = base->poc;
            b_fpoc[0] = base->field_poc[0];
            b_fpoc[1] = base->field_poc[1];
            b_pts  = base->f->pts;
            b_pdts = base->f->pkt_dts;
            have_base = 1;
        }
    }

    if (!have_base)
        return 0;

    if (h->cur_pic_ptr->poc != b_poc) {
        h->cur_pic_ptr->field_poc[0] = b_fpoc[0];
        h->cur_pic_ptr->field_poc[1] = b_fpoc[1];
        h->cur_pic_ptr->poc          = b_poc;
    }

    /* The demuxer hands the access unit's pts/dts to the first fragment
     * only; the split-off dependent part arrives with AV_NOPTS_VALUE. The
     * views of one access unit are displayed at the same instant, so adopt
     * the base picture's timestamps when the dependent has none of its own
     * (without this, pts-driven output never sees valid timestamps). */
    if (h->cur_pic_ptr->f->pts == AV_NOPTS_VALUE &&
        b_pts != AV_NOPTS_VALUE)
        h->cur_pic_ptr->f->pts = b_pts;
    if (h->cur_pic_ptr->f->pkt_dts == AV_NOPTS_VALUE &&
        b_pdts != AV_NOPTS_VALUE)
        h->cur_pic_ptr->f->pkt_dts = b_pdts;

    return 1;
}

/* This function is called right after decoding the slice header for a first
 * slice in a field (or a frame). It decides whether we are decoding a new frame
 * or a second field in a pair and does the necessary setup.
 */
static int h264_field_start(H264Context *h, const H264SliceContext *sl,
                            const H2645NAL *nal, int first_slice)
{
    int i;
    const SPS *sps;

    int last_pic_structure, last_pic_droppable, ret;

    ret = h264_init_ps(h, sl, first_slice);
    if (ret < 0)
        return ret;

    sps = h->ps.sps;

    if (sps->bitstream_restriction_flag &&
        h->avctx->has_b_frames < sps->num_reorder_frames) {
        h->avctx->has_b_frames = sps->num_reorder_frames;
    }

    last_pic_droppable   = h->droppable;
    last_pic_structure   = h->picture_structure;
    h->droppable         = (nal->ref_idc == 0);
    h->picture_structure = sl->picture_structure;

    H264ViewState *v = &h->views[h->cur_view];

    v->poc.frame_num        = sl->frame_num;
    v->poc.poc_lsb          = sl->poc_lsb;
    v->poc.delta_poc_bottom = sl->delta_poc_bottom;
    v->poc.delta_poc[0]     = sl->delta_poc[0];
    v->poc.delta_poc[1]     = sl->delta_poc[1];

    if (nal->type == H264_NAL_IDR_SLICE)
        h->poc_offset = sl->idr_pic_id;
    else if (h->picture_intra_only)
        h->poc_offset = 0;

    /* Shorten frame num gaps so we don't have to allocate reference
     * frames just to throw them away */
    if (v->poc.frame_num != v->poc.prev_frame_num) {
        int unwrap_prev_frame_num = v->poc.prev_frame_num;
        int max_frame_num         = 1 << sps->log2_max_frame_num;

        if (unwrap_prev_frame_num > v->poc.frame_num)
            unwrap_prev_frame_num -= max_frame_num;

        if ((v->poc.frame_num - unwrap_prev_frame_num) > sps->ref_frame_count) {
            unwrap_prev_frame_num = (v->poc.frame_num - sps->ref_frame_count) - 1;
            if (unwrap_prev_frame_num < 0)
                unwrap_prev_frame_num += max_frame_num;

            v->poc.prev_frame_num = unwrap_prev_frame_num;
        }
    }

    /* See if we have a decoded first field looking for a pair...
     * Here, we're using that to see if we should mark previously
     * decode frames as "finished".
     * We have to do that before the "dummy" in-between frame allocation,
     * since that can modify h->cur_pic_ptr. */
    if (h->first_field) {
        int last_field = last_pic_structure == PICT_BOTTOM_FIELD;
        av_assert0(h->cur_pic_ptr);
        av_assert0(h->cur_pic_ptr->f->buf[0]);
        assert(h->cur_pic_ptr->reference != DELAYED_PIC_REF);

        /* Mark old field/frame as completed */
        if (h->cur_pic_ptr->tf.owner[last_field] == h->avctx) {
            ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX, last_field);
        }

        /* figure out if we have a complementary field pair */
        if (!FIELD_PICTURE(h) || h->picture_structure == last_pic_structure) {
            /* Previous field is unmatched. Don't display it, but let it
             * remain for reference if marked as such. */
            if (last_pic_structure != PICT_FRAME) {
                ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX,
                                          last_pic_structure == PICT_TOP_FIELD);
            }
        } else {
            if (h->cur_pic_ptr->frame_num != v->poc.frame_num) {
                /* This and previous field were reference, but had
                 * different frame_nums. Consider this field first in
                 * pair. Throw away previous field except for reference
                 * purposes. */
                if (last_pic_structure != PICT_FRAME) {
                    ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX,
                                              last_pic_structure == PICT_TOP_FIELD);
                }
            } else {
                /* Second field in complementary pair */
                if (!((last_pic_structure   == PICT_TOP_FIELD &&
                       h->picture_structure == PICT_BOTTOM_FIELD) ||
                      (last_pic_structure   == PICT_BOTTOM_FIELD &&
                       h->picture_structure == PICT_TOP_FIELD))) {
                    av_log(h->avctx, AV_LOG_ERROR,
                           "Invalid field mode combination %d/%d\n",
                           last_pic_structure, h->picture_structure);
                    h->picture_structure = last_pic_structure;
                    h->droppable         = last_pic_droppable;
                    return AVERROR_INVALIDDATA;
                } else if (last_pic_droppable != h->droppable) {
                    avpriv_request_sample(h->avctx,
                                          "Found reference and non-reference fields in the same frame, which");
                    h->picture_structure = last_pic_structure;
                    h->droppable         = last_pic_droppable;
                    return AVERROR_PATCHWELCOME;
                }
            }
        }
    }

    while (v->poc.frame_num != v->poc.prev_frame_num && !h->first_field &&
           v->poc.frame_num != (v->poc.prev_frame_num + 1) % (1 << sps->log2_max_frame_num)) {
        const H264Picture *prev = v->short_ref_count ? v->short_ref[0] : NULL;
        av_log(h->avctx, AV_LOG_DEBUG, "Frame num gap %d %d\n",
               v->poc.frame_num, v->poc.prev_frame_num);
        if (!sps->gaps_in_frame_num_allowed_flag)
            for(i=0; i<FF_ARRAY_ELEMS(v->last_pocs); i++)
                v->last_pocs[i] = INT_MIN;
        ret = h264_frame_start(h);
        if (ret < 0) {
            h->first_field = 0;
            return ret;
        }

        v->poc.prev_frame_num++;
        v->poc.prev_frame_num        %= 1 << sps->log2_max_frame_num;
        h->cur_pic_ptr->frame_num = v->poc.prev_frame_num;
        h->cur_pic_ptr->invalid_gap = !sps->gaps_in_frame_num_allowed_flag;
        ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX, 0);
        ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX, 1);

        h->explicit_ref_marking = 0;
        ret = ff_h264_execute_ref_pic_marking(h);
        if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
            return ret;
        /* Error concealment: If a ref is missing, copy the previous ref
         * in its place.
         * FIXME: Avoiding a memcpy would be nice, but ref handling makes
         * many assumptions about there being no actual duplicates.
         * FIXME: This does not copy padding for out-of-frame motion
         * vectors.  Given we are concealing a lost frame, this probably
         * is not noticeable by comparison, but it should be fixed. */
        if (v->short_ref_count) {
            int c[4] = {
                1<<(h->ps.sps->bit_depth_luma-1),
                1<<(h->ps.sps->bit_depth_chroma-1),
                1<<(h->ps.sps->bit_depth_chroma-1),
                -1
            };

            if (prev &&
                v->short_ref[0]->f->width == prev->f->width &&
                v->short_ref[0]->f->height == prev->f->height &&
                v->short_ref[0]->f->format == prev->f->format) {
                ff_thread_await_progress(&prev->tf, INT_MAX, 0);
                if (prev->field_picture)
                    ff_thread_await_progress(&prev->tf, INT_MAX, 1);
                ff_thread_release_ext_buffer(&v->short_ref[0]->tf);
                v->short_ref[0]->tf.f = v->short_ref[0]->f;
                ret = ff_thread_ref_frame(&v->short_ref[0]->tf, &prev->tf);
                if (ret < 0)
                    return ret;
                v->short_ref[0]->poc = prev->poc + 2U;
                v->short_ref[0]->gray = prev->gray;
                ff_thread_report_progress(&v->short_ref[0]->tf, INT_MAX, 0);
                if (v->short_ref[0]->field_picture)
                    ff_thread_report_progress(&v->short_ref[0]->tf, INT_MAX, 1);
            } else if (!h->frame_recovered) {
                if (!h->avctx->hwaccel)
                    color_frame(v->short_ref[0]->f, c);
                v->short_ref[0]->gray = 1;
            }
            v->short_ref[0]->frame_num = v->poc.prev_frame_num;
        }
    }

    /* See if we have a decoded first field looking for a pair...
     * We're using that to see whether to continue decoding in that
     * frame, or to allocate a new one. */
    if (h->first_field) {
        av_assert0(h->cur_pic_ptr);
        av_assert0(h->cur_pic_ptr->f->buf[0]);
        assert(h->cur_pic_ptr->reference != DELAYED_PIC_REF);

        /* figure out if we have a complementary field pair */
        if (!FIELD_PICTURE(h) || h->picture_structure == last_pic_structure) {
            /* Previous field is unmatched. Don't display it, but let it
             * remain for reference if marked as such. */
            h->missing_fields ++;
            h->cur_pic_ptr = NULL;
            h->first_field = FIELD_PICTURE(h);
        } else {
            h->missing_fields = 0;
            if (h->cur_pic_ptr->frame_num != v->poc.frame_num) {
                ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX,
                                          h->picture_structure==PICT_BOTTOM_FIELD);
                /* This and the previous field had different frame_nums.
                 * Consider this field first in pair. Throw away previous
                 * one except for reference purposes. */
                h->first_field = 1;
                h->cur_pic_ptr = NULL;
            } else if (h->cur_pic_ptr->reference & DELAYED_PIC_REF) {
                /* This frame was already output, we cannot draw into it
                 * anymore.
                 */
                h->first_field = 1;
                h->cur_pic_ptr = NULL;
            } else {
                /* Second field in complementary pair */
                h->first_field = 0;
            }
        }
    } else {
        /* Frame or first field in a potentially complementary pair */
        h->first_field = FIELD_PICTURE(h);
    }

    if (!FIELD_PICTURE(h) || h->first_field) {
        if (h264_frame_start(h) < 0) {
            h->first_field = 0;
            return AVERROR_INVALIDDATA;
        }
    } else {
        int field = h->picture_structure == PICT_BOTTOM_FIELD;
        release_unused_pictures(h, 0);
        h->cur_pic_ptr->tf.owner[field] = h->avctx;
    }
    /* Some macroblocks can be accessed before they're available in case
    * of lost slices, MBAFF or threading. */
    if (FIELD_PICTURE(h)) {
        for(i = (h->picture_structure == PICT_BOTTOM_FIELD); i<h->mb_height; i++)
            memset(h->slice_table + i*h->mb_stride, -1, (h->mb_stride - (i+1==h->mb_height)) * sizeof(*h->slice_table));
    } else {
        memset(h->slice_table, -1,
            (h->mb_height * h->mb_stride - 1) * sizeof(*h->slice_table));
    }

    ret = ff_h264_init_poc(h->cur_pic_ptr->field_poc, &h->cur_pic_ptr->poc,
                      h->ps.sps, &v->poc, h->picture_structure, nal->ref_idc);
    if (ret < 0)
        return ret;

    /* Multiview: once the base view's POC is finalized, latch its POC and
     * the access unit's timestamps as scalars for the dependent view's
     * field start to adopt (a pointer would be unreliable: the base
     * picture's DPB slot can be reused first). The base view is always
     * slot 0; its slice references the compatibility SPS (mvc.present ==
     * 0), so the latch must not be conditioned on the active SPS's mvc
     * extension. */
    if (h->cur_pic_ptr && h->view_count > 1 && h->cur_view == 0) {
        h->au_base_poc          = h->cur_pic_ptr->poc;
        h->au_base_field_poc[0] = h->cur_pic_ptr->field_poc[0];
        h->au_base_field_poc[1] = h->cur_pic_ptr->field_poc[1];
        h->au_base_pts          = h->cur_pic_ptr->f->pts;
        h->au_base_pkt_dts      = h->cur_pic_ptr->f->pkt_dts;
        h->au_base_valid        = 1;
    }

    /* Every dependent-view slice must carry the POC of its co-located
     * base-view frame: some 3D streams write the dependent view's
     * pic_order_cnt_lsb with a per-view offset the standard per-view
     * unwrap cannot reconcile, and both the Annex E inter-view references
     * (exact POC equality) and the cross-view output ordering (lowest POC
     * across all views) require dependent views to track the base POC. */
    if (h->cur_pic_ptr && h->cur_view > 0 &&
        h->view_count > 1 && h->ps.sps->mvc.present)
        h264_adopt_base_view_poc(h, sl);

    memcpy(h->mmco, sl->mmco, sl->nb_mmco * sizeof(*h->mmco));
    h->nb_mmco = sl->nb_mmco;
    h->explicit_ref_marking = sl->explicit_ref_marking;

    h->picture_idr = nal->type == H264_NAL_IDR_SLICE;

    if (h->sei.recovery_point.recovery_frame_cnt >= 0) {
        const int sei_recovery_frame_cnt = h->sei.recovery_point.recovery_frame_cnt;

        if (v->poc.frame_num != sei_recovery_frame_cnt || sl->slice_type_nos != AV_PICTURE_TYPE_I)
            h->valid_recovery_point = 1;

        if (   h->recovery_frame < 0
            || av_zero_extend(h->recovery_frame - v->poc.frame_num, h->ps.sps->log2_max_frame_num) > sei_recovery_frame_cnt) {
            h->recovery_frame = av_zero_extend(v->poc.frame_num + sei_recovery_frame_cnt, h->ps.sps->log2_max_frame_num);

            if (!h->valid_recovery_point)
                h->recovery_frame = v->poc.frame_num;
        }
    }

    h->cur_pic_ptr->f->flags |= AV_FRAME_FLAG_KEY * !!(nal->type == H264_NAL_IDR_SLICE);

    if (nal->type == H264_NAL_IDR_SLICE) {
        h->cur_pic_ptr->recovered |= FRAME_RECOVERED_IDR;
        // If we have an IDR, all frames after it in decoded order are
        // "recovered".
        h->frame_recovered |= FRAME_RECOVERED_IDR;
    }

    if (h->recovery_frame == v->poc.frame_num && nal->ref_idc) {
        h->recovery_frame = -1;
        h->cur_pic_ptr->recovered |= FRAME_RECOVERED_SEI;
        /* A recovery point makes every later frame recovered, but the
         * commit path only propagates that for the view carrying the
         * recovery point: dependent views never carry it, and an
         * unselected base view is not even committed, so the dependent
         * view's output gate would stay closed until the next IDR (after
         * a seek: every frame between seek and refresh lost). Propagate
         * to the whole context at the recovery point itself so every view
         * opens its gate at the same stream position. */
        if (h->view_count > 1)
            h->frame_recovered |= FRAME_RECOVERED_SEI;
    }

#if 1
    h->cur_pic_ptr->recovered |= h->frame_recovered;
#else
    h->cur_pic_ptr->recovered |= !!(h->frame_recovered & FRAME_RECOVERED_IDR);
#endif

    /* Multiview: grant the co-located base-view picture's recovery bits to
     * the dependent picture. The dependent half is inter-coded, so the
     * unmarked-RAP heuristic in ff_h264_execute_ref_pic_marking() never
     * fires for it, and an unselected base view is never committed, so
     * frame_recovered is never promoted from it: after a mid-stream seek
     * on an unmarked base RAP the dependent view would lose every frame up
     * to its own next IDR. Locate the base picture by the adopted POC in
     * the base view's delayed queue (pinned until its own commit, which
     * cannot precede this field start; on no-B streams the state instead
     * arrives via the context-level frame_recovered bit ORed in above).
     * Pictures whose base pair is itself not yet recovered get nothing,
     * keeping both views' output grids aligned. */
    if (h->view_count > 1 && h->cur_view > 0 &&
        h->ps.sps && h->ps.sps->mvc.present && h->cur_pic_ptr &&
        h264_view_selected(h, h->cur_view)) {
        const H264MVCSPS *mvc = &h->ps.sps->mvc;
        const int base_view_id = mvc->view_id[0];

        for (int vs = 0; vs < h->view_count; vs++) {
            const H264ViewState *bv = &h->views[vs];

            if (bv->view_id != base_view_id)
                continue;
            for (int i = 0; bv->delayed_pic[i] &&
                            bv->delayed_pic[i] != h->cur_pic_ptr; i++) {
                const H264Picture *bp = bv->delayed_pic[i];

                if (bp->view_idx == vs && bp->poc == h->cur_pic_ptr->poc) {
                    h->cur_pic_ptr->recovered |= bp->recovered;
                    break;
                }
            }
            break;
        }
    }

    /* Set the frame properties/side data. Only done for the second field in
     * field coded frames, since some SEI information is present for each field
     * and is merged by the SEI parsing code. */
    if (!FIELD_PICTURE(h) || !h->first_field || h->missing_fields > 1) {
        ret = h264_export_frame_props(h);
        if (ret < 0)
            return ret;

        ret = h264_select_output_frame(h);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int h264_slice_header_parse(const H264Context *h, H264SliceContext *sl,
                                   const H2645NAL *nal)
{
    const SPS *sps;
    const PPS *pps;
    int ret;
    unsigned int slice_type, tmp, i;
    int field_pic_flag, bottom_field_flag;
    int first_slice = sl == h->slice_ctx && !h->current_slice;
    int picture_structure;

    if (first_slice)
        av_assert0(!h->setup_finished);

    sl->degenerate = 0;
    sl->first_mb_addr = get_ue_golomb_long(&sl->gb);

    slice_type = get_ue_golomb_31(&sl->gb);
    if (slice_type > 9) {
        av_log(h->avctx, AV_LOG_ERROR,
               "slice type %d too large at %d\n",
               slice_type, sl->first_mb_addr);
        return AVERROR_INVALIDDATA;
    }
    if (slice_type > 4) {
        slice_type -= 5;
        sl->slice_type_fixed = 1;
    } else
        sl->slice_type_fixed = 0;

    slice_type         = ff_h264_golomb_to_pict_type[slice_type];
    sl->slice_type     = slice_type;
    sl->slice_type_nos = slice_type & 3;

    if (nal->type  == H264_NAL_IDR_SLICE &&
        sl->slice_type_nos != AV_PICTURE_TYPE_I) {
        av_log(h->avctx, AV_LOG_ERROR, "A non-intra slice in an IDR NAL unit.\n");
        return AVERROR_INVALIDDATA;
    }

    sl->pps_id = get_ue_golomb(&sl->gb);
    if (sl->pps_id >= MAX_PPS_COUNT) {
        av_log(h->avctx, AV_LOG_ERROR, "pps_id %u out of range\n", sl->pps_id);
        return AVERROR_INVALIDDATA;
    }
    if (!h->ps.pps_list[sl->pps_id]) {
        av_log(h->avctx, AV_LOG_ERROR,
               "non-existing PPS %u referenced\n",
               sl->pps_id);
        return AVERROR_INVALIDDATA;
    }
    pps = h->ps.pps_list[sl->pps_id];
    sps = pps->sps;

    sl->frame_num = get_bits(&sl->gb, sps->log2_max_frame_num);
    if (!first_slice) {
        if (h->views[h->cur_view].poc.frame_num != sl->frame_num) {
            av_log(h->avctx, AV_LOG_ERROR, "Frame num change from %d to %d\n",
                   h->views[h->cur_view].poc.frame_num, sl->frame_num);
            return AVERROR_INVALIDDATA;
        }
    }

    sl->mb_mbaff       = 0;

    if (sps->frame_mbs_only_flag) {
        picture_structure = PICT_FRAME;
    } else {
        if (!sps->direct_8x8_inference_flag && slice_type == AV_PICTURE_TYPE_B) {
            av_log(h->avctx, AV_LOG_ERROR, "This stream was generated by a broken encoder, invalid 8x8 inference\n");
            return -1;
        }
        field_pic_flag = get_bits1(&sl->gb);
        if (field_pic_flag) {
            bottom_field_flag = get_bits1(&sl->gb);
            picture_structure = PICT_TOP_FIELD + bottom_field_flag;
        } else {
            picture_structure = PICT_FRAME;
        }
    }
    sl->picture_structure      = picture_structure;
    sl->mb_field_decoding_flag = picture_structure != PICT_FRAME;

    if (picture_structure == PICT_FRAME) {
        sl->curr_pic_num = sl->frame_num;
        sl->max_pic_num  = 1 << sps->log2_max_frame_num;
    } else {
        sl->curr_pic_num = 2 * sl->frame_num + 1;
        sl->max_pic_num  = 1 << (sps->log2_max_frame_num + 1);
    }

    /* idr_pic_id is present for IDR-coded slices; for slice extension NALs
     * the IDR-ness is signaled by the extension header's non_idr_flag (read
     * iff non_idr_flag==0). Some 3D streams' dependent-view anchor pictures
     * use exactly that combination (type 20, non_idr=0, ref_idc=3); missing
     * the read shifts every later header field and desynchronizes the anchor. */
    if (nal->type == H264_NAL_IDR_SLICE ||
        (nal->type == H264_NAL_EXTEN_SLICE && nal->mv_ext_parsed &&
         !nal->mv_non_idr)) {
        unsigned idr_pic_id = get_ue_golomb_long(&sl->gb);
        if (idr_pic_id < 65536) {
            sl->idr_pic_id = idr_pic_id;
        } else
            av_log(h->avctx, AV_LOG_WARNING, "idr_pic_id is invalid\n");
    }

    sl->poc_lsb = 0;
    sl->delta_poc_bottom = 0;
    if (sps->poc_type == 0) {
        sl->poc_lsb = get_bits(&sl->gb, sps->log2_max_poc_lsb);

        if (pps->pic_order_present == 1 && picture_structure == PICT_FRAME)
            sl->delta_poc_bottom = get_se_golomb(&sl->gb);
    }

    sl->delta_poc[0] = sl->delta_poc[1] = 0;
    if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero_flag) {
        sl->delta_poc[0] = get_se_golomb(&sl->gb);

        if (pps->pic_order_present == 1 && picture_structure == PICT_FRAME)
            sl->delta_poc[1] = get_se_golomb(&sl->gb);
    }

    sl->redundant_pic_count = 0;
    if (pps->redundant_pic_cnt_present)
        sl->redundant_pic_count = get_ue_golomb(&sl->gb);

    if (sl->slice_type_nos == AV_PICTURE_TYPE_B)
        sl->direct_spatial_mv_pred = get_bits1(&sl->gb);

    /* The anchor flag of the (multiview) NAL header selects which of the
     * SPS inter-view reference lists applies to this slice. */
    sl->mvc_anchor = (nal->mv_ext_parsed && nal->mv_anchor_pic);

    ret = ff_h264_parse_ref_count(&sl->list_count, sl->ref_count,
                                  &sl->gb, pps, sl->slice_type_nos,
                                  picture_structure, h->avctx,
                                  sl->mvc_anchor && sps->mvc.present,
                                  &sl->degenerate);
    if (ret < 0)
        return ret;

    if (sl->slice_type_nos != AV_PICTURE_TYPE_I) {
        ret = ff_h264_decode_ref_pic_list_reordering(sl, h->avctx);
        if (ret < 0) {
            sl->ref_count[1] = sl->ref_count[0] = 0;
            return ret;
        }
    }
    sl->pwt.use_weight = 0;
    for (i = 0; i < 2; i++) {
        sl->pwt.luma_weight_flag[i]   = 0;
        sl->pwt.chroma_weight_flag[i] = 0;
    }
    if ((pps->weighted_pred && sl->slice_type_nos == AV_PICTURE_TYPE_P) ||
        (pps->weighted_bipred_idc == 1 &&
         sl->slice_type_nos == AV_PICTURE_TYPE_B)) {
        ret = ff_h264_pred_weight_table(&sl->gb, sps, sl->ref_count,
                                  sl->slice_type_nos, &sl->pwt,
                                  picture_structure, h->avctx,
                                  sl->mvc_anchor && sps->mvc.present,
                                  &sl->degenerate);
        if (ret < 0)
            return ret;
    }

    sl->explicit_ref_marking = 0;
    if (nal->ref_idc) {
        ret = ff_h264_decode_ref_pic_marking(sl, &sl->gb, nal, h->avctx);
        if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
            return AVERROR_INVALIDDATA;
    }

    /* Some 2D+delta MVC streams under-deliver dependent anchor slices:
     * the NAL carries the complete slice header but a degenerate
     * near-empty slice data region (up to ~124 bits); healthy anchor
     * bands keep at least ~8x the 1024-bit floor below. The floor is
     * fixed per NAL - dependent anchors are split into row-band slices,
     * one NAL per band, so a per-picture floor would misclassify
     * legitimate low-motion bands. Do NOT extend this to non-anchor
     * dependent slices: their small bands are complete all-skip
     * payloads and must be parsed normally. The defaults set below are
     * only initial values: the real header reads always run and the
     * anchor tolerances clamp out-of-range values without skipping
     * bits, so the bit reader lands at the CABAC payload start. */
    {
        int remain = get_bits_left(&sl->gb);
        const int floor = 1024;

        if (sl->mvc_anchor && pps->sps->mvc.present && remain < floor) {
            static int degenerate_anchor_logged; /* one-shot debug log */

            if (!degenerate_anchor_logged) {
                degenerate_anchor_logged = 1;
                av_log(h->avctx, AV_LOG_DEBUG,
                       "dependent anchor slice keeps %d bits after the "
                        "slice header (truncation floor %d); treating it "
                        "as a degenerate anchor\n", remain, floor);
            }
            sl->degenerate = 1;
            sl->last_qscale_diff = 0;
            sl->cabac_init_idc   = 2;
            sl->qscale           = pps->init_qp;
            sl->chroma_qp[0]     = get_chroma_qp(pps, 0, sl->qscale);
            sl->chroma_qp[1]     = get_chroma_qp(pps, 1, sl->qscale);
            sl->deblocking_filter     = 1;
            sl->slice_alpha_c0_offset = 0;
            sl->slice_beta_offset     = 0;

            /* Do NOT return here: this class's slice data region is a
             * complete self-contained CABAC payload, so fall through and
             * parse the fields below for real (the defaults above are
             * just their initial values). Returning here left the bit
             * reader mid-header and misaligned the CABAC start. */
        }
    }

    /* Degenerate anchor slices (see above) may place bit-level garbage
     * in the fields below the picture marking: fall back to defaults
     * without reporting and mark the picture degenerate so it is dropped
     * as a whole instead of decoded from misaligned bits. */
    if (sl->slice_type_nos != AV_PICTURE_TYPE_I && pps->cabac) {
        tmp = get_ue_golomb_31(&sl->gb);
        if (tmp > 2) {
            if (sl->mvc_anchor && sps->mvc.present) {
                sl->degenerate = 1;
                tmp = 2;
            } else {
                av_log(h->avctx, AV_LOG_ERROR, "cabac_init_idc %u overflow\n", tmp);
                return AVERROR_INVALIDDATA;
            }
        }
        sl->cabac_init_idc = tmp;
    }

    sl->last_qscale_diff = 0;
    tmp = pps->init_qp + (unsigned)get_se_golomb(&sl->gb);
    if (tmp > 51 + 6 * (sps->bit_depth_luma - 8)) {
        if (sl->mvc_anchor && sps->mvc.present) {
            sl->degenerate = 1;
            tmp = pps->init_qp;
        } else {
            av_log(h->avctx, AV_LOG_ERROR, "QP %u out of range\n", tmp);
            return AVERROR_INVALIDDATA;
        }
    }
    sl->qscale       = tmp;
    sl->chroma_qp[0] = get_chroma_qp(pps, 0, sl->qscale);
    sl->chroma_qp[1] = get_chroma_qp(pps, 1, sl->qscale);
    // FIXME qscale / qp ... stuff
    if (sl->slice_type == AV_PICTURE_TYPE_SP)
        get_bits1(&sl->gb); /* sp_for_switch_flag */
    if (sl->slice_type == AV_PICTURE_TYPE_SP ||
        sl->slice_type == AV_PICTURE_TYPE_SI)
        get_se_golomb(&sl->gb); /* slice_qs_delta */

    sl->deblocking_filter     = 1;
    sl->slice_alpha_c0_offset = 0;
    sl->slice_beta_offset     = 0;
    if (pps->deblocking_filter_parameters_present) {
        tmp = get_ue_golomb_31(&sl->gb);
        if (tmp > 2) {
            if (sl->mvc_anchor && sps->mvc.present) { // see above
                sl->degenerate = 1;
                tmp = 2;
            } else {
                av_log(h->avctx, AV_LOG_ERROR,
                       "deblocking_filter_idc %u out of range\n", tmp);
                return AVERROR_INVALIDDATA;
            }
        }
        sl->deblocking_filter = tmp;
        if (sl->deblocking_filter < 2)
            sl->deblocking_filter ^= 1;  // 1<->0

        if (sl->deblocking_filter) {
            int slice_alpha_c0_offset_div2 = get_se_golomb(&sl->gb);
            int slice_beta_offset_div2     = get_se_golomb(&sl->gb);
            if (slice_alpha_c0_offset_div2 >  6 ||
                slice_alpha_c0_offset_div2 < -6 ||
                slice_beta_offset_div2 >  6     ||
                slice_beta_offset_div2 < -6) {
                if (sl->mvc_anchor && sps->mvc.present) { // see above
                    sl->degenerate = 1;
                    slice_alpha_c0_offset_div2 = slice_beta_offset_div2 = 0;
                } else {
                    av_log(h->avctx, AV_LOG_ERROR,
                           "deblocking filter parameters %d %d out of range\n",
                           slice_alpha_c0_offset_div2, slice_beta_offset_div2);
                    return AVERROR_INVALIDDATA;
                }
            }
            sl->slice_alpha_c0_offset = slice_alpha_c0_offset_div2 * 2;
            sl->slice_beta_offset     = slice_beta_offset_div2 * 2;
        }
    }

    return 0;
}

/* do all the per-slice initialization needed before we can start decoding the
 * actual MBs */
static int h264_slice_init(H264Context *h, H264SliceContext *sl,
                           const H2645NAL *nal)
{
    H264ViewState *v = &h->views[h->cur_view];
    int i, j, ret = 0;

    if (h->picture_idr && nal->type != H264_NAL_IDR_SLICE) {
        av_log(h->avctx, AV_LOG_ERROR, "Invalid mix of IDR and non-IDR slices\n");
        return AVERROR_INVALIDDATA;
    }

    av_assert1(h->mb_num == h->mb_width * h->mb_height);
    if (sl->first_mb_addr << FIELD_OR_MBAFF_PICTURE(h) >= h->mb_num ||
        sl->first_mb_addr >= h->mb_num) {
        av_log(h->avctx, AV_LOG_ERROR, "first_mb_in_slice overflow\n");
        return AVERROR_INVALIDDATA;
    }
    sl->resync_mb_x = sl->mb_x =  sl->first_mb_addr % h->mb_width;
    sl->resync_mb_y = sl->mb_y = (sl->first_mb_addr / h->mb_width) <<
                                 FIELD_OR_MBAFF_PICTURE(h);
    if (h->picture_structure == PICT_BOTTOM_FIELD)
        sl->resync_mb_y = sl->mb_y = sl->mb_y + 1;
    av_assert1(sl->mb_y < h->mb_height);

    ret = ff_h264_build_ref_list(h, sl);
    if (ret < 0)
        return ret;

    if (h->ps.pps->weighted_bipred_idc == 2 &&
        sl->slice_type_nos == AV_PICTURE_TYPE_B) {
        implicit_weight_table(h, sl, -1);
        if (FRAME_MBAFF(h)) {
            implicit_weight_table(h, sl, 0);
            implicit_weight_table(h, sl, 1);
        }
    }

    if (sl->slice_type_nos == AV_PICTURE_TYPE_B && !sl->direct_spatial_mv_pred)
        ff_h264_direct_dist_scale_factor(h, sl);
    ff_h264_direct_ref_list_init(h, sl);

    if (h->avctx->skip_loop_filter >= AVDISCARD_ALL ||
        (h->avctx->skip_loop_filter >= AVDISCARD_NONKEY &&
         h->nal_unit_type != H264_NAL_IDR_SLICE) ||
        (h->avctx->skip_loop_filter >= AVDISCARD_NONINTRA &&
         sl->slice_type_nos != AV_PICTURE_TYPE_I) ||
        (h->avctx->skip_loop_filter >= AVDISCARD_BIDIR  &&
         sl->slice_type_nos == AV_PICTURE_TYPE_B) ||
        (h->avctx->skip_loop_filter >= AVDISCARD_NONREF &&
         nal->ref_idc == 0))
        sl->deblocking_filter = 0;

    if (sl->deblocking_filter == 1 && h->nb_slice_ctx > 1) {
        if (h->avctx->flags2 & AV_CODEC_FLAG2_FAST) {
            /* Cheat slightly for speed:
             * Do not bother to deblock across slices. */
            sl->deblocking_filter = 2;
        } else {
            h->postpone_filter = 1;
        }
    }
    sl->qp_thresh = 15 -
                   FFMIN(sl->slice_alpha_c0_offset, sl->slice_beta_offset) -
                   FFMAX3(0,
                          h->ps.pps->chroma_qp_index_offset[0],
                          h->ps.pps->chroma_qp_index_offset[1]) +
                   6 * (h->ps.sps->bit_depth_luma - 8);

    // slice_table is uint16_t initialized to 0xFFFF as a sentinel.
    if (h->current_slice >= 0xFFFE) {
        av_log(h->avctx, AV_LOG_ERROR, "Too many slices (%d)\n", h->current_slice + 1);
        return AVERROR_PATCHWELCOME;
    }

    sl->slice_num       = ++h->current_slice;

    if (sl->slice_num)
        h->slice_row[(sl->slice_num-1)&(MAX_SLICES-1)]= sl->resync_mb_y;
    if (   h->slice_row[sl->slice_num&(MAX_SLICES-1)] + 3 >= sl->resync_mb_y
        && h->slice_row[sl->slice_num&(MAX_SLICES-1)] <= sl->resync_mb_y
        && sl->slice_num >= MAX_SLICES) {
        //in case of ASO this check needs to be updated depending on how we decide to assign slice numbers in this case
        av_log(h->avctx, AV_LOG_WARNING, "Possibly too many slices (%d >= %d), increase MAX_SLICES and recompile if there are artifacts\n", sl->slice_num, MAX_SLICES);
    }

    for (j = 0; j < 2; j++) {
        int id_list[16];
        int *ref2frm = h->ref2frm[sl->slice_num & (MAX_SLICES - 1)][j];
        for (i = 0; i < 16; i++) {
            id_list[i] = 60;
            if (j < sl->list_count && i < sl->ref_count[j] &&
                sl->ref_list[j][i].parent->f->buf[0]) {
                int k;
                const AVBuffer *buf = sl->ref_list[j][i].parent->f->buf[0]->buffer;
                for (k = 0; k < v->short_ref_count; k++)
                    if (v->short_ref[k]->f->buf[0]->buffer == buf) {
                        id_list[i] = k;
                        break;
                    }
                for (k = 0; k < v->long_ref_count; k++)
                    if (v->long_ref[k] && v->long_ref[k]->f->buf[0]->buffer == buf) {
                        id_list[i] = v->short_ref_count + k;
                        break;
                    }
            }
        }

        ref2frm[0] =
        ref2frm[1] = -1;
        for (i = 0; i < 16; i++)
            ref2frm[i + 2] = 4 * id_list[i] + (sl->ref_list[j][i].reference & 3);
        ref2frm[18 + 0] =
        ref2frm[18 + 1] = -1;
        for (i = 16; i < 48; i++)
            ref2frm[i + 4] = 4 * id_list[(i - 16) >> 1] +
                             (sl->ref_list[j][i].reference & 3);
    }

    if (sl->slice_type_nos == AV_PICTURE_TYPE_I) {
        h->cur_pic_ptr->gray = 0;
        h->non_gray = 1;
    } else {
        int gray = 0;
        for (j = 0; j < sl->list_count; j++) {
            for (i = 0; i < sl->ref_count[j]; i++) {
                gray |= sl->ref_list[j][i].parent->gray;
            }
        }
        h->cur_pic_ptr->gray = gray;
    }

    if (h->avctx->debug & FF_DEBUG_PICT_INFO) {
        av_log(h->avctx, AV_LOG_DEBUG,
               "slice:%d %c mb:%d %c%s%s frame:%d poc:%d/%d ref:%d/%d qp:%d loop:%d:%d:%d weight:%d%s %s\n",
               sl->slice_num,
               (h->picture_structure == PICT_FRAME ? 'F' : h->picture_structure == PICT_TOP_FIELD ? 'T' : 'B'),
               sl->mb_y * h->mb_width + sl->mb_x,
               av_get_picture_type_char(sl->slice_type),
                sl->slice_type_fixed ? " fix" : "",
                nal->type == H264_NAL_IDR_SLICE ? " IDR" : "",
                v->poc.frame_num,
               h->cur_pic_ptr->field_poc[0],
               h->cur_pic_ptr->field_poc[1],
               sl->ref_count[0], sl->ref_count[1],
               sl->qscale,
               sl->deblocking_filter,
               sl->slice_alpha_c0_offset, sl->slice_beta_offset,
               sl->pwt.use_weight,
               sl->pwt.use_weight == 1 && sl->pwt.use_weight_chroma ? "c" : "",
               sl->slice_type == AV_PICTURE_TYPE_B ? (sl->direct_spatial_mv_pred ? "SPAT" : "TEMP") : "");
    }

    return 0;
}

int ff_h264_queue_decode_slice(H264Context *h, const H2645NAL *nal)
{
    H264SliceContext *sl;
    int first_slice, ret;

    {
        /* Multiview (Annex E): a slice extension NAL carries a picture of
         * the view named by its NAL header, plain slice NALs belong to
         * the base view. Access units contain both views back to back, so
         * a view change mid-packet finalizes the picture in progress and
         * starts the next view's picture from scratch. */
        int slot = 0;

        if (nal->type >= H264_NAL_AUXILIARY_SLICE &&
            nal->type <= H264_NAL_RESERVED23) {
            slot = -1;
            for (int i = 0; i < h->view_count; i++)
                if (h->views[i].view_id == nal->mv_view_id) {
                    slot = i;
                    break;
                }
            if (slot < 0) {
                av_log(h->avctx, AV_LOG_ERROR,
                       "Multiview NAL unit carries unregistered view id %d\n",
                       nal->mv_view_id);
                return 0; // skip the slice
            }
        }

        /* View selection: VCL of a non-base view the user did not request
         * is not decoded at all; the base view (slot 0) is always decoded
         * because dependent slices resolve inter-view references through
         * it. Skipping before the view switch leaves h->cur_view (and the
         * picture in progress) untouched.
         *
         * 2-view limitation: correct only for base + one dependent view.
         * In a 3+ view reference chain, deselecting an intermediate view
         * leaves the next view's slices pointing at never-decoded
         * inter-view references (the missing-view drop path covers views
         * absent from the bitstream, not user-deselected ones). */
        if (slot > 0 && !h264_view_selected(h, slot)) {
            h->view_sel_skipped = 1;
            return 0;
        }

        if (slot != h->cur_view) {
            /* current_slice is only set while a picture's slices are
             * queued but not finished: at packet start the previous
             * picture already went through ff_h264_field_end() */
            if (h->current_slice) {
                /* flush the queued slices of the previous view's picture */
                if (h->nb_slice_ctx_queued) {
                    ret = ff_h264_execute_decode_slices(h);
                    if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
                        return ret;
                }
                if (h->cur_pic_ptr) {
                    ret = ff_h264_field_end(h, h->slice_ctx, 1);
                    if (ret < 0)
                        return ret;
                    ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX, 0);
                    ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX, 1);
                    h->cur_pic_ptr = NULL;
                }
            }
            h->current_slice  = 0;
            h->first_field    = 0;
            h->droppable      = 0;
            h->cur_view       = slot;
        }
    }

    sl = h->slice_ctx + h->nb_slice_ctx_queued;
    first_slice = sl == h->slice_ctx && !h->current_slice;

    sl->gb = nal->gb;
    sl->nal_size = nal->size;

    ret = h264_slice_header_parse(h, sl, nal);
    if (ret < 0)
        return ret;

    /* An inter slice of this view whose own reference material has all
     * vanished from the DPB (preceding pictures dropped): mark
     * degenerate; the handling below decides drop vs decode. */
    if (!sl->degenerate && h->view_count > 1 && h->cur_view > 0 &&
        sl->slice_type_nos != AV_PICTURE_TYPE_I) {
        const H264ViewState *v = &h->views[h->cur_view];
        if (!v->short_ref_count && !v->long_ref_count &&
            !v->delayed_pic[0] && !v->parked_pic)
            sl->degenerate = 1;
    }

    /* Degenerate slices (see h264_slice_header_parse) are dropped as a
     * whole when decoding could not possibly succeed: either the needed
     * reference material is genuinely absent from the stream (delta-only
     * distribution, where decoding would only yield a fully concealed,
     * suppressed picture), or the header tolerances ended with an empty
     * reference list for a non-intra slice (decoding a non-empty payload
     * would drive motion compensation against zeroed slots and crash).
     * In both cases the remaining slices of the access unit are consumed
     * by the latch. Anchors in streams where every view IS present must
     * not be dropped: their near-empty payload decodes against the real
     * base-view material and is already normalized by the header
     * tolerances. */
    if (sl->degenerate && h->view_count > 1 && h->cur_view > 0) {
        const H264ViewState *base = &h->views[0];
        if ((!base->short_ref_count && !base->long_ref_count &&
             !base->delayed_pic[0] && !base->parked_pic) ||
             (sl->list_count == 0 &&
              sl->slice_type_nos != AV_PICTURE_TYPE_I)) {
            h->drop_view_slices = 1;
            if (h->current_slice) {
                /* A picture is already in progress: abandon it without
                 * executing the queued slices, so no part of the
                 * undecodable payload is decoded. The view's POC state is
                 * deliberately left untouched (unlike ff_h264_field_end())
                 * so the next picture's POC prediction stays correct. */
                ff_h264_unref_picture(&h->cur_pic);
                ff_h264_unref_picture(h->cur_pic_ptr);
                h->cur_pic_ptr         = NULL;
                h->nb_slice_ctx_queued = 0;
                h->current_slice       = 0;
                h->first_field         = 0;
                h->droppable           = 0;
            }
            return 0;
        }
    }

    // discard redundant pictures
    if (sl->redundant_pic_count > 0) {
        sl->ref_count[0] = sl->ref_count[1] = 0;
        return 0;
    }

    if (sl->first_mb_addr == 0 || !h->current_slice) {
        if (h->setup_finished) {
            av_log(h->avctx, AV_LOG_ERROR, "Too many fields\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (sl->first_mb_addr == 0) { // FIXME better field boundary detection
        if (h->current_slice) {
            // this slice starts a new field
            // first decode any pending queued slices
            if (h->nb_slice_ctx_queued) {
                H264SliceContext tmp_ctx;

                ret = ff_h264_execute_decode_slices(h);
                if (ret < 0 && (h->avctx->err_recognition & AV_EF_EXPLODE))
                    return ret;

                memcpy(&tmp_ctx, h->slice_ctx, sizeof(tmp_ctx));
                memcpy(h->slice_ctx, sl, sizeof(tmp_ctx));
                memcpy(sl, &tmp_ctx, sizeof(tmp_ctx));
                sl = h->slice_ctx;
            }

            if (h->cur_pic_ptr && FIELD_PICTURE(h) && h->first_field) {
                ret = ff_h264_field_end(h, h->slice_ctx, 1);
                if (ret < 0)
                    return ret;
            } else if (h->cur_pic_ptr && !FIELD_PICTURE(h) && !h->first_field && h->nal_unit_type  == H264_NAL_IDR_SLICE) {
                av_log(h->avctx, AV_LOG_WARNING, "Broken frame packetizing\n");
                ret = ff_h264_field_end(h, h->slice_ctx, 1);
                ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX, 0);
                ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX, 1);
                h->cur_pic_ptr = NULL;
                if (ret < 0)
                    return ret;
            } else
                return AVERROR_INVALIDDATA;
        }

        if (!h->first_field) {
            if (h->cur_pic_ptr && !h->droppable) {
                ff_thread_report_progress(&h->cur_pic_ptr->tf, INT_MAX,
                                          h->picture_structure == PICT_BOTTOM_FIELD);
            }
            h->cur_pic_ptr = NULL;
        }
    }

    if (!h->current_slice)
        av_assert0(sl == h->slice_ctx);

    if (h->current_slice == 0 && !h->first_field) {
        if (
            (h->avctx->skip_frame >= AVDISCARD_NONREF && !h->nal_ref_idc) ||
            (h->avctx->skip_frame >= AVDISCARD_BIDIR  && sl->slice_type_nos == AV_PICTURE_TYPE_B) ||
            (h->avctx->skip_frame >= AVDISCARD_NONINTRA && sl->slice_type_nos != AV_PICTURE_TYPE_I) ||
            (h->avctx->skip_frame >= AVDISCARD_NONKEY && h->nal_unit_type != H264_NAL_IDR_SLICE && h->sei.recovery_point.recovery_frame_cnt < 0) ||
            h->avctx->skip_frame >= AVDISCARD_ALL) {
            return 0;
        }
    }

    if (!first_slice) {
        const PPS *pps = h->ps.pps_list[sl->pps_id];

        if (h->ps.pps->sps_id != pps->sps_id ||
            h->ps.pps->transform_8x8_mode != pps->transform_8x8_mode /*||
            (h->setup_finished && h->ps.pps != pps)*/) {
            av_log(h->avctx, AV_LOG_ERROR, "PPS changed between slices\n");
            return AVERROR_INVALIDDATA;
        }
        if (h->ps.sps != pps->sps) {
            av_log(h->avctx, AV_LOG_ERROR,
               "SPS changed in the middle of the frame\n");
            return AVERROR_INVALIDDATA;
        }
    }

    if (h->current_slice == 0) {
        ret = h264_field_start(h, sl, nal, first_slice);
        if (ret < 0)
            return ret;
    } else {
        if (h->picture_structure != sl->picture_structure ||
            h->droppable         != (nal->ref_idc == 0)) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "Changing field mode (%d -> %d) between slices is not allowed\n",
                   h->picture_structure, sl->picture_structure);
            return AVERROR_INVALIDDATA;
        } else if (!h->cur_pic_ptr) {
            av_log(h->avctx, AV_LOG_ERROR,
                   "unset cur_pic_ptr on slice %d\n",
                   h->current_slice + 1);
            return AVERROR_INVALIDDATA;
        }
    }

    ret = h264_slice_init(h, sl, nal);
    if (ret < 0)
        return ret;

    h->nb_slice_ctx_queued++;

    return 0;
}

int ff_h264_get_slice_type(const H264SliceContext *sl)
{
    switch (sl->slice_type) {
    case AV_PICTURE_TYPE_P:
        return 0;
    case AV_PICTURE_TYPE_B:
        return 1;
    case AV_PICTURE_TYPE_I:
        return 2;
    case AV_PICTURE_TYPE_SP:
        return 3;
    case AV_PICTURE_TYPE_SI:
        return 4;
    default:
        return AVERROR_INVALIDDATA;
    }
}

static av_always_inline void fill_filter_caches_inter(const H264Context *h,
                                                      H264SliceContext *sl,
                                                      int mb_type, int top_xy,
                                                      const int left_xy[LEFT_MBS],
                                                      int top_type,
                                                      const int left_type[LEFT_MBS],
                                                      int mb_xy, int list)
{
    int b_stride = h->b_stride;
    int16_t(*mv_dst)[2] = &sl->mv_cache[list][scan8[0]];
    int8_t *ref_cache   = &sl->ref_cache[list][scan8[0]];
    if (IS_INTER(mb_type) || IS_DIRECT(mb_type)) {
        if (USES_LIST(top_type, list)) {
            const int b_xy  = h->mb2b_xy[top_xy] + 3 * b_stride;
            const int b8_xy = 4 * top_xy + 2;
            const int *ref2frm = &h->ref2frm[h->slice_table[top_xy] & (MAX_SLICES - 1)][list][(MB_MBAFF(sl) ? 20 : 2)];
            AV_COPY128(mv_dst - 1 * 8, h->cur_pic.motion_val[list][b_xy + 0]);
            ref_cache[0 - 1 * 8] =
            ref_cache[1 - 1 * 8] = ref2frm[h->cur_pic.ref_index[list][b8_xy + 0]];
            ref_cache[2 - 1 * 8] =
            ref_cache[3 - 1 * 8] = ref2frm[h->cur_pic.ref_index[list][b8_xy + 1]];
        } else {
            AV_ZERO128(mv_dst - 1 * 8);
            AV_WN32A(&ref_cache[0 - 1 * 8], ((LIST_NOT_USED) & 0xFF) * 0x01010101u);
        }

        if (!IS_INTERLACED(mb_type ^ left_type[LTOP])) {
            if (USES_LIST(left_type[LTOP], list)) {
                const int b_xy  = h->mb2b_xy[left_xy[LTOP]] + 3;
                const int b8_xy = 4 * left_xy[LTOP] + 1;
                const int *ref2frm = &h->ref2frm[h->slice_table[left_xy[LTOP]] & (MAX_SLICES - 1)][list][(MB_MBAFF(sl) ? 20 : 2)];
                AV_COPY32(mv_dst - 1 +  0, h->cur_pic.motion_val[list][b_xy + b_stride * 0]);
                AV_COPY32(mv_dst - 1 +  8, h->cur_pic.motion_val[list][b_xy + b_stride * 1]);
                AV_COPY32(mv_dst - 1 + 16, h->cur_pic.motion_val[list][b_xy + b_stride * 2]);
                AV_COPY32(mv_dst - 1 + 24, h->cur_pic.motion_val[list][b_xy + b_stride * 3]);
                ref_cache[-1 +  0] =
                ref_cache[-1 +  8] = ref2frm[h->cur_pic.ref_index[list][b8_xy + 2 * 0]];
                ref_cache[-1 + 16] =
                ref_cache[-1 + 24] = ref2frm[h->cur_pic.ref_index[list][b8_xy + 2 * 1]];
            } else {
                AV_ZERO32(mv_dst - 1 +  0);
                AV_ZERO32(mv_dst - 1 +  8);
                AV_ZERO32(mv_dst - 1 + 16);
                AV_ZERO32(mv_dst - 1 + 24);
                ref_cache[-1 +  0] =
                ref_cache[-1 +  8] =
                ref_cache[-1 + 16] =
                ref_cache[-1 + 24] = LIST_NOT_USED;
            }
        }
    }

    if (!USES_LIST(mb_type, list)) {
        fill_rectangle(mv_dst, 4, 4, 8, pack16to32(0, 0), 4);
        AV_WN32A(&ref_cache[0 * 8], ((LIST_NOT_USED) & 0xFF) * 0x01010101u);
        AV_WN32A(&ref_cache[1 * 8], ((LIST_NOT_USED) & 0xFF) * 0x01010101u);
        AV_WN32A(&ref_cache[2 * 8], ((LIST_NOT_USED) & 0xFF) * 0x01010101u);
        AV_WN32A(&ref_cache[3 * 8], ((LIST_NOT_USED) & 0xFF) * 0x01010101u);
        return;
    }

    {
        const int8_t *ref = &h->cur_pic.ref_index[list][4 * mb_xy];
        const int *ref2frm = &h->ref2frm[sl->slice_num & (MAX_SLICES - 1)][list][(MB_MBAFF(sl) ? 20 : 2)];
        uint32_t ref01 = (pack16to32(ref2frm[ref[0]], ref2frm[ref[1]]) & 0x00FF00FF) * 0x0101;
        uint32_t ref23 = (pack16to32(ref2frm[ref[2]], ref2frm[ref[3]]) & 0x00FF00FF) * 0x0101;
        AV_WN32A(&ref_cache[0 * 8], ref01);
        AV_WN32A(&ref_cache[1 * 8], ref01);
        AV_WN32A(&ref_cache[2 * 8], ref23);
        AV_WN32A(&ref_cache[3 * 8], ref23);
    }

    {
        int16_t(*mv_src)[2] = &h->cur_pic.motion_val[list][4 * sl->mb_x + 4 * sl->mb_y * b_stride];
        AV_COPY128(mv_dst + 8 * 0, mv_src + 0 * b_stride);
        AV_COPY128(mv_dst + 8 * 1, mv_src + 1 * b_stride);
        AV_COPY128(mv_dst + 8 * 2, mv_src + 2 * b_stride);
        AV_COPY128(mv_dst + 8 * 3, mv_src + 3 * b_stride);
    }
}

/**
 * @return non zero if the loop filter can be skipped
 */
static int fill_filter_caches(const H264Context *h, H264SliceContext *sl, int mb_type)
{
    const int mb_xy = sl->mb_xy;
    int top_xy, left_xy[LEFT_MBS];
    int top_type, left_type[LEFT_MBS];
    const uint8_t *nnz;
    uint8_t *nnz_cache;

    top_xy = mb_xy - (h->mb_stride << MB_FIELD(sl));

    left_xy[LBOT] = left_xy[LTOP] = mb_xy - 1;
    if (FRAME_MBAFF(h)) {
        const int left_mb_field_flag = IS_INTERLACED(h->cur_pic.mb_type[mb_xy - 1]);
        const int curr_mb_field_flag = IS_INTERLACED(mb_type);
        if (sl->mb_y & 1) {
            if (left_mb_field_flag != curr_mb_field_flag)
                left_xy[LTOP] -= h->mb_stride;
        } else {
            if (curr_mb_field_flag)
                top_xy += h->mb_stride &
                          (((h->cur_pic.mb_type[top_xy] >> 7) & 1) - 1);
            if (left_mb_field_flag != curr_mb_field_flag)
                left_xy[LBOT] += h->mb_stride;
        }
    }

    sl->top_mb_xy        = top_xy;
    sl->left_mb_xy[LTOP] = left_xy[LTOP];
    sl->left_mb_xy[LBOT] = left_xy[LBOT];
    {
        /* For sufficiently low qp, filtering wouldn't do anything.
         * This is a conservative estimate: could also check beta_offset
         * and more accurate chroma_qp. */
        int qp_thresh = sl->qp_thresh; // FIXME strictly we should store qp_thresh for each mb of a slice
        int qp        = h->cur_pic.qscale_table[mb_xy];
        if (qp <= qp_thresh &&
            (left_xy[LTOP] < 0 ||
             ((qp + h->cur_pic.qscale_table[left_xy[LTOP]] + 1) >> 1) <= qp_thresh) &&
            (top_xy < 0 ||
             ((qp + h->cur_pic.qscale_table[top_xy] + 1) >> 1) <= qp_thresh)) {
            if (!FRAME_MBAFF(h))
                return 1;
            if ((left_xy[LTOP] < 0 ||
                 ((qp + h->cur_pic.qscale_table[left_xy[LBOT]] + 1) >> 1) <= qp_thresh) &&
                (top_xy < h->mb_stride ||
                 ((qp + h->cur_pic.qscale_table[top_xy - h->mb_stride] + 1) >> 1) <= qp_thresh))
                return 1;
        }
    }

    top_type        = h->cur_pic.mb_type[top_xy];
    left_type[LTOP] = h->cur_pic.mb_type[left_xy[LTOP]];
    left_type[LBOT] = h->cur_pic.mb_type[left_xy[LBOT]];
    if (sl->deblocking_filter == 2) {
        if (h->slice_table[top_xy] != sl->slice_num)
            top_type = 0;
        if (h->slice_table[left_xy[LBOT]] != sl->slice_num)
            left_type[LTOP] = left_type[LBOT] = 0;
    } else {
        if (h->slice_table[top_xy] == 0xFFFF)
            top_type = 0;
        if (h->slice_table[left_xy[LBOT]] == 0xFFFF)
            left_type[LTOP] = left_type[LBOT] = 0;
    }
    sl->top_type        = top_type;
    sl->left_type[LTOP] = left_type[LTOP];
    sl->left_type[LBOT] = left_type[LBOT];

    if (IS_INTRA(mb_type))
        return 0;

    fill_filter_caches_inter(h, sl, mb_type, top_xy, left_xy,
                             top_type, left_type, mb_xy, 0);
    if (sl->list_count == 2)
        fill_filter_caches_inter(h, sl, mb_type, top_xy, left_xy,
                                 top_type, left_type, mb_xy, 1);

    nnz       = h->non_zero_count[mb_xy];
    nnz_cache = sl->non_zero_count_cache;
    AV_COPY32(&nnz_cache[4 + 8 * 1], &nnz[0]);
    AV_COPY32(&nnz_cache[4 + 8 * 2], &nnz[4]);
    AV_COPY32(&nnz_cache[4 + 8 * 3], &nnz[8]);
    AV_COPY32(&nnz_cache[4 + 8 * 4], &nnz[12]);
    sl->cbp = h->cbp_table[mb_xy];

    if (top_type) {
        nnz = h->non_zero_count[top_xy];
        AV_COPY32(&nnz_cache[4 + 8 * 0], &nnz[3 * 4]);
    }

    if (left_type[LTOP]) {
        nnz = h->non_zero_count[left_xy[LTOP]];
        nnz_cache[3 + 8 * 1] = nnz[3 + 0 * 4];
        nnz_cache[3 + 8 * 2] = nnz[3 + 1 * 4];
        nnz_cache[3 + 8 * 3] = nnz[3 + 2 * 4];
        nnz_cache[3 + 8 * 4] = nnz[3 + 3 * 4];
    }

    /* CAVLC 8x8dct requires NNZ values for residual decoding that differ
     * from what the loop filter needs */
    if (!CABAC(h) && h->ps.pps->transform_8x8_mode) {
        if (IS_8x8DCT(top_type)) {
            nnz_cache[4 + 8 * 0] =
            nnz_cache[5 + 8 * 0] = (h->cbp_table[top_xy] & 0x4000) >> 12;
            nnz_cache[6 + 8 * 0] =
            nnz_cache[7 + 8 * 0] = (h->cbp_table[top_xy] & 0x8000) >> 12;
        }
        if (IS_8x8DCT(left_type[LTOP])) {
            nnz_cache[3 + 8 * 1] =
            nnz_cache[3 + 8 * 2] = (h->cbp_table[left_xy[LTOP]] & 0x2000) >> 12; // FIXME check MBAFF
        }
        if (IS_8x8DCT(left_type[LBOT])) {
            nnz_cache[3 + 8 * 3] =
            nnz_cache[3 + 8 * 4] = (h->cbp_table[left_xy[LBOT]] & 0x8000) >> 12; // FIXME check MBAFF
        }

        if (IS_8x8DCT(mb_type)) {
            nnz_cache[scan8[0]] =
            nnz_cache[scan8[1]] =
            nnz_cache[scan8[2]] =
            nnz_cache[scan8[3]] = (sl->cbp & 0x1000) >> 12;

            nnz_cache[scan8[0 + 4]] =
            nnz_cache[scan8[1 + 4]] =
            nnz_cache[scan8[2 + 4]] =
            nnz_cache[scan8[3 + 4]] = (sl->cbp & 0x2000) >> 12;

            nnz_cache[scan8[0 + 8]] =
            nnz_cache[scan8[1 + 8]] =
            nnz_cache[scan8[2 + 8]] =
            nnz_cache[scan8[3 + 8]] = (sl->cbp & 0x4000) >> 12;

            nnz_cache[scan8[0 + 12]] =
            nnz_cache[scan8[1 + 12]] =
            nnz_cache[scan8[2 + 12]] =
            nnz_cache[scan8[3 + 12]] = (sl->cbp & 0x8000) >> 12;
        }
    }

    return 0;
}

static void loop_filter(const H264Context *h, H264SliceContext *sl, int start_x, int end_x)
{
    uint8_t *dest_y, *dest_cb, *dest_cr;
    int linesize, uvlinesize, mb_x, mb_y;
    const int end_mb_y       = sl->mb_y + FRAME_MBAFF(h);
    const int old_slice_type = sl->slice_type;
    const int pixel_shift    = h->pixel_shift;
    const int block_h        = 16 >> h->chroma_y_shift;

    if (h->postpone_filter)
        return;

    if (sl->deblocking_filter) {
        for (mb_x = start_x; mb_x < end_x; mb_x++)
            for (mb_y = end_mb_y - FRAME_MBAFF(h); mb_y <= end_mb_y; mb_y++) {
                int mb_xy, mb_type;
                mb_xy         = sl->mb_xy = mb_x + mb_y * h->mb_stride;
                mb_type       = h->cur_pic.mb_type[mb_xy];

                if (FRAME_MBAFF(h))
                    sl->mb_mbaff               =
                    sl->mb_field_decoding_flag = !!IS_INTERLACED(mb_type);

                sl->mb_x = mb_x;
                sl->mb_y = mb_y;
                dest_y  = h->cur_pic.f->data[0] +
                          ((mb_x << pixel_shift) + mb_y * sl->linesize) * 16;
                dest_cb = h->cur_pic.f->data[1] +
                          (mb_x << pixel_shift) * (8 << CHROMA444(h)) +
                          mb_y * sl->uvlinesize * block_h;
                dest_cr = h->cur_pic.f->data[2] +
                          (mb_x << pixel_shift) * (8 << CHROMA444(h)) +
                          mb_y * sl->uvlinesize * block_h;
                // FIXME simplify above

                if (MB_FIELD(sl)) {
                    linesize   = sl->mb_linesize   = sl->linesize   * 2;
                    uvlinesize = sl->mb_uvlinesize = sl->uvlinesize * 2;
                    if (mb_y & 1) { // FIXME move out of this function?
                        dest_y  -= sl->linesize   * 15;
                        dest_cb -= sl->uvlinesize * (block_h - 1);
                        dest_cr -= sl->uvlinesize * (block_h - 1);
                    }
                } else {
                    linesize   = sl->mb_linesize   = sl->linesize;
                    uvlinesize = sl->mb_uvlinesize = sl->uvlinesize;
                }
                backup_mb_border(h, sl, dest_y, dest_cb, dest_cr, linesize,
                                 uvlinesize, 0);
                if (fill_filter_caches(h, sl, mb_type))
                    continue;
                sl->chroma_qp[0] = get_chroma_qp(h->ps.pps, 0, h->cur_pic.qscale_table[mb_xy]);
                sl->chroma_qp[1] = get_chroma_qp(h->ps.pps, 1, h->cur_pic.qscale_table[mb_xy]);

                if (FRAME_MBAFF(h)) {
                    ff_h264_filter_mb(h, sl, mb_x, mb_y, dest_y, dest_cb, dest_cr,
                                      linesize, uvlinesize);
                } else {
                    ff_h264_filter_mb_fast(h, sl, mb_x, mb_y, dest_y, dest_cb,
                                           dest_cr, linesize, uvlinesize);
                }
            }
    }
    sl->slice_type  = old_slice_type;
    sl->mb_x         = end_x;
    sl->mb_y         = end_mb_y - FRAME_MBAFF(h);
    sl->chroma_qp[0] = get_chroma_qp(h->ps.pps, 0, sl->qscale);
    sl->chroma_qp[1] = get_chroma_qp(h->ps.pps, 1, sl->qscale);
}

static void predict_field_decoding_flag(const H264Context *h, H264SliceContext *sl)
{
    const int mb_xy = sl->mb_x + sl->mb_y * h->mb_stride;
    int mb_type     = (h->slice_table[mb_xy - 1] == sl->slice_num) ?
                      h->cur_pic.mb_type[mb_xy - 1] :
                      (h->slice_table[mb_xy - h->mb_stride] == sl->slice_num) ?
                      h->cur_pic.mb_type[mb_xy - h->mb_stride] : 0;
    sl->mb_mbaff    = sl->mb_field_decoding_flag = IS_INTERLACED(mb_type) ? 1 : 0;
}

/**
 * Draw edges and report progress for the last MB row.
 */
static void decode_finish_row(const H264Context *h, H264SliceContext *sl)
{
    int top            = 16 * (sl->mb_y      >> FIELD_PICTURE(h));
    int pic_height     = 16 *  h->mb_height >> FIELD_PICTURE(h);
    int height         =  16      << FRAME_MBAFF(h);
    int deblock_border = (16 + 4) << FRAME_MBAFF(h);

    if (sl->deblocking_filter) {
        if ((top + height) >= pic_height)
            height += deblock_border;
        top -= deblock_border;
    }

    if (top >= pic_height || (top + height) < 0)
        return;

    height = FFMIN(height, pic_height - top);
    if (top < 0) {
        height = top + height;
        top    = 0;
    }

    ff_h264_draw_horiz_band(h, sl, top, height);

    // Droppable pictures need no progress reports in single-view streams,
    // but in multiview a droppable picture may still be referenced across
    // views, so it must report like any other picture.
    if (h->er.error_occurred)
        return;
    if (h->droppable && h->view_count <= 1)
        return;

    ff_thread_report_progress(&h->cur_pic_ptr->tf, top + height - 1,
                              h->picture_structure == PICT_BOTTOM_FIELD);
}

static void er_add_slice(H264SliceContext *sl,
                         int startx, int starty,
                         int endx, int endy, int status)
{
    if (!sl->h264->enable_er)
        return;

    if (CONFIG_ERROR_RESILIENCE) {
        ff_er_add_slice(sl->er, startx, starty, endx, endy, status);
    }
}

/**
 * 2D+delta MVC: may this slice be completed as skipped macroblocks
 * against reference list 0? Runtime fallback for a dependent slice whose
 * payload genuinely fails to decode; the correct output is essentially a
 * copy of the base view reference of the same POC, so skipping against
 * list 0 reference 0 reproduces the undamaged picture instead of a
 * concealed one.
 *
 * Guarded to progressive P slices of dependent MVC views with a valid
 * reference (plain H.264 and the base view never take this path); slice
 * threading is excluded: the fill re-decodes to the end of the picture
 * and would race the other slice contexts.
 */
static int dependent_slice_fillable(const H264Context *h, H264SliceContext *sl)
{
    return h->view_count > 1 && h->cur_view > 0 &&
           !(h->avctx->active_thread_type & FF_THREAD_SLICE) &&
           h->ps.sps && h->ps.sps->mvc.present &&
           !FIELD_OR_MBAFF_PICTURE(h) &&
           sl->slice_type_nos == AV_PICTURE_TYPE_P &&
           sl->ref_count[0] > 0 &&
           sl->ref_list[0][0].parent != NULL &&
           sl->ref_list[0][0].data[0] != NULL;
}

/**
 * Is this slice's macroblock region already covered by a dependent
 * anchor fill earlier in the picture (see dependent_fill_account())?
 * The fill re-decoded those macroblocks and accounted for the whole
 * region, so the slice's own ER_MB_END bookkeeping must be suppressed -
 * or ff_er_frame_end() would see a negative error count and conceal an
 * undamaged picture.
 */
static int dependent_fill_covers(const H264Context *h,
                                 const H264SliceContext *sl)
{
    return h->dep_fill_first >= 0 && h->dep_fill_first < sl->first_mb_addr;
}

/**
 * Decode the picture region [start_addr, end_addr) as skipped
 * (PART_P_SKIP) macroblocks against reference list 0 entry 0. Used by
 * the 2D+delta dependent view paths: those pictures decode as an
 * (essentially) empty delta - a copy of the base view reference frame
 * of the same POC - and the motion prediction resolves to reference 0
 * with a zero MV, so the macroblocks are exact copies.
 *
 * Per macroblock the fill leaves precisely the state a normally decoded
 * skipped macroblock leaves, advancing mb_x/mb_y like decode_slice();
 * each completed row is filtered in full from column 0 (the macroblocks
 * before the region never received their row filter pass). On return
 * sl->mb_x/sl->mb_y sit at or beyond end_addr and *lf_x_start has no
 * pending work.
 */
static void fill_dep_region_as_skip(const H264Context *h,
                                    H264SliceContext *sl,
                                    int start_addr, int end_addr,
                                    int *lf_x_start)
{
    sl->mb_x        = start_addr % h->mb_width;
    sl->mb_y        = start_addr / h->mb_width;
    *lf_x_start     = 0;
    sl->mb_skip_run = -1;
    while (sl->mb_x + sl->mb_y * h->mb_width < end_addr) {
        sl->mb_xy = sl->mb_x + sl->mb_y * h->mb_stride;
        h->cbp_table[sl->mb_xy] = 0;
        h->chroma_pred_mode_table[sl->mb_xy] = 0;
        sl->last_qscale_diff = 0;
        decode_mb_skip(h, sl);
        ff_h264_hl_decode_mb(h, sl);

        if (++sl->mb_x >= h->mb_width) {
            loop_filter(h, sl, *lf_x_start, sl->mb_x);
            sl->mb_x = 0;
            *lf_x_start = 0;
            decode_finish_row(h, sl);
            ++sl->mb_y;
            if (sl->mb_y >= h->mb_height)
                break;
        }
    }
}

/**
 * Complete a dependent slice whose payload failed to decode (see
 * dependent_slice_fillable).
 *
 * Re-decodes from the failing slice's *first* macroblock, not the
 * failing one: macroblocks consumed before the failure may hold garbage
 * from the truncated/desynced entropy stream, and in the sequential
 * single-slice-context setup these streams use that region extends to
 * the end of the picture (later slices decode over the filled
 * macroblocks, their payloads intact).
 *
 * Do NOT preserve the prefix macroblocks of a fully failed slice: the
 * bands are plain self-contained CABAC payloads, so the prefixes are
 * reads of a misaligned stream, not genuine content - whole-band base
 * copies are pixel-identical to the expected output, keeping the
 * prefixes regresses the frames that reference them.
 */
static void fill_dependent_anchor_slice_as_skip(const H264Context *h,
                                                H264SliceContext *sl,
                                                int *lf_x_start)
{
    static int fill_logged;
    if (!fill_logged) {
        fill_logged = 1;
        av_log(h->avctx, AV_LOG_WARNING,
               "dependent slice decode failure at MB %d %d; completing the "
               "picture as skipped macroblocks against the base view "
               "reference\n", sl->resync_mb_x, sl->resync_mb_y);
    }
    fill_dep_region_as_skip(h, sl, sl->first_mb_addr, h->mb_num, lf_x_start);
}

/**
 * Account for the region covered by the dependent anchor fill in the
 * error-resilience state. Runs only the first time the picture gets
 * filled: every macroblock from the filling slice's first to the
 * picture's last is marked error-free and the error count is deducted
 * for exactly that region; later in-region slices suppress their own
 * accounting (dependent_fill_covers()), so the picture reaches
 * ff_er_frame_end() with an error count of zero and is not concealed.
 * The region ends at the picture's last macroblock: ff_er_add_slice()
 * treats end_i == s->mb_num as a hard error.
 */
static void dependent_fill_account(H264Context *h, H264SliceContext *sl)
{
    if (h->dep_fill_first < 0) {
        h->dep_fill_first = sl->first_mb_addr;
        er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y,
                     h->mb_width - 1, h->mb_height - 1, ER_MB_END);
    }
}

/**
 * 2D+delta MVC, at picture end: complete the macroblocks that a
 * truncated dependent slice NAL left undecoded.
 *
 * These streams also carry slice NALs whose CABAC payload ends before
 * the slice region is complete; the decoder "completes" the slice on
 * the entropy terminator, leaving a contiguous run of unaccounted
 * macroblocks that ff_er_frame_end() would conceal as garbage. The
 * correct output reads the exhausted payload as zeros: skipped copies
 * of the base view reference over the whole region.
 *
 * Detects that situation from the error-resilience state (one
 * contiguous untouched run whose size matches the remaining error
 * count, no other error) and completes it the same way as the in-slice
 * fill, with the ER_MB_END accounting restricted to the region and a
 * passing error_occurred cleared.
 *
 * Returns the number of completed macroblocks, 0 when nothing applied;
 * safe to call unconditionally (plain H.264 and base views cannot match
 * the gate, intact pictures leave no untouched run).
 */
int ff_h264_complete_truncated_region(H264Context *h)
{
    H264SliceContext *const sl = h->slice_ctx;
    const uint8_t *const tab = h->er.error_status_table;
    const int *const xy_of_mb = h->er.mb_index2xy;
    const int err_bits = ER_AC_ERROR | ER_DC_ERROR | ER_MV_ERROR;
    const int mb_num = h->mb_num;
    int ec, i, g0 = -1, g1 = 0;
    int in_run = 0, second_run = 0;

    /* The table holds the state of the picture currently being decoded
     * only while at least one of its slices was queued; an access unit
     * without slices would otherwise see the previous picture's state. */
    if (!h->cur_pic_ptr || !h->current_slice)
        return 0;

    if (!tab || !xy_of_mb || !dependent_slice_fillable(h, sl))
        return 0;

    ec = (int) atomic_load(&h->er.error_count);
    if (ec <= 0 || ec % 3)
        return 0;

    /* The table is a padded mb_stride-wide array (stride > mb_width), so
     * macroblocks are addressed through mb_index2xy, like the rest of
     * the error-resilience code. */
    for (i = 0; i < mb_num; i++) {
        if (tab[xy_of_mb[i]] & err_bits) {
            if (in_run)
                g1 = i + 1;
            else {
                in_run = 1;
                if (g0 >= 0)
                    second_run = 1;
                g0 = i;
                g1 = i + 1;
            }
        } else if (in_run)
            in_run = 0;
    }
    if (g0 < 0 || second_run || ec / 3 != g1 - g0)
        return 0;

    static int region_logged;
    if (!region_logged) {
        region_logged = 1;
        av_log(h->avctx, AV_LOG_WARNING,
               "truncated dependent slice left %d macroblocks undecoded "
               "from MB %d %d; completing the region as skipped "
               "macroblocks against the base view reference\n",
               g1 - g0, g0 % h->mb_width, g0 / h->mb_width);
    }

    {
        int lf_x_start;
        fill_dep_region_as_skip(h, sl, g0, g1, &lf_x_start);
        er_add_slice(sl, g0 % h->mb_width, g0 / h->mb_width,
                     (g1 - 1) % h->mb_width, (g1 - 1) / h->mb_width,
                     ER_MB_END);
    }
    h->er.error_occurred = 0;

    return g1 - g0;
}

static int decode_slice(struct AVCodecContext *avctx, void *arg)
{
    H264SliceContext *sl = arg;
    const H264Context *h = sl->h264;
    int lf_x_start = sl->mb_x;
    int orig_deblock = sl->deblocking_filter;
    int ret;
    /* Bit-exact CABAC payload copy for the dependent view (NULL for the
     * base view and for CAVLC; freed on finish and early CABAC errors). */
    uint8_t *dep_cabac_buf = NULL;
    int dep_cabac_bytes = 0;

    sl->linesize   = h->cur_pic_ptr->f->linesize[0];
    sl->uvlinesize = h->cur_pic_ptr->f->linesize[1];

    ret = alloc_scratch_buffers(sl, sl->linesize);
    if (ret < 0)
        return ret;

    sl->mb_skip_run = -1;

    av_assert0(h->block_offset[15] == (4 * ((scan8[15] - scan8[0]) & 7) << h->pixel_shift) + 4 * sl->linesize * ((scan8[15] - scan8[0]) >> 3));

    if (h->postpone_filter)
        sl->deblocking_filter = 0;

    sl->is_complex = FRAME_MBAFF(h) || h->picture_structure != PICT_FRAME ||
                     (CONFIG_GRAY && (h->flags & AV_CODEC_FLAG_GRAY));

    /* Also suppressed inside a dependent anchor fill region
     * (dependent_fill_covers()): the fill marks the whole region
     * error-free in one shot, so the boundary macroblocks left behind
     * carry no ER_MB_END and would flag every later slice as an error. */
    if (!(h->avctx->active_thread_type & FF_THREAD_SLICE) && h->picture_structure == PICT_FRAME && sl->er->error_status_table &&
        !dependent_fill_covers(h, sl)) {
        const int start_i  = av_clip(sl->resync_mb_x + sl->resync_mb_y * h->mb_width, 0, h->mb_num - 1);
        if (start_i) {
            int prev_status = sl->er->error_status_table[sl->er->mb_index2xy[start_i - 1]];
            prev_status &= ~ VP_START;
            if (prev_status != (ER_MV_END | ER_DC_END | ER_AC_END))
                sl->er->error_occurred = 1;
        }
    }

    if (h->ps.pps->cabac) {
        /* 2D+delta MVC dependent anchor bands: the correct decode drops
         * the 1-7 bits after the slice-header end, so the CABAC payload
         * starts at the first byte boundary at or after the header end -
         * the same start the stock aligned handoff below computes. The
         * private copy exists to keep the +2/+4 end-of-slice over-reads
         * in bounds via the +8 zero pad and to drive the band-boundary
         * finish logic below (verified byte-exact on the affected
         * streams). In-epoch dependent slices keep the stock aligned
         * handoff. */
        if (h->view_count > 1 && h->cur_view > 0 && h->ps.sps->mvc.present &&
            sl->mvc_anchor) {
            int e    = get_bits_count(&sl->gb);
            int bits = get_bits_left(&sl->gb);
            int start = (e - 32 + 7) >> 3; /* type-20: 4-byte NAL header */
            int i;

            /* Buffer layout / bound. sl->gb is init'd on the NAL itself
             * (h2645_parse.c), so gb.buffer points at the NAL's first
             * byte (the 4-byte type-20 header is its first part).
             * The copy length is the UNALIGNED bound - ceil(bits/8)
             * with bits measured from the UNALIGNED header end
             * (get_bits_left at e) - clamped to the NAL's byte extent.
             * The stock aligned bound ((size_in_bits - FFALIGN(e,8) +
             * 7) / 8) is exactly one byte shorter for 0x80-terminated
             * NALs with a misaligned header end (it stops at the
             * stop-bit byte, never reaching the NAL's last byte); A/B
             * verification shows the unaligned bound is the correct
             * one - still inside the NAL extent, with the NAL-extent
             * clamp, the 3-byte guard below and the +8 zero pad
             * unchanged. Clamping below the 3-byte guard defers to the
             * stock aligned handoff (the only state a pathologically
             * short NAL could produce). */
            dep_cabac_bytes = (bits + 7) / 8;
            dep_cabac_bytes = FFMIN(dep_cabac_bytes,
                                     sl->nal_size - 4 - start);
            /* Need the 3 bytes ff_init_cabac_decoder() may fetch ahead;
             * the +8 zero pad keeps renormalizing over-reads near the
             * logical end in bounds, as the +2/+4 checks expect. */
            dep_cabac_buf = dep_cabac_bytes >= 3 ?
                av_mallocz(dep_cabac_bytes + 8) : NULL;
            if (dep_cabac_buf) {
                const uint8_t *raw = sl->gb.buffer + 4 + start;

                for (i = 0; i < dep_cabac_bytes; i++)
                    dep_cabac_buf[i] = raw[i];
            }
        }

        /* init cabac */
        if (dep_cabac_buf)
            ret = ff_init_cabac_decoder(&sl->cabac, dep_cabac_buf,
                                         dep_cabac_bytes);
        else {
            /* realign */
            align_get_bits(&sl->gb);

            ret = ff_init_cabac_decoder(&sl->cabac,
                                  sl->gb.buffer + get_bits_count(&sl->gb) / 8,
                                  (get_bits_left(&sl->gb) + 7) / 8);
        }
        if (ret < 0) {
            av_free(dep_cabac_buf);
            return ret;
        }

        ff_h264_init_cabac_states(h, sl);

        for (;;) {
            int ret, eos;
            if (sl->mb_x + sl->mb_y * h->mb_width >= sl->next_slice_idx) {
                if (dep_cabac_buf &&
                    sl->cabac.bytestream <= sl->cabac.bytestream_end + 2) {
                    /* A dependent band finished all of its macroblocks
                     * at the band boundary with the payload intact (the
                     * +2 allowance matches the stock over-read threshold:
                     * the boundary's own end-of-slice renorm may pull the
                     * final 2-byte pair one step past the logical end,
                     * while a truncated/corrupt band keeps running past
                     * +4). Finish like the EOS path instead of flagging
                     * a slice overlap. */
                    if (!dependent_fill_covers(h, sl))
                        er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y,
                                     sl->mb_x - 1, sl->mb_y, ER_MB_END);
                    if (sl->mb_x > lf_x_start)
                        loop_filter(h, sl, lf_x_start, sl->mb_x);
                    goto finish;
                }
                av_log(h->avctx, AV_LOG_ERROR, "Slice overlaps with next at %d\n",
                       sl->next_slice_idx);
                er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y, sl->mb_x,
                             sl->mb_y, ER_MB_ERROR);
                av_free(dep_cabac_buf);
                return AVERROR_INVALIDDATA;
            }

            ret = ff_h264_decode_mb_cabac(h, sl);

            if (ret >= 0)
                ff_h264_hl_decode_mb(h, sl);

            // FIXME optimal? or let mb_decode decode 16x32 ?
            if (ret >= 0 && FRAME_MBAFF(h)) {
                sl->mb_y++;

                ret = ff_h264_decode_mb_cabac(h, sl);

                if (ret >= 0)
                    ff_h264_hl_decode_mb(h, sl);
                sl->mb_y--;
            }
            eos = get_cabac_terminate(&sl->cabac);

            /* Dependent anchor bands occasionally close their CABAC
             * payload with the end-of-slice state unset and no payload
             * left; the correct decode reads the zero padding there,
             * the overrun macroblocks falling inside the next band
             * slice's region, where they are re-decoded. Decoding one
             * macroblock further would hard-fail and trip the fill
             * gate, repainting the band's genuine rows and poisoning
             * reference frames, so the exhausted payload is treated as
             * end-of-slice (a genuinely truncated last band is
             * completed as skipped macroblocks by
             * ff_h264_complete_truncated_region()).
             * bytestream >= bytestream_end + 1 means the range coder's
             * last refill pair already straddled the logical end, so
             * every further refill is zero-only; +0 is excluded (its
             * in-flight state still holds the last two real payload
             * bytes). Both thresholds fire ONLY at a row end outside
             * the picture's last row (mb_x == mb_width - 1, mb_y <
             * mb_height - 1): a +1 tail in the final row is a genuine
             * continuation, and a mid-row fire (+1, or +2 only
             * reachable on a truncated NAL) leaves an undecoded row
             * tail no later NAL of this picture covers. Band
             * boundaries in the affected streams are row-aligned, so
             * the genuine handoff completions keep firing;
             * sl->next_slice_idx is NOT used as the handoff test (it
             * reads the picture end on per-NAL-packetized streams). A
             * healthy band at +1 with the end-of-slice state set is
             * excluded by the !eos guard. */
            if (!eos && ret >= 0 && sl->mvc_anchor && dep_cabac_buf &&
                sl->mb_x == h->mb_width - 1 &&
                (sl->cabac.bytestream >= sl->cabac.bytestream_end + 2 ||
                 (sl->cabac.bytestream == sl->cabac.bytestream_end + 1 &&
                  sl->mb_y < h->mb_height - 1 &&
                  sl->mb_x == h->mb_width - 1)))
                eos = (int) (sl->cabac.bytestream -
                             sl->cabac.bytestream_start);

            if ((h->workaround_bugs & FF_BUG_TRUNCATED) &&
                sl->cabac.bytestream > sl->cabac.bytestream_end + 2) {
                er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y, sl->mb_x - 1,
                             sl->mb_y, ER_MB_END);
                if (sl->mb_x >= lf_x_start)
                    loop_filter(h, sl, lf_x_start, sl->mb_x + 1);
                goto finish;
            }
            if (sl->cabac.bytestream > sl->cabac.bytestream_end + 2 )
                av_log(h->avctx, AV_LOG_DEBUG, "bytestream overread %td\n", sl->cabac.bytestream_end - sl->cabac.bytestream);
            if (ret < 0 || sl->cabac.bytestream > sl->cabac.bytestream_end + 4) {
                if (dependent_slice_fillable(h, sl)) {
                    fill_dependent_anchor_slice_as_skip(h, sl, &lf_x_start);
                    dependent_fill_account((H264Context *) sl->h264, sl);
                    goto finish;
                }
                av_log(h->avctx, AV_LOG_ERROR,
                       "error while decoding MB %d %d, bytestream %td\n",
                       sl->mb_x, sl->mb_y,
                       sl->cabac.bytestream_end - sl->cabac.bytestream);
                er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y, sl->mb_x,
                             sl->mb_y, ER_MB_ERROR);
                av_free(dep_cabac_buf);
                return AVERROR_INVALIDDATA;
            }

            if (++sl->mb_x >= h->mb_width) {
                loop_filter(h, sl, lf_x_start, sl->mb_x);
                sl->mb_x = lf_x_start = 0;
                decode_finish_row(h, sl);
                ++sl->mb_y;
                if (FIELD_OR_MBAFF_PICTURE(h)) {
                    ++sl->mb_y;
                    if (FRAME_MBAFF(h) && sl->mb_y < h->mb_height)
                        predict_field_decoding_flag(h, sl);
                }
            }

            if (eos || sl->mb_y >= h->mb_height) {
                ff_tlog(h->avctx, "slice end %d %d\n",
                        get_bits_count(&sl->gb), sl->gb.size_in_bits);
                if (!dependent_fill_covers(h, sl))
                    er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y,
                                 sl->mb_x - 1, sl->mb_y, ER_MB_END);
                if (sl->mb_x > lf_x_start)
                    loop_filter(h, sl, lf_x_start, sl->mb_x);
                goto finish;
            }
        }
    } else {
        for (;;) {
            int ret;

            if (sl->mb_x + sl->mb_y * h->mb_width >= sl->next_slice_idx) {
                av_log(h->avctx, AV_LOG_ERROR, "Slice overlaps with next at %d\n",
                       sl->next_slice_idx);
                er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y, sl->mb_x,
                             sl->mb_y, ER_MB_ERROR);
                return AVERROR_INVALIDDATA;
            }

            ret = ff_h264_decode_mb_cavlc(h, sl);

            if (ret >= 0)
                ff_h264_hl_decode_mb(h, sl);

            // FIXME optimal? or let mb_decode decode 16x32 ?
            if (ret >= 0 && FRAME_MBAFF(h)) {
                sl->mb_y++;
                ret = ff_h264_decode_mb_cavlc(h, sl);

                if (ret >= 0)
                    ff_h264_hl_decode_mb(h, sl);
                sl->mb_y--;
            }

            if (ret < 0) {
                if (dependent_slice_fillable(h, sl)) {
                    fill_dependent_anchor_slice_as_skip(h, sl, &lf_x_start);
                    dependent_fill_account((H264Context *) sl->h264, sl);
                    goto finish;
                }
                av_log(h->avctx, AV_LOG_ERROR,
                       "error while decoding MB %d %d\n", sl->mb_x, sl->mb_y);
                er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y, sl->mb_x,
                             sl->mb_y, ER_MB_ERROR);
                return ret;
            }

            if (++sl->mb_x >= h->mb_width) {
                loop_filter(h, sl, lf_x_start, sl->mb_x);
                sl->mb_x = lf_x_start = 0;
                decode_finish_row(h, sl);
                ++sl->mb_y;
                if (FIELD_OR_MBAFF_PICTURE(h)) {
                    ++sl->mb_y;
                    if (FRAME_MBAFF(h) && sl->mb_y < h->mb_height)
                        predict_field_decoding_flag(h, sl);
                }
                if (sl->mb_y >= h->mb_height) {
                    ff_tlog(h->avctx, "slice end %d %d\n",
                            get_bits_count(&sl->gb), sl->gb.size_in_bits);

                    if (   get_bits_left(&sl->gb) == 0
                        || get_bits_left(&sl->gb) > 0 && !(h->avctx->err_recognition & AV_EF_AGGRESSIVE)) {
                        if (!dependent_fill_covers(h, sl))
                            er_add_slice(sl, sl->resync_mb_x,
                                         sl->resync_mb_y,
                                         sl->mb_x - 1, sl->mb_y,
                                         ER_MB_END);

                        goto finish;
                    } else {
                        er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y,
                                     sl->mb_x, sl->mb_y, ER_MB_END);

                        return AVERROR_INVALIDDATA;
                    }
                }
            }

            if (get_bits_left(&sl->gb) <= 0 && sl->mb_skip_run <= 0) {
                ff_tlog(h->avctx, "slice end %d %d\n",
                        get_bits_count(&sl->gb), sl->gb.size_in_bits);

                if (get_bits_left(&sl->gb) == 0) {
                    if (!dependent_fill_covers(h, sl))
                        er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y,
                                     sl->mb_x - 1, sl->mb_y, ER_MB_END);
                    if (sl->mb_x > lf_x_start)
                        loop_filter(h, sl, lf_x_start, sl->mb_x);

                    goto finish;
                } else {
                    er_add_slice(sl, sl->resync_mb_x, sl->resync_mb_y, sl->mb_x,
                                 sl->mb_y, ER_MB_ERROR);

                    return AVERROR_INVALIDDATA;
                }
            }
        }
    }

finish:
    av_free(dep_cabac_buf);
    sl->deblocking_filter = orig_deblock;
    return 0;
}

/**
 * Call decode_slice() for each context.
 *
 * @param h h264 master context
 */
int ff_h264_execute_decode_slices(H264Context *h)
{
    AVCodecContext *const avctx = h->avctx;
    H264SliceContext *sl;
    int context_count = h->nb_slice_ctx_queued;
    int ret = 0;
    int i, j;

    h->slice_ctx[0].next_slice_idx = INT_MAX;

    if (h->avctx->hwaccel || context_count < 1)
        return 0;

    av_assert0(context_count && h->slice_ctx[context_count - 1].mb_y < h->mb_height);

    if (context_count == 1) {

        h->slice_ctx[0].next_slice_idx = h->mb_width * h->mb_height;
        h->postpone_filter = 0;

        ret = decode_slice(avctx, &h->slice_ctx[0]);
        h->mb_y = h->slice_ctx[0].mb_y;
        if (ret < 0)
            goto finish;
    } else {
        av_assert0(context_count > 0);
        for (i = 0; i < context_count; i++) {
            int next_slice_idx = h->mb_width * h->mb_height;
            int slice_idx;

            sl                 = &h->slice_ctx[i];

            /* make sure none of those slices overlap */
            slice_idx = sl->mb_y * h->mb_width + sl->mb_x;
            for (j = 0; j < context_count; j++) {
                H264SliceContext *sl2 = &h->slice_ctx[j];
                int        slice_idx2 = sl2->mb_y * h->mb_width + sl2->mb_x;

                if (i == j || slice_idx2 < slice_idx)
                    continue;
                next_slice_idx = FFMIN(next_slice_idx, slice_idx2);
            }
            sl->next_slice_idx = next_slice_idx;
        }

        avctx->execute(avctx, decode_slice, h->slice_ctx,
                       NULL, context_count, sizeof(h->slice_ctx[0]));

        /* pull back stuff from slices to master context */
        sl                   = &h->slice_ctx[context_count - 1];
        h->mb_y              = sl->mb_y;

        if (h->postpone_filter) {
            h->postpone_filter = 0;

            for (i = 0; i < context_count; i++) {
                int y_end, x_end;

                sl = &h->slice_ctx[i];
                y_end = FFMIN(sl->mb_y + 1, h->mb_height);
                x_end = (sl->mb_y >= h->mb_height) ? h->mb_width : sl->mb_x;

                for (j = sl->resync_mb_y; j < y_end; j += 1 + FIELD_OR_MBAFF_PICTURE(h)) {
                    sl->mb_y = j;
                    loop_filter(h, sl, j > sl->resync_mb_y ? 0 : sl->resync_mb_x,
                                j == y_end - 1 ? x_end : h->mb_width);
                }
            }
        }
    }

finish:
    h->nb_slice_ctx_queued = 0;
    return ret;
}
