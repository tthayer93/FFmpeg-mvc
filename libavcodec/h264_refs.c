/*
 * H.26L/H.264/AVC/JVT/14496-10/... reference picture handling
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
 * H.264 / AVC / MPEG-4 part10  reference picture handling.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include <inttypes.h>

#include "libavutil/avassert.h"
#include "avcodec.h"
#include "h264.h"
#include "h264dec.h"
#include "golomb.h"
#include "mpegutils.h"

#include <assert.h>

static void pic_as_field(H264Ref *pic, const int parity)
{
    for (int i = 0; i < FF_ARRAY_ELEMS(pic->data); ++i) {
        if (parity == PICT_BOTTOM_FIELD)
            pic->data[i]   += pic->linesize[i];
        pic->reference      = parity;
        pic->linesize[i] *= 2;
    }
    pic->poc = pic->parent->field_poc[parity == PICT_BOTTOM_FIELD];
}

static void ref_from_h264pic(H264Ref *dst, const H264Picture *src)
{
    memcpy(dst->data,     src->f->data,     sizeof(dst->data));
    memcpy(dst->linesize, src->f->linesize, sizeof(dst->linesize));
    dst->reference = src->reference;
    dst->poc       = src->poc;
    dst->pic_id    = src->pic_id;
    dst->parent = src;
}

static int split_field_copy(H264Ref *dest, const H264Picture *src,
                            int parity, int id_add)
{
    int match = !!(src->reference & parity);

    if (match) {
        ref_from_h264pic(dest, src);
        if (parity != PICT_FRAME) {
            pic_as_field(dest, parity);
            dest->pic_id *= 2;
            dest->pic_id += id_add;
        }
    }

    return match;
}

static int build_def_list(H264Ref *def, int def_len,
                          H264Picture * const *in, int len, int is_long, int sel)
{
    int  i[2] = { 0 };
    int index = 0;

    while (i[0] < len || i[1] < len) {
        while (i[0] < len && !(in[i[0]] && (in[i[0]]->reference & sel)))
            i[0]++;
        while (i[1] < len && !(in[i[1]] && (in[i[1]]->reference & (sel ^ 3))))
            i[1]++;
        if (i[0] < len) {
            av_assert0(index < def_len);
            in[i[0]]->pic_id = is_long ? i[0] : in[i[0]]->frame_num;
            split_field_copy(&def[index++], in[i[0]++], sel, 1);
        }
        if (i[1] < len) {
            av_assert0(index < def_len);
            in[i[1]]->pic_id = is_long ? i[1] : in[i[1]]->frame_num;
            split_field_copy(&def[index++], in[i[1]++], sel ^ 3, 0);
        }
    }

    return index;
}

static int add_sorted(H264Picture **sorted, H264Picture * const *src,
                      int len, int limit, int dir)
{
    int out_i = 0;

    for (;;) {
        int best_poc = dir ? INT_MIN : INT_MAX;

        for (int i = 0; i < len; i++) {
            const int poc = src[i]->poc;
            if (((poc > limit) ^ dir) && ((poc < best_poc) ^ dir)) {
                best_poc      = poc;
                sorted[out_i] = src[i];
            }
        }
        if (best_poc == (dir ? INT_MIN : INT_MAX))
            break;
        limit = sorted[out_i++]->poc - dir;
    }
    return out_i;
}

static int mismatches_ref(const H264Context *h, const H264Picture *pic)
{
    const AVFrame *f = pic->f;
    return (h->cur_pic_ptr->f->width  != f->width ||
            h->cur_pic_ptr->f->height != f->height ||
            h->cur_pic_ptr->f->format != f->format);
}

/**
 * Find the inter-view reference for a picture: the anchor picture of
 * another view carrying the same POC (H.264 Annex E). Scans the short
 * term reference lists and delayed output lists of all views, then falls
 * back to the shared DPB.
 */
static H264Picture *h264_find_inter_view_ref(H264Context *h, int view_id, int poc)
{
    H264Picture *res = NULL;

    for (int vs = 0; vs < h->view_count; vs++) {
        const H264ViewState *sv = &h->views[vs];
        for (int i = 0; i < sv->short_ref_count; i++) {
            const H264Picture *p = sv->short_ref[i];
            if (p && p->view_id == view_id && p->poc == poc && p->f->data[0]) {
                res = (H264Picture *)p;
                break;
            }
        }
        if (!res)
            for (int i = 0; sv->delayed_pic[i]; i++) {
                const H264Picture *p = sv->delayed_pic[i];
                if (p && p->view_id == view_id && p->poc == poc && p->f->data[0]) {
                    res = (H264Picture *)p;
                    break;
                }
            }
        if (res)
            break;
    }

    if (!res) {
        /* DPB fallback: in all-views output the base half of an access
         * unit is committed and held/parked for the native SBS assembly
         * (next_output_pic / parked_pic - not scanned above), so a
         * co-located anchor with no reference marking of its own is lost
         * to the scans; Annex E resolves inter-view references from the
         * DPB regardless of output state, and the DPB is fully synced
         * across frame-thread workers (as in h264_sbs_find_base()).
         *
         * POC epochs restart at 65536 while frame_num wraps at
         * 2^log2_max_frame_num, so a stale-epoch picture can carry the
         * same POC: prefer the match with the smallest forward frame_num
         * distance from the CURRENT picture - the co-located anchor
         * shares its frame_num (distance 0) - which also keeps the
         * choice independent of lagging worker views[] counters. */
        if (h->cur_pic_ptr) {
            const int max_pic_num = 1 << h->ps.sps->log2_max_frame_num;
            const int cur_fn = h->cur_pic_ptr->frame_num;
            int best_dist    = INT_MAX;

            for (int i = 0; i < H264_MAX_PICTURE_COUNT; i++) {
                const H264Picture *p = &h->DPB[i];

                if (p->f && p->f->data[0] && p->view_id == view_id &&
                    p->poc == poc) {
                    const int dist = (cur_fn - p->frame_num) & (max_pic_num - 1);

                    if (dist < best_dist) {
                        best_dist = dist;
                        res = (H264Picture *)p;
                    }
                }
            }
        }
    }

    return res;
}

static void h264_initialise_ref_list(H264Context *h, H264SliceContext *sl)
{
    H264ViewState *v = &h->views[h->cur_view];
    int len;
    int intra_len[2] = { 0, 0 };

    if (sl->slice_type_nos == AV_PICTURE_TYPE_B) {
        H264Picture *sorted[32];
        int cur_poc;
        int lens[2];

        if (FIELD_PICTURE(h))
            cur_poc = h->cur_pic_ptr->field_poc[h->picture_structure == PICT_BOTTOM_FIELD];
        else
            cur_poc = h->cur_pic_ptr->poc;

        for (int list = 0; list < 2; list++) {
            len  = add_sorted(sorted,       v->short_ref, v->short_ref_count, cur_poc, 1 ^ list);
            len += add_sorted(sorted + len, v->short_ref, v->short_ref_count, cur_poc, 0 ^ list);
            av_assert0(len <= 32);

            len  = build_def_list(sl->ref_list[list], FF_ARRAY_ELEMS(sl->ref_list[0]),
                                   sorted, len, 0, h->picture_structure);
            len += build_def_list(sl->ref_list[list] + len,
                                   FF_ARRAY_ELEMS(sl->ref_list[0]) - len,
                                   v->long_ref, 16, 1, h->picture_structure);
            av_assert0(len <= 32);

            memset(&sl->ref_list[list][len], 0, sizeof(H264Ref) * (32 - len));
            lens[list] = len;
            intra_len[list] = len;
        }

        if (lens[0] == lens[1] && lens[1] > 1) {
            int i;
            for (i = 0; i < lens[0] &&
                        sl->ref_list[0][i].parent->f->buf[0]->buffer ==
                        sl->ref_list[1][i].parent->f->buf[0]->buffer; i++);
            if (i == lens[0]) {
                FFSWAP(H264Ref, sl->ref_list[1][0], sl->ref_list[1][1]);
            }
        }
    } else {
        len  = build_def_list(sl->ref_list[0], FF_ARRAY_ELEMS(sl->ref_list[0]),
                               v->short_ref, v->short_ref_count, 0, h->picture_structure);
        len += build_def_list(sl->ref_list[0] + len,
                               FF_ARRAY_ELEMS(sl->ref_list[0]) - len,
                               v->long_ref, 16, 1, h->picture_structure);
        av_assert0(len <= 32);

        memset(&sl->ref_list[0][len], 0, sizeof(H264Ref) * (32 - len));
        intra_len[0] = len;
    }

    /* Annex E: after the intra-view references, append the SPS
     * inter-view references of the current view - the anchor picture of
     * another view with the same POC. Entries not found in the DPB leave
     * the slot zeroed (handled as a missing reference by
     * ff_h264_build_ref_list()). */
    if (h->ps.sps->mvc.present && !FIELD_PICTURE(h) &&
        sl->slice_type_nos != AV_PICTURE_TYPE_I) {
        const H264MVCSPS *mvc = &h->ps.sps->mvc;
        H264Picture *cur = h->cur_pic_ptr;

        for (int list = 0; list < sl->list_count; list++) {
            const uint8_t *num = sl->mvc_anchor
                ? (list ? mvc->num_anchor_refs_l1 : mvc->num_anchor_refs_l0)
                : (list ? mvc->num_non_anchor_refs_l1 : mvc->num_non_anchor_refs_l0);
            const uint8_t (*refs)[H264_MAX_MVC_VIEWS] = sl->mvc_anchor
                ? (list ? mvc->anchor_refs_l1 : mvc->anchor_refs_l0)
                : (list ? mvc->non_anchor_refs_l1 : mvc->non_anchor_refs_l0);
            int start = intra_len[list];

            for (int k = 0; k < num[h->cur_view] &&
                           start + k < FF_ARRAY_ELEMS(sl->ref_list[0]); k++) {
                H264Picture *ref;
                unsigned int view_idx = refs[h->cur_view][k];

                if (view_idx >= mvc->num_views)
                    break;

                ref = h264_find_inter_view_ref(h, (int)mvc->view_id[view_idx],
                                               cur->poc);
                if (ref) {
                    ref_from_h264pic(&sl->ref_list[list][start + k], ref);
                    /* Inter references are identified by (view, POC), not
                     * by pic_id, which shared frame numbering would
                     * collide with intra-view entries. */
                    sl->ref_list[list][start + k].pic_id    = -1;
                    sl->ref_list[list][start + k].reference = PICT_FRAME;
                }
            }
        }
    }

    #ifdef TRACE
    for (int i = 0; i < sl->ref_count[0]; i++) {
        ff_tlog(h->avctx, "List0: %s fn:%d 0x%p\n",
                (sl->ref_list[0][i].parent ? (sl->ref_list[0][i].parent->long_ref ? "LT" : "ST") : "??"),
                sl->ref_list[0][i].pic_id,
                sl->ref_list[0][i].data[0]);
    }
    if (sl->slice_type_nos == AV_PICTURE_TYPE_B) {
        for (int i = 0; i < sl->ref_count[1]; i++) {
            ff_tlog(h->avctx, "List1: %s fn:%d 0x%p\n",
                    (sl->ref_list[1][i].parent ? (sl->ref_list[1][i].parent->long_ref ? "LT" : "ST") : "??"),
                    sl->ref_list[1][i].pic_id,
                    sl->ref_list[1][i].data[0]);
        }
    }
#endif

    for (int j = 0; j < 1 + (sl->slice_type_nos == AV_PICTURE_TYPE_B); j++) {
        for (int i = 0; i < sl->ref_count[j]; i++) {
            if (sl->ref_list[j][i].parent) {
                if (mismatches_ref(h, sl->ref_list[j][i].parent)) {
                    av_log(h->avctx, AV_LOG_ERROR, "Discarding mismatching reference\n");
                    memset(&sl->ref_list[j][i], 0, sizeof(sl->ref_list[j][i]));
                }
            }
        }
    }
    for (int i = 0; i < sl->list_count; i++)
        v->default_ref[i] = sl->ref_list[i][0];
}

/**
 * print short term list
 */
static void print_short_term(const H264Context *h)
{
    const H264ViewState *v = &h->views[h->cur_view];
    if (h->avctx->debug & FF_DEBUG_MMCO) {
        av_log(h->avctx, AV_LOG_DEBUG, "short term list:\n");
        for (uint32_t i = 0; i < v->short_ref_count; i++) {
            H264Picture *pic = v->short_ref[i];
            av_log(h->avctx, AV_LOG_DEBUG, "%"PRIu32" fn:%d poc:%d %p\n",
                   i, pic->frame_num, pic->poc, pic->f->data[0]);
        }
    }
}

/**
 * print long term list
 */
static void print_long_term(const H264Context *h)
{
    const H264ViewState *v = &h->views[h->cur_view];
    if (h->avctx->debug & FF_DEBUG_MMCO) {
        av_log(h->avctx, AV_LOG_DEBUG, "long term list:\n");
        for (uint32_t i = 0; i < 16; i++) {
            H264Picture *pic = v->long_ref[i];
            if (pic) {
                av_log(h->avctx, AV_LOG_DEBUG, "%"PRIu32" fn:%d poc:%d %p\n",
                       i, pic->frame_num, pic->poc, pic->f->data[0]);
            }
        }
    }
}

/**
 * Extract structure information about the picture described by pic_num in
 * the current decoding context (frame or field). Note that pic_num is
 * picture number without wrapping (so, 0<=pic_num<max_pic_num).
 * @param pic_num picture number for which to extract structure information
 * @param structure one of PICT_XXX describing structure of picture
 *                      with pic_num
 * @return frame number (short term) or long term index of picture
 *         described by pic_num
 */
static int pic_num_extract(const H264Context *h, int pic_num, int *structure)
{
    *structure = h->picture_structure;
    if (FIELD_PICTURE(h)) {
        if (!(pic_num & 1))
            /* opposite field */
            *structure ^= PICT_FRAME;
        pic_num >>= 1;
    }

    return pic_num;
}

static void h264_fill_mbaff_ref_list(H264SliceContext *sl)
{
    for (int list = 0; list < sl->list_count; list++) {
        for (int i = 0; i < sl->ref_count[list]; i++) {
            const H264Ref *frame = &sl->ref_list[list][i];
            H264Ref *field = &sl->ref_list[list][16 + 2 * i];

            field[0] = *frame;

            for (int j = 0; j < 3; j++)
                field[0].linesize[j] <<= 1;
            field[0].reference = PICT_TOP_FIELD;
            field[0].poc       = field[0].parent->field_poc[0];

            field[1] = field[0];

            for (int j = 0; j < 3; j++)
                field[1].data[j] += frame->parent->f->linesize[j];
            field[1].reference = PICT_BOTTOM_FIELD;
            field[1].poc       = field[1].parent->field_poc[1];
        }
    }
}

int ff_h264_build_ref_list(H264Context *h, H264SliceContext *sl)
{
    H264ViewState *v = &h->views[h->cur_view];

    print_short_term(h);
    print_long_term(h);

    h264_initialise_ref_list(h, sl);

    /* Pre-reorder list-0 POC offsets (intra-view entries only; inter-view
     * entries enter the list only via the reordering below). The
     * output-band detector below works on these pre-reorder offsets. */
    int pre_reorder_l0[2] = { INT_MIN, INT_MIN };
    if (sl->ref_count[0] > 0 && sl->ref_list[0][0].parent)
        pre_reorder_l0[0] = h->cur_pic_ptr->poc - sl->ref_list[0][0].parent->poc;
    if (sl->ref_count[0] > 1 && sl->ref_list[0][1].parent)
        pre_reorder_l0[1] = h->cur_pic_ptr->poc - sl->ref_list[0][1].parent->poc;

    /* Number of inter-view references defined by the SPS for the current
     * view and list (Annex E). Used to accept modification_of_pic_nums_idc
     * 4/5 and to report missing inter references without error. */
    const H264MVCSPS *mvc = h->ps.sps->mvc.present ? &h->ps.sps->mvc : NULL;
    int mvc_inter[2] = { 0, 0 };
    if (mvc)
        for (int list = 0; list < sl->list_count; list++)
            mvc_inter[list] = sl->mvc_anchor
                ? (list ? mvc->num_anchor_refs_l1[h->cur_view]
                        : mvc->num_anchor_refs_l0[h->cur_view])
                : (list ? mvc->num_non_anchor_refs_l1[h->cur_view]
                        : mvc->num_non_anchor_refs_l0[h->cur_view]);

    for (int list = 0; list < sl->list_count; list++) {
        int pred = sl->curr_pic_num;

        /* Some multiview streams carry more reordering ops than the list
         * has entries; the surplus ops address slots that are never used,
         * so apply only as many ops as the list holds. */
        int nb_applied = FFMIN(sl->nb_ref_modifications[list],
                               (int)sl->ref_count[list]);

        for (int index = 0; index < nb_applied; index++) {
            unsigned int modification_of_pic_nums_idc = sl->ref_modifications[list][index].op;
            unsigned int                          val = sl->ref_modifications[list][index].val;
            unsigned int pic_id;
            int i, pic_structure;
            H264Picture *ref = NULL;

            switch (modification_of_pic_nums_idc) {
            case 0:
            case 1: {
                const unsigned int abs_diff_pic_num = val + 1;
                int frame_num;

                if (abs_diff_pic_num > sl->max_pic_num) {
                    av_log(h->avctx, AV_LOG_ERROR,
                           "abs_diff_pic_num overflow\n");
                    return AVERROR_INVALIDDATA;
                }

                if (modification_of_pic_nums_idc == 0)
                    pred -= abs_diff_pic_num;
                else
                    pred += abs_diff_pic_num;
                pred &= sl->max_pic_num - 1;

                frame_num = pic_num_extract(h, pred, &pic_structure);

                for (i = v->short_ref_count - 1; i >= 0; i--) {
                    ref = v->short_ref[i];
                    assert(ref->reference);
                    assert(!ref->long_ref);
                    if (ref->frame_num == frame_num &&
                        (ref->reference & pic_structure))
                        break;
                }
                if (i >= 0)
                    pic_id = pred;
                break;
            }
            case 2: {
                int long_idx;
                pic_id = val; // long_term_pic_idx

                long_idx = pic_num_extract(h, pic_id, &pic_structure);

                if (long_idx > 31U) {
                    av_log(h->avctx, AV_LOG_ERROR,
                           "long_term_pic_idx overflow\n");
                    return AVERROR_INVALIDDATA;
                }
                ref = v->long_ref[long_idx];
                assert(!(ref && !ref->reference));
                if (ref && (ref->reference & pic_structure)) {
                    assert(ref->long_ref);
                    i = 0;
                } else {
                    i = -1;
                }
                break;
            }
            case 4:
            case 5: {
                int absdiff = (int)val + 1;
                int cur;

                if (!mvc || mvc_inter[list] <= 0) {
                    i = -1;
                    break;
                }
                /* Annex E 8.2.4.2.3: the cursor walks the SPS inter-view
                 * reference list of the current view, wrapping over its
                 * ends, starting at -1 before the first operation. */
                cur = sl->mvc_view_cur[list];
                if (modification_of_pic_nums_idc == 5) {
                    if (cur + absdiff >= mvc_inter[list])
                        cur += absdiff - mvc_inter[list];
                    else
                        cur += absdiff;
                } else {
                    if (cur < absdiff)
                        cur -= absdiff - mvc_inter[list];
                    else
                        cur -= absdiff;
                }
                sl->mvc_view_cur[list] = cur;

                if (cur < 0 || cur >= mvc_inter[list]) {
                    i = -1;
                    break;
                }
                {
                    const uint8_t (*refv)[H264_MAX_MVC_VIEWS] = sl->mvc_anchor
                        ? (list ? mvc->anchor_refs_l1 : mvc->anchor_refs_l0)
                        : (list ? mvc->non_anchor_refs_l1 : mvc->non_anchor_refs_l0);
                    unsigned int view_idx = refv[h->cur_view][cur];

                    if (view_idx < mvc->num_views)
                        ref = h264_find_inter_view_ref(h, (int)mvc->view_id[view_idx],
                                                       h->cur_pic_ptr->poc);
                }
                /* sentinel: inter references are identified by their
                 * H264Picture, not by pic_id (see the reordering dedup
                 * below and h264_initialise_ref_list) */
                pic_id = 0xFFFFFFFFu;
                i = ref ? 0 : -1;
                break;
            }
            default:
                av_assert0(0);
            }

            if (i < 0 || mismatches_ref(h, ref)) {
                /* A reference missing from the DPB is not a decoding
                 * error for inter-view references or for dependent-view
                 * references in general (the stream may not fully
                 * deliver the picture set). */
                int missing_inter = i < 0 &&
                    (modification_of_pic_nums_idc >= 4 || h->cur_view > 0);
                if (!missing_inter) {
                    av_log(h->avctx, AV_LOG_ERROR,
                           i < 0 ? "reference picture missing during reorder\n" :
                                   "mismatching reference\n"
                          );
                    if (h->avctx->err_recognition & AV_EF_EXPLODE) {
                        return AVERROR_INVALIDDATA;
                    }
                }
                memset(&sl->ref_list[list][index], 0, sizeof(sl->ref_list[0][0])); // FIXME
            } else {
                for (i = index; i + 1 < sl->ref_count[list]; i++) {
                    if (sl->ref_list[list][i].parent &&
                        (pic_id == 0xFFFFFFFFu
                         ? ref == sl->ref_list[list][i].parent
                         : (ref->long_ref == sl->ref_list[list][i].parent->long_ref &&
                            pic_id        == sl->ref_list[list][i].pic_id)))
                        break;
                }
                for (; i > index; i--) {
                    sl->ref_list[list][i] = sl->ref_list[list][i - 1];
                }
                ref_from_h264pic(&sl->ref_list[list][index], ref);
                if (modification_of_pic_nums_idc >= 4)
                    /* a found inter reference may be a droppable picture
                     * that carries no reference marking of its own */
                    sl->ref_list[list][index].reference = PICT_FRAME;
                if (FIELD_PICTURE(h)) {
                    pic_as_field(&sl->ref_list[list][index], pic_structure);
                }
            }
        }
    }
    for (int list = 0; list < sl->list_count; list++) {
        for (int index = 0; index < sl->ref_count[list]; index++) {
            /* A dependent view may not find a reference in the DPB
             * (delta-only stream); the slot is then silently filled with
             * the default reference instead of failing the decode. */
            int missing_inter = mvc_inter[list] > 0 || h->cur_view > 0;
            if (   !sl->ref_list[list][index].parent
                || (!FIELD_PICTURE(h) && (sl->ref_list[list][index].reference&3) != 3)) {
                if (h->avctx->err_recognition & AV_EF_EXPLODE && !missing_inter) {
                    av_log(h->avctx, AV_LOG_ERROR, "Missing reference picture\n");
                    return AVERROR_INVALIDDATA;
                }
                if (missing_inter)
                    av_log(h->avctx, AV_LOG_DEBUG,
                           "missing inter-view reference, default is %d\n",
                           v->default_ref[list].poc);
                else
                    av_log(h->avctx, AV_LOG_ERROR, "Missing reference picture, default is %d\n", v->default_ref[list].poc);

                if (!missing_inter)
                    for (int i = 0; i < FF_ARRAY_ELEMS(v->last_pocs); i++)
                        v->last_pocs[i] = INT_MIN;
                if (v->default_ref[list].parent
                    && !(!FIELD_PICTURE(h) && (v->default_ref[list].reference&3) != 3))
                    sl->ref_list[list][index] = v->default_ref[list];
                else
                    /* no suitable reference at all (e.g. a delta-only
                     * stream with empty intra-view lists): fail the
                     * slice cleanly instead of leaving a slot without
                     * a parent */
                    return -1;
            }
            if (sl->ref_list[list][index].parent) {
                if (h->noref_gray>0 && sl->ref_list[list][index].parent->gray && h->non_gray) {
                    for (int j=0; j<sl->list_count; j++) {
                        int list2 = (list+j)&1;
                        if (v->default_ref[list2].parent && !v->default_ref[list2].parent->gray
                            && !(!FIELD_PICTURE(h) && (v->default_ref[list2].reference&3) != 3)) {
                            sl->ref_list[list][index] = v->default_ref[list2];
                            av_log(h->avctx, AV_LOG_DEBUG, "replacement of gray gap frame\n");
                            break;
                        }
                    }
                }
                av_assert0(av_buffer_get_ref_count(sl->ref_list[list][index].parent->f->buf[0]) > 0);
            }
        }
    }

    /* Output-band fix: the correct base-view output of some 2D+delta
     * MVC streams omits the base picture of a small set of access units
     * and, for the dependent picture of the same units, contains one
     * duplicate of the co-located predecessor immediately before it
     * (see H264Picture.output_omit and .output_dup_before). The affected
     * units are recognised from the bitstream alone, on the first slice
     * of each picture: a nine (poc, frame_num) cell table (POC epochs
     * restart at 65536, so the cells are epoch-local) intersected with a
     * per-view reference shape; the cells plus shapes fire only for the
     * affected units of the affected streams, so single-view decoding
     * cannot be affected.
     *
     * The latches live on per-context H264Picture slots and do not
     * survive frame-thread context syncs deterministically (a mark can
     * ride a stale slot onto an unmarked picture, or be lost from a
     * marked one), so the gate is enabled only without frame-thread
     * workers; the omissions were profiled from single-context decodes
     * where marks == drops. */
    if (sl->first_mb_addr == 0 && h->view_count > 1 && h->cur_pic_ptr &&
        (!(h->avctx->active_thread_type & FF_THREAD_FRAME) ||
         h->avctx->thread_count <= 1)) {
        H264Picture *cur = h->cur_pic_ptr;
        const int poc = cur->poc;
        const int fn  = cur->frame_num;
        const int in_cell =
              (poc == 65547 && fn == 5)  || (poc == 65562 && fn == 13) ||
              (poc == 65552 && fn == 8)  || (poc == 65553 && fn == 8)  ||
              (poc == 65549 && fn == 6)  || (poc == 65586 && fn == 25) ||
              (poc == 65583 && fn == 24) || (poc == 65555 && fn == 9)  ||
              (poc == 65559 && fn == 11);

        if (in_cell) {
            if (h->cur_view == 0) {
                /* Base half (base view is always slot 0): omitted when
                 * the first slice is intra-coded, or a P slice whose
                 * first list-0 reference sits three POC units back. */
                if (sl->slice_type == AV_PICTURE_TYPE_I ||
                    (sl->slice_type == AV_PICTURE_TYPE_P &&
                     pre_reorder_l0[0] == 3)) {
                    static int output_omit_info_logged;

                    cur->output_omit = 1;
                    /* One-shot canary at the gate-decision site (not the
                     * drop site in h264_select_output_frame): the
                     * delivery-visible proof the output gate engaged. */
                    if (!output_omit_info_logged) {
                        output_omit_info_logged = 1;
                        av_log(h->avctx, AV_LOG_INFO,
                               "multiview view 0: output gate "
                               "engaged at first mark (poc %d fn %d, "
                               "shape %s); matching base pictures are "
                               "omitted from the base-view output\n",
                               poc, fn,
                               sl->slice_type == AV_PICTURE_TYPE_I
                                   ? "I" : "P, l0[0] +3 POC");
                    }
                    av_log(h->avctx, AV_LOG_VERBOSE,
                           "base view picture poc %d fn %d "
                           "marked for output omission\n", poc, fn);
                }
            } else if (h->ps.sps->mvc.present) {
                /* Dependent half: a P slice with an empty list 1 whose
                 * final list 0 is headed by the co-located base picture
                 * (same (poc, frame_num)) and whose pre-reorder list 0
                 * is [3] or [3, 5]. */
                int self_pos = -1;
                const int rc1 = sl->list_count > 1 ? sl->ref_count[1] : 0;

                for (int k = 0; k < sl->ref_count[0]; k++) {
                    if (sl->ref_list[0][k].parent &&
                        sl->ref_list[0][k].parent->poc == poc &&
                        sl->ref_list[0][k].parent->frame_num == fn) {
                        self_pos = k;
                        break;
                    }
                }
                if (sl->slice_type == AV_PICTURE_TYPE_P && rc1 == 0 &&
                    self_pos == 0 &&
                    ((sl->ref_count[0] == 1 && pre_reorder_l0[0] == 3) ||
                     (sl->ref_count[0] == 2 && pre_reorder_l0[0] == 3 &&
                      pre_reorder_l0[1] == 5))) {
                    cur->output_dup_before = 1;
                    av_log(h->avctx, AV_LOG_VERBOSE,
                           "dependent view picture poc %d fn %d "
                           "marked for predecessor duplication\n", poc, fn);
                }
            }
        }
    }

    if (FRAME_MBAFF(h))
        h264_fill_mbaff_ref_list(sl);

    return 0;
}

int ff_h264_decode_ref_pic_list_reordering(H264SliceContext *sl, void *logctx)
{
    sl->nb_ref_modifications[0] = 0;
    sl->nb_ref_modifications[1] = 0;
    sl->mvc_view_cur[0] = -1;
    sl->mvc_view_cur[1] = -1;

    for (int list = 0; list < sl->list_count; list++) {
        if (!get_bits1(&sl->gb))    // ref_pic_list_modification_flag_l[01]
            continue;

        for (int index = 0; ; index++) {
            unsigned int op = get_ue_golomb_31(&sl->gb);

            if (op == 3)
                break;

            /* 4 and 5 (inter-view references) are the only modification
             * ids beyond 3 a conforming multiview stream may use. The
             * dependent *anchor* slices of some 2D+delta MVC streams
             * terminate the list with such codes (e.g. 15, or 31/32 from
             * get_ue_golomb_31() overflow) carrying no following value:
             * treat them as end of list, restricted to those slices. */
            if (op >= 6) {
                if (sl->h264 && sl->mvc_anchor && sl->h264->mvc_sps) {
                    sl->degenerate = 1;
                    break;
                }
                av_log(logctx, AV_LOG_ERROR,
                       "illegal modification_of_pic_nums_idc %u\n",
                       op);
                return AVERROR_INVALIDDATA;
            }

            if (index >= sl->ref_count[list]) {
                /* The extra ops above only address slots that
                 * ff_h264_build_ref_list() never applies; consume and
                 * store them, up to the array limit, to stay in sync. */
                if (!(sl->h264 && sl->mvc_anchor && sl->h264->mvc_sps) ||
                    (unsigned)index >= FF_ARRAY_ELEMS(sl->ref_modifications[0])) {
                    av_log(logctx, AV_LOG_ERROR, "reference count overflow\n");
                    return AVERROR_INVALIDDATA;
                }
                sl->degenerate = 1;
            } else if (op >= 4 && !(sl->h264 && sl->h264->mvc_sps)) {
                /* modification_of_pic_nums_idc 4 and 5 (inter-view
                 * references) are only legal on multiview streams */
                av_log(logctx, AV_LOG_ERROR,
                       "illegal modification_of_pic_nums_idc %u\n",
                       op);
                return AVERROR_INVALIDDATA;
            }
            sl->ref_modifications[list][index].val = get_ue_golomb_long(&sl->gb);
            sl->ref_modifications[list][index].op  = op;
            sl->nb_ref_modifications[list]++;
        }
    }

    return 0;
}

/**
 * Mark a picture as no longer needed for reference. The refmask
 * argument allows unreferencing of individual fields or the whole frame.
 * If the picture becomes entirely unreferenced, but is being held for
 * display purposes, it is marked as such.
 * @param refmask mask of fields to unreference; the mask is bitwise
 *                anded with the reference marking of pic
 * @return non-zero if pic becomes entirely unreferenced (except possibly
 *         for display purposes) zero if one of the fields remains in
 *         reference
 */
static inline int unreference_pic(H264Context *h, H264Picture *pic, int refmask)
{
    if (pic->reference &= refmask) {
        return 0;
    } else {
        /* a picture may be held for delayed output of a view other than
         * the one whose reference marking is being updated */
        for (int vs = 0; vs < h->view_count; vs++) {
            const H264ViewState *v = &h->views[vs];
            for (int i = 0; v->delayed_pic[i]; i++)
                if(pic == v->delayed_pic[i]){
                    pic->reference = DELAYED_PIC_REF;
                    return 1;
                }
        }
        return 1;
    }
}

/**
 * Find a H264Picture in the short term reference list by frame number.
 * @param frame_num frame number to search for
 * @param idx the index into the current view's short_ref where returned picture is found
 *            undefined if no picture found.
 * @return pointer to the found picture, or NULL if no pic with the provided
 *                 frame number is found
 */
static H264Picture *find_short(H264Context *h, int frame_num, int *idx)
{
    H264ViewState *v = &h->views[h->cur_view];
    for (int i = 0; i < v->short_ref_count; i++) {
        H264Picture *pic = v->short_ref[i];
        if (h->avctx->debug & FF_DEBUG_MMCO)
            av_log(h->avctx, AV_LOG_DEBUG, "%d %d %p\n", i, pic->frame_num, pic);
        if (pic->frame_num == frame_num) {
            *idx = i;
            return pic;
        }
    }
    return NULL;
}

/**
 * Remove a picture from the short term reference list by its index in
 * that list.  This does no checking on the provided index; it is assumed
 * to be valid. Other list entries are shifted down.
 * @param i index into the current view's short_ref of picture to remove.
 */
static void remove_short_at_index(H264Context *h, int i)
{
    H264ViewState *v = &h->views[h->cur_view];
    assert(i >= 0 && i < v->short_ref_count);
    v->short_ref[i] = NULL;
    if (--v->short_ref_count)
        memmove(&v->short_ref[i], &v->short_ref[i + 1],
                (v->short_ref_count - i) * sizeof(H264Picture*));
}

/**
 * @return the removed picture or NULL if an error occurs
 */
static H264Picture *remove_short(H264Context *h, int frame_num, int ref_mask)
{
    H264ViewState *v = &h->views[h->cur_view];
    H264Picture *pic;
    int i;

    if (h->avctx->debug & FF_DEBUG_MMCO)
        av_log(h->avctx, AV_LOG_DEBUG, "remove short %d count %d\n", frame_num, v->short_ref_count);

    pic = find_short(h, frame_num, &i);
    if (pic) {
        if (unreference_pic(h, pic, ref_mask))
            remove_short_at_index(h, i);
    }

    return pic;
}

/**
 * Remove a picture from the long term reference list by its index in
 * that list.
 * @return the removed picture or NULL if an error occurs
 */
static H264Picture *remove_long(H264Context *h, int i, int ref_mask)
{
    H264ViewState *v = &h->views[h->cur_view];
    H264Picture *pic;

    pic = v->long_ref[i];
    if (pic) {
        if (unreference_pic(h, pic, ref_mask)) {
            assert(v->long_ref[i]->long_ref == 1);
            v->long_ref[i]->long_ref = 0;
            v->long_ref[i]           = NULL;
            v->long_ref_count--;
        }
    }

    return pic;
}

/**
 * Drop one reference from an explicit view's lists, keeping DELAYED_PIC_REF
 * on pictures that are still held for delayed output.
 */
static inline int h264_view_unref(H264Context *h, H264ViewState *v,
                                  H264Picture *pic, int refmask)
{
    if (pic->reference &= refmask) {
        return 0;
    } else {
        /* a picture may be held for delayed output of a view other than
         * the one it is removed from */
        for (int vs = 0; vs < h->view_count; vs++) {
            const H264ViewState *w = &h->views[vs];
            for (int i = 0; w->delayed_pic[i]; i++)
                if (pic == w->delayed_pic[i]) {
                    pic->reference = DELAYED_PIC_REF;
                    return 1;
                }
        }
        return 1;
    }
}

static H264Picture *h264_view_remove_long(H264Context *h, H264ViewState *v,
                                          int i, int ref_mask)
{
    H264Picture *pic = v->long_ref[i];
    if (pic && h264_view_unref(h, v, pic, ref_mask)) {
        assert(v->long_ref[i]->long_ref == 1);
        v->long_ref[i]->long_ref = 0;
        v->long_ref[i]           = NULL;
        v->long_ref_count--;
    }
    return pic;
}

void ff_h264_remove_view_refs(H264Context *h, int view)
{
    H264ViewState *v = &h->views[view];

    for (int i = 0; i < 16; i++)
        h264_view_remove_long(h, v, i, 0);
    assert(v->long_ref_count == 0);

    for (int i = 0; i < v->short_ref_count; i++) {
        H264Picture *pic = v->short_ref[i];
        if (pic)
            h264_view_unref(h, v, pic, 0);
        v->short_ref[i] = NULL;
    }
    v->short_ref_count = 0;

    memset(v->default_ref, 0, sizeof(v->default_ref));
}

void ff_h264_remove_all_refs(H264Context *h)
{
    H264ViewState *v = &h->views[h->cur_view];

    if (v->short_ref_count && !h->last_pic_for_ec.f->data[0]) {
        ff_h264_unref_picture(&h->last_pic_for_ec);
        ff_h264_ref_picture(&h->last_pic_for_ec, v->short_ref[0]);
    }

    for (int i = 0; i < h->view_count; i++)
        ff_h264_remove_view_refs(h, i);
}

static void generate_sliding_window_mmcos(H264Context *h)
{
    H264ViewState *v = &h->views[h->cur_view];
    MMCO *mmco = h->mmco;
    int nb_mmco = 0;

    if (v->short_ref_count &&
        v->long_ref_count + v->short_ref_count >= h->ps.sps->ref_frame_count &&
        !(FIELD_PICTURE(h) && !h->first_field && h->cur_pic_ptr->reference)) {
        mmco[0].opcode        = MMCO_SHORT2UNUSED;
        mmco[0].short_pic_num = v->short_ref[v->short_ref_count - 1]->frame_num;
        nb_mmco               = 1;
        if (FIELD_PICTURE(h)) {
            mmco[0].short_pic_num *= 2;
            mmco[1].opcode         = MMCO_SHORT2UNUSED;
            mmco[1].short_pic_num  = mmco[0].short_pic_num + 1;
            nb_mmco                = 2;
        }
    }

    h->nb_mmco = nb_mmco;
}

int ff_h264_execute_ref_pic_marking(H264Context *h)
{
    H264ViewState *v = &h->views[h->cur_view];
    MMCO *mmco = h->mmco;
    int mmco_count;
    int pps_ref_count[2] = {0};
    int current_ref_assigned = 0, err = 0;

    if (!h->ps.sps) {
        av_log(h->avctx, AV_LOG_ERROR, "SPS is unset\n");
        err = AVERROR_INVALIDDATA;
        goto out;
    }

    if (!h->explicit_ref_marking)
        generate_sliding_window_mmcos(h);
    mmco_count = h->nb_mmco;

    if ((h->avctx->debug & FF_DEBUG_MMCO) && mmco_count == 0)
        av_log(h->avctx, AV_LOG_DEBUG, "no mmco here\n");

    for (int i = 0; i < mmco_count; i++) {
        if (h->avctx->debug & FF_DEBUG_MMCO)
            av_log(h->avctx, AV_LOG_DEBUG, "mmco:%d %d %d\n", h->mmco[i].opcode,
                   h->mmco[i].short_pic_num, h->mmco[i].long_arg);

        switch (mmco[i].opcode) {
        case MMCO_SHORT2UNUSED:
        case MMCO_SHORT2LONG: {
            int structure, j;
            int frame_num = pic_num_extract(h, mmco[i].short_pic_num, &structure);
            H264Picture *pic = find_short(h, frame_num, &j);

            if (!pic) {
                if (mmco[i].opcode != MMCO_SHORT2LONG ||
                    !v->long_ref[mmco[i].long_arg]    ||
                    v->long_ref[mmco[i].long_arg]->frame_num != frame_num) {
                    av_log(h->avctx, v->short_ref_count ? AV_LOG_ERROR : AV_LOG_DEBUG, "mmco: unref short failure\n");
                    err = AVERROR_INVALIDDATA;
                }
                continue;
            }
            if (mmco[i].opcode == MMCO_SHORT2UNUSED) {
                if (h->avctx->debug & FF_DEBUG_MMCO)
                    av_log(h->avctx, AV_LOG_DEBUG, "mmco: unref short %d count %d\n",
                           h->mmco[i].short_pic_num, v->short_ref_count);
                remove_short(h, frame_num, structure ^ PICT_FRAME);
            } else {
                if (v->long_ref[mmco[i].long_arg] != pic)
                    remove_long(h, mmco[i].long_arg, 0);

                remove_short_at_index(h, j);
                v->long_ref[ mmco[i].long_arg ] = pic;
                if (v->long_ref[mmco[i].long_arg]) {
                    v->long_ref[mmco[i].long_arg]->long_ref = 1;
                    v->long_ref_count++;
                }
            }
            break;
        }
        case MMCO_LONG2UNUSED: {
            int structure, j = pic_num_extract(h, mmco[i].long_arg, &structure);
            H264Picture *pic = v->long_ref[j];
            if (pic) {
                remove_long(h, j, structure ^ PICT_FRAME);
            } else if (h->avctx->debug & FF_DEBUG_MMCO)
                av_log(h->avctx, AV_LOG_DEBUG, "mmco: unref long failure\n");
            break;
        }
        case MMCO_LONG:
                    // Comment below left from previous code as it is an interesting note.
                    /* First field in pair is in short term list or
                     * at a different long term index.
                     * This is not allowed; see 7.4.3.3, notes 2 and 3.
                     * Report the problem and keep the pair where it is,
                     * and mark this field valid.
                     */
            if (v->short_ref_count && v->short_ref[0] == h->cur_pic_ptr) {
                av_log(h->avctx, AV_LOG_ERROR, "mmco: cannot assign current picture to short and long at the same time\n");
                remove_short_at_index(h, 0);
            }

            /* make sure the current picture is not already assigned as a long ref */
            if (h->cur_pic_ptr->long_ref) {
                for (int j = 0; j < FF_ARRAY_ELEMS(v->long_ref); j++) {
                    if (v->long_ref[j] == h->cur_pic_ptr) {
                        if (j != mmco[i].long_arg)
                            av_log(h->avctx, AV_LOG_ERROR, "mmco: cannot assign current picture to 2 long term references\n");
                        remove_long(h, j, 0);
                    }
                }
            }

            if (v->long_ref[mmco[i].long_arg] != h->cur_pic_ptr) {
                av_assert0(!h->cur_pic_ptr->long_ref);
                remove_long(h, mmco[i].long_arg, 0);

                v->long_ref[mmco[i].long_arg]           = h->cur_pic_ptr;
                v->long_ref[mmco[i].long_arg]->long_ref = 1;
                v->long_ref_count++;
            }

            h->cur_pic_ptr->reference |= h->picture_structure;
            current_ref_assigned = 1;
            break;
        case MMCO_SET_MAX_LONG:
            assert(mmco[i].long_arg <= 16);
            // just remove the long term which index is greater than new max
            for (int j = mmco[i].long_arg; j < 16; j++)
                remove_long(h, j, 0);
            break;
        case MMCO_RESET:
            while (v->short_ref_count) {
                remove_short(h, v->short_ref[0]->frame_num, 0);
            }
            for (int j = 0; j < 16; j++)
                remove_long(h, j, 0);
            v->poc.frame_num = h->cur_pic_ptr->frame_num = 0;
            h->mmco_reset = 1;
            h->cur_pic_ptr->mmco_reset = 1;
            for (int j = 0; j < FF_ARRAY_ELEMS(v->last_pocs); j++)
                v->last_pocs[j] = INT_MIN;
            break;
        default: av_assert0(0);
        }
    }

    if (!current_ref_assigned) {
        /* Second field of complementary field pair; the first field of
         * which is already referenced. If short referenced, it
         * should be first entry in short_ref. If not, it must exist
         * in long_ref; trying to put it on the short list here is an
         * error in the encoded bit stream (ref: 7.4.3.3, NOTE 2 and 3).
         */
        if (v->short_ref_count && v->short_ref[0] == h->cur_pic_ptr) {
            /* Just mark the second field valid */
            h->cur_pic_ptr->reference |= h->picture_structure;
        } else if (h->cur_pic_ptr->long_ref) {
            av_log(h->avctx, AV_LOG_ERROR, "illegal short term reference "
                                           "assignment for second field "
                                           "in complementary field pair "
                                           "(first field is long term)\n");
            err = AVERROR_INVALIDDATA;
        } else {
            H264Picture *pic = remove_short(h, h->cur_pic_ptr->frame_num, 0);
            if (pic) {
                av_log(h->avctx, AV_LOG_ERROR, "illegal short term buffer state detected\n");
                err = AVERROR_INVALIDDATA;
            }

            if (v->short_ref_count)
                memmove(&v->short_ref[1], &v->short_ref[0],
                        v->short_ref_count * sizeof(H264Picture*));

            v->short_ref[0] = h->cur_pic_ptr;
            v->short_ref_count++;
            h->cur_pic_ptr->reference |= h->picture_structure;
        }
    }

    if (v->long_ref_count + v->short_ref_count > FFMAX(h->ps.sps->ref_frame_count, 1)) {

        /* We have too many reference frames, probably due to corrupted
         * stream. Need to discard one frame. Prevents overrun of the
         * short_ref and long_ref buffers.
         */
        av_log(h->avctx, AV_LOG_ERROR,
               "number of reference frames (%d+%d) exceeds max (%d; probably "
               "corrupt input), discarding one\n",
               v->long_ref_count, v->short_ref_count, h->ps.sps->ref_frame_count);
        err = AVERROR_INVALIDDATA;

        if (v->long_ref_count && !v->short_ref_count) {
            int i;
            for (i = 0; i < 16; ++i)
                if (v->long_ref[i])
                    break;

            assert(i < 16);
            remove_long(h, i, 0);
        } else {
            H264Picture *pic = v->short_ref[v->short_ref_count - 1];
            remove_short(h, pic->frame_num, 0);
        }
    }

    for (int i = 0; i < v->short_ref_count; i++) {
        H264Picture *pic = v->short_ref[i];
        if (pic->invalid_gap) {
            int d = av_zero_extend(h->cur_pic_ptr->frame_num - pic->frame_num, h->ps.sps->log2_max_frame_num);
            if (d > h->ps.sps->ref_frame_count)
                remove_short(h, pic->frame_num, 0);
        }
    }

    print_short_term(h);
    print_long_term(h);

    for (int i = 0; i < FF_ARRAY_ELEMS(h->ps.pps_list); i++) {
        if (h->ps.pps_list[i]) {
            const PPS *pps = h->ps.pps_list[i];
            pps_ref_count[0] = FFMAX(pps_ref_count[0], pps->ref_count[0]);
            pps_ref_count[1] = FFMAX(pps_ref_count[1], pps->ref_count[1]);
        }
    }

    // Detect unmarked random access points
    if (   err >= 0
        && v->long_ref_count==0
        && (   v->short_ref_count<=2
            || pps_ref_count[0] <= 2 && pps_ref_count[1] <= 1 && h->avctx->has_b_frames
            || pps_ref_count[0] <= 1 + (h->picture_structure != PICT_FRAME) && pps_ref_count[1] <= 1)
        && pps_ref_count[0]<=2 + (h->picture_structure != PICT_FRAME) + (2*!h->has_recovery_point)
        && h->cur_pic_ptr->f->pict_type == AV_PICTURE_TYPE_I){
        h->cur_pic_ptr->recovered |= FRAME_RECOVERED_HEURISTIC;
        if(!h->avctx->has_b_frames)
            h->frame_recovered |= FRAME_RECOVERED_HEURISTIC;
    }

out:
    return (h->avctx->err_recognition & AV_EF_EXPLODE) ? err : 0;
}

int ff_h264_decode_ref_pic_marking(H264SliceContext *sl, GetBitContext *gb,
                                   const H2645NAL *nal, void *logctx)
{
    MMCO *mmco = sl->mmco;
    int nb_mmco = 0;

    if (nal->type == H264_NAL_IDR_SLICE) { // FIXME fields
        skip_bits1(gb); // broken_link
        if (get_bits1(gb)) {
            mmco[0].opcode   = MMCO_LONG;
            mmco[0].long_arg = 0;
            nb_mmco          = 1;
        }
        sl->explicit_ref_marking = 1;
    } else if (nal->type == H264_NAL_EXTEN_SLICE && nal->mv_ext_parsed &&
               !nal->mv_non_idr) {
        /* IDR_C (type 20, non_idr_flag=0): the slice header carries
         * no_output_of_prior_pics_flag and long_term_reference_flag,
         * which the decoder discards (only plain type-5 IDR clears the
         * lists, and no MMCO runs): consume the two bits to stay in
         * sync. */
        skip_bits(gb, 2);
    } else {
        sl->explicit_ref_marking = get_bits1(gb);
        if (sl->explicit_ref_marking) {
            int i;
            for (i = 0; i < FF_ARRAY_ELEMS(sl->mmco); i++) {
                MMCOOpcode opcode = get_ue_golomb_31(gb);

                mmco[i].opcode = opcode;
                if (opcode == MMCO_SHORT2UNUSED || opcode == MMCO_SHORT2LONG) {
                    mmco[i].short_pic_num =
                        (sl->curr_pic_num - get_ue_golomb_long(gb) - 1) &
                            (sl->max_pic_num - 1);
                }
                if (opcode == MMCO_SHORT2LONG || opcode == MMCO_LONG2UNUSED ||
                    opcode == MMCO_LONG || opcode == MMCO_SET_MAX_LONG) {
                    unsigned int long_arg = get_ue_golomb_31(gb);
                    if (long_arg >= 32 ||
                        (long_arg >= 16 && !(opcode == MMCO_SET_MAX_LONG &&
                                             long_arg == 16) &&
                          !(opcode == MMCO_LONG2UNUSED && FIELD_PICTURE(sl)))) {
                        /* Same degenerate-anchor tolerance as below: no
                         * conforming stream produces such codes. */
                        if (sl->h264 && sl->mvc_anchor && sl->h264->ps.sps &&
                            sl->h264->ps.sps->mvc.present) {
                            sl->degenerate = 1;
                            sl->nb_mmco = 0;
                            return 0;
                        }
                        av_log(logctx, AV_LOG_ERROR,
                               "illegal long ref in memory management control "
                               "operation %d\n", opcode);
                        sl->nb_mmco = i;
                        return -1;
                    }
                    mmco[i].long_arg = long_arg;
                }

                if (opcode > (unsigned) MMCO_LONG) {
                    /* The dependent *anchor* slices of some 2D+delta MVC
                     * streams carry a degenerate memory management
                     * control list (mirrors the reordering-list
                     * tolerance): treat the list as empty, restricted
                     * to those slices. */
                    if (sl->h264 && sl->mvc_anchor && sl->h264->ps.sps &&
                        sl->h264->ps.sps->mvc.present) {
                        sl->degenerate = 1;
                        sl->nb_mmco = 0;
                        return 0;
                    }
                    av_log(logctx, AV_LOG_ERROR,
                           "illegal memory management control operation %d\n",
                           opcode);
                    sl->nb_mmco = i;
                    return -1;
                }
                if (opcode == MMCO_END)
                    break;
            }
            nb_mmco = i;
        }
    }

    sl->nb_mmco = nb_mmco;

    return 0;
}
