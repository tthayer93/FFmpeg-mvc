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

/* What a delivered multiview frame is allowed to say about itself.
 *
 * A two-view stream reaches a consumer as one of two things, and a container's
 * claim about the whole stream describes neither of them:
 *
 *   allviews  the native side-by-side composition: one frame holding both
 *             views of the access unit, left eye first. It is a stereoscopic
 *             picture, so it owes exactly one arrangement tag saying what it
 *             is - and no per-view id, because it is not a view.
 *   view0/1   one view delivered on its own: a MONO picture. It owes no
 *             arrangement tag at all, and it does owe the per-view id that
 *             says which eye it is - except on the pictures started before the
 *             fixture's lagged secondary sequence header registers the second
 *             view, which are started as plain pictures and carry none. Those
 *             are counted and reported, not fixed here.
 *   base-first only the base view's fragments of the leading access unit
 *             arrive, and these fixtures lag their secondary sequence header
 *             past it, so no second view is ever registered. What delivers is
 *             the plain H.264 decoder: the arrangement is the stream's own
 *             claim and it owes that claim back field for field, with no
 *             per-view id. This is the guard on the far side of the fix: a
 *             delivery rule aimed at multiview frames has no business touching
 *             a plain frame, and no FATE reference covers the arrangement of a
 *             plain stream's frames.
 *
 * The arrangement a container states for the stream (a Matroska StereoMode
 * tag, an MP4 st3d box) is cloned onto every picture the decoder allocates, by
 * ff_decode_frame_props() out of the decoder context's coded side data, and a
 * packet can carry one too. Nothing in that inheritance is written by the
 * decode: it is the stream's claim, and on the two-view discs measured it
 * reads "frame alternate / packed / primary_eye none / baseline 0" while the
 * frames leaving the decoder are a side-by-side pair, or one eye of it. A
 * player believes that tag and tries to unpack a second eye that is not in the
 * picture; an H.264 encoder reads the same side data and re-issues it to the
 * live bitstream as a frame-packing SEI. So the inherited claim is not
 * evidence about a delivered frame, and it is not allowed to travel as one.
 *
 * Nothing here can be measured without that inheritance, because a crafted
 * elementary stream carries no container claim of its own - so every delivery
 * below is run against injected inherited side data, along both routes a
 * container's claim travels: coded side data on the decoder context (what a
 * demuxer hands over) and side data on every packet. Three claims are used -
 * no claim at all, the claim the real discs carry, and a claim that
 * contradicts the truth about a composed frame in every field that frame has
 * to state (packing claimed inverted, the wrong primary eye, a 60 mm baseline
 * invented), so that merely passing the inheritance through cannot pass.
 *
 * That the claim really arrived is itself asserted, and not by reading the
 * side data back (which is what the fix has to do about it): the compose stage
 * reads the arrangement off the half it is about to weld, so an inverted claim
 * must land the two views the other way round. The fixtures paint their views
 * with two flat levels, so the corner luma of each half of a composed frame
 * says which view went where, and control-vs-inverted is the witness.
 *
 * The colour description is pinned in the same pass. The fixtures author no
 * VUI, so the description declared on the decoder context is the source's, and
 * a composed frame - which is built from its halves, not decoded - must report
 * it exactly as the single views do. Dropping it would leave a viewer to guess
 * the range and the matrix of half of a stereo pair.
 *
 * Status: the allviews legs and the single-view legs were red by design until
 * the composed tag was authored to state the eye it puts first and the
 * inherited arrangement stopped being delivered on a frame that is one eye.
 * The base-plain legs are green before the change and green after it: they are
 * the price the other three must not exceed, not the target.
 * The reference here is the truth about the frame, not whatever the container
 * happened to claim.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#include "libavutil/file.h"
#include "libavutil/frame.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"
#include "libavutil/stereo3d.h"

/* The access unit delimiter of H.264 (NAL unit type 9): the cut a demuxer
 * makes, and the cut these fixtures put at the head of every access unit. */
#define NAL_AUD 9

/* An IDR slice of the base view (NAL unit type 5), which is what a container's
 * seek table calls a keyframe; these fixtures refresh the base view on a
 * period. */
#define NAL_IDR 5

/* The fixture's display schedule: pictures one twenty-fifth of a second apart
 * in 90 kHz units, as in the sibling decoder tests. */
#define PTS_STEP 3600

/* The geometry of the fixture's views, and of the composed frame the fixture
 * owes a consumer that wants both views. */
#define VIEW_W 16
#define VIEW_H 16
#define COMPOSED_W (2 * VIEW_W)

#define MAX_AU 16
#define MAX_FRAMES 64

/* The colour the run declares on the decoder context. The fixtures author no
 * VUI, so this is the source's description and every frame - composed or one
 * eye - owes it back verbatim. */
#define COLOR_RANGE     AVCOL_RANGE_MPEG
#define COLOR_PRIMARIES AVCOL_PRI_BT709
#define COLOR_TRC       AVCOL_TRC_BT709
#define COLOR_SPC       AVCOL_SPC_BT709

/* One inherited claim, as a container hands it over. */
typedef struct Inherited {
    const char *name;
    int         present;
    AVStereo3D  claim;
} Inherited;

/* The claim the two-view discs carry (measured on the ALVH and HT2 discs:
 * "frame alternate / packed / primary_eye none / baseline 0" for a stream
 * whose frames are a side-by-side pair), and a claim that is wrong about
 * everything the composed frame must state. */
static const Inherited inherited_claims[] = {
    {
        "no-claim", 0,
        { AV_STEREO3D_2D, 0, AV_STEREO3D_VIEW_UNSPEC, AV_PRIMARY_EYE_NONE,
          0, { 0, 1 }, { 0, 1 } },
    },
    {
        "frame-alternate", 1,
        { AV_STEREO3D_FRAMESEQUENCE, 0, AV_STEREO3D_VIEW_PACKED,
          AV_PRIMARY_EYE_NONE, 0, { 0, 1 }, { 0, 1 } },
    },
    {
        "side-by-side-inverted-baseline", 1,
        { AV_STEREO3D_SIDEBYSIDE, AV_STEREO3D_FLAG_INVERT,
          AV_STEREO3D_VIEW_PACKED, AV_PRIMARY_EYE_RIGHT, 60000,
          { 0, 1 }, { 0, 1 } },
    },
};
#define CLAIM_NONE 0                        /* index of the control claim */
#define CLAIM_INVERTED 2                    /* index of the contradicting one */

/* One delivery a consumer can ask for, and what it owes back. */
typedef struct Mode {
    const char *name;
    const char *view_ids;    /* the decoder's view_ids option; NULL = unset */
    int         composed;    /* the frames are pairs, not single views */
    int         view_id;     /* the per-view id owed back; -1 = none owed */
    /* Deliver only the base view's fragments of each access unit, the way a
     * stream that carried nothing but a base layer would arrive. */
    int         base_only;
    /* Deliver only this many leading access units (0 = the whole stream).
     * Together with base_only this is how the table reaches the decoder's
     * plain-H.264 side: these fixtures lag their secondary sequence header to
     * the second access unit, so the leading base picture is started while
     * exactly one view has ever been registered - and a frame started that
     * way is a plain H.264 frame, whose arrangement is the stream's own
     * business. */
    unsigned    head_units;
} Mode;

static const Mode delivery_modes[] = {
    { "allviews",    "-1", 1, -1, 0, 0 },
    { "view0",        "0", 0,  0, 0, 0 },
    { "view1",        "1", 0,  1, 0, 0 },
    { "base-first", NULL,   0, -1, 1, 1 },
};
#define MODE_ALLVIEWS 0

/* One delivered frame, kept as far as its announcement goes. Every side data
 * entry of the types under test is counted, not just the first, because two
 * arrangement tags on one frame is itself a defect. The corner luma of each
 * half is kept too: the fixtures paint their views with two flat levels, so
 * the two corners say which view landed on which side - the only way to see
 * that an injected claim reached the frames the compose stage judged. */
typedef struct KeptFrame {
    int64_t pts;
    int width, height;
    int nb_stereo3d;
    AVStereo3D stereo3d;
    int nb_view_id;
    int view_id;
    int color_range, color_primaries, color_trc, colorspace;
    int luma_left, luma_right;
} KeptFrame;

typedef struct Run {
    char        name[128];
    KeptFrame   frames[MAX_FRAMES];
    unsigned    delivered;
    unsigned    composed;          /* frames at the composed geometry */
    unsigned    rejected;          /* fragments the session refused */
    unsigned    bad_arrangement;
    unsigned    bad_view_id;
    unsigned    bad_color;
    unsigned    bad_geometry;
    unsigned    view_id_seen;      /* a single view arrived with its id */
    unsigned    pre_registration;  /* single views started before the second
                                    * view was ever registered, so carrying no
                                    * per-view id; counted, see frame_defect */
} Run;

/* Offsets of the start codes of the access units of an Annex-B stream. */
static unsigned access_units(const uint8_t *buf, size_t size,
                             size_t *cut, unsigned max)
{
    unsigned n = 0;

    for (size_t i = 0; i + 3 < size; i++) {
        if (buf[i] || buf[i + 1] || buf[i + 2] != 1)
            continue;
        if ((buf[i + 3] & 0x1F) == NAL_AUD && n < max)
            cut[n++] = i;
    }

    return n;
}

/* Where the dependent view's part of an access unit begins (H.264 types 14,
 * 19 and 20), which is where the demuxers of these files cut it. */
static size_t dependent_part(const uint8_t *buf, size_t from, size_t to)
{
    for (size_t i = from; i + 3 < to; i++) {
        if (buf[i] || buf[i + 1] || buf[i + 2] != 1)
            continue;
        if ((buf[i + 3] & 0x1F) == 14 || (buf[i + 3] & 0x1F) == 19 ||
            (buf[i + 3] & 0x1F) == 20)
            return i;
    }

    return to;
}

/* Whether an access unit refreshes the base view, which is what makes a
 * container call it a keyframe. */
static int au_refreshes(const uint8_t *buf, size_t from, size_t to)
{
    for (size_t i = from; i + 3 < to; i++) {
        if (buf[i] || buf[i + 1] || buf[i + 2] != 1)
            continue;
        if ((buf[i + 3] & 0x1F) == NAL_IDR)
            return 1;
    }

    return 0;
}

static void run_init(Run *run, const char *path, const Mode *mode,
                     const Inherited *inh)
{
    memset(run, 0, sizeof(*run));
    snprintf(run->name, sizeof(run->name), "%s|%s|%s", path, mode->name,
             inh->name);
}

/* Keep a delivered frame's announcement. */
static int keep_frame(Run *run, const AVFrame *frame)
{
    KeptFrame *kf;

    if (run->delivered >= MAX_FRAMES) {
        fprintf(stderr, "%s: more frames than this program can keep\n",
                run->name);
        return AVERROR(ENOMEM);
    }
    kf = &run->frames[run->delivered];
    memset(kf, 0, sizeof(*kf));

    kf->pts      = frame->pts;
    kf->width    = frame->width;
    kf->height   = frame->height;
    kf->view_id  = -1;
    kf->color_range     = frame->color_range;
    kf->color_primaries = frame->color_primaries;
    kf->color_trc       = frame->color_trc;
    kf->colorspace      = frame->colorspace;

    for (int i = 0; i < frame->nb_side_data; i++) {
        const AVFrameSideData *sd = frame->side_data[i];

        if (sd->type == AV_FRAME_DATA_STEREO3D) {
            if (kf->nb_stereo3d == 0 && sd->size >= (int)sizeof(AVStereo3D))
                memcpy(&kf->stereo3d, sd->data, sizeof(kf->stereo3d));
            kf->nb_stereo3d++;
        } else if (sd->type == AV_FRAME_DATA_VIEW_ID) {
            if (kf->nb_view_id == 0 && sd->size >= sizeof(int))
                memcpy(&kf->view_id, sd->data, sizeof(kf->view_id));
            kf->nb_view_id++;
        }
    }

    /* The two corners of the luma plane at the delivered width: which view
     * the compose stage put on which side. */
    kf->luma_left  = frame->data[0] ? (int)frame->data[0][0] : -1;
    kf->luma_right = (frame->data[0] && frame->width >= 2 * VIEW_W)
                     ? (int)frame->data[0][frame->width / 2] : -1;

    if (frame->width == COMPOSED_W && frame->height == VIEW_H)
        run->composed++;
    run->delivered++;

    return 0;
}

/* What this frame owes, against the delivery it came from. Returns 1 with the
 * reason in detail, or 0 when the frame is what it claims to be. */
static int frame_defect(Run *run, const KeptFrame *kf, const Mode *mode,
                        const Inherited *inh, char *detail, size_t detail_size)
{
    const AVStereo3D *s3d = &kf->stereo3d;

    if (mode->base_only) {
        /* Only the base view's fragments of the leading access unit were
         * delivered, and these fixtures lag their secondary sequence header
         * past it: this frame was started while one view had ever been
         * registered, which makes it a plain H.264 delivery. Its arrangement
         * is the stream's own claim and this decoder owes it back verbatim,
         * with no per-view id - there is nothing to tell apart. The rule
         * being written for multiview frames has no business here. */
        if (kf->width != VIEW_W || kf->height != VIEW_H) {
            snprintf(detail, detail_size,
                     "a one-view delivery went out at %dx%d, not %dx%d",
                     kf->width, kf->height, VIEW_W, VIEW_H);
            run->bad_geometry++;
            return 1;
        }
        if (!!kf->nb_stereo3d != !!inh->present) {
            snprintf(detail, detail_size,
                     "a one-view delivery carries %d arrangement tag%s where "
                     "the stream states %d: nothing but a base view was ever "
                     "registered, so this is a plain H.264 frame and the "
                     "arrangement is the stream's own claim, not this "
                     "decoder's business",
                     kf->nb_stereo3d, kf->nb_stereo3d > 1 ? "s" : "",
                     inh->present);
            run->bad_arrangement++;
            return 1;
        }
        if (inh->present &&
            (s3d->type        != inh->claim.type ||
             s3d->flags       != inh->claim.flags ||
             s3d->view        != inh->claim.view ||
             s3d->primary_eye != inh->claim.primary_eye ||
             s3d->baseline    != inh->claim.baseline ||
             s3d->horizontal_disparity_adjustment.num !=
                 inh->claim.horizontal_disparity_adjustment.num ||
             s3d->horizontal_disparity_adjustment.den !=
                 inh->claim.horizontal_disparity_adjustment.den ||
             s3d->horizontal_field_of_view.num !=
                 inh->claim.horizontal_field_of_view.num ||
             s3d->horizontal_field_of_view.den !=
                 inh->claim.horizontal_field_of_view.den)) {
            snprintf(detail, detail_size,
                     "a one-view delivery rewrote the stream's own claim into "
                     "'%s'/%s/%s inv=%d baseline=%u (the stream said '%s'/%s/"
                     "%s inv=%d baseline=%u)",
                     av_stereo3d_type_name(s3d->type),
                     av_stereo3d_view_name(s3d->view),
                     av_stereo3d_primary_eye_name(s3d->primary_eye),
                     !!(s3d->flags & AV_STEREO3D_FLAG_INVERT), s3d->baseline,
                     av_stereo3d_type_name(inh->claim.type),
                     av_stereo3d_view_name(inh->claim.view),
                     av_stereo3d_primary_eye_name(inh->claim.primary_eye),
                     !!(inh->claim.flags & AV_STEREO3D_FLAG_INVERT),
                     inh->claim.baseline);
            run->bad_arrangement++;
            return 1;
        }
        if (kf->nb_view_id) {
            snprintf(detail, detail_size,
                     "a one-view delivery carries %d per-view ids; no second "
                     "view was ever registered, so there is nothing to tell "
                     "apart", kf->nb_view_id);
            run->bad_view_id++;
            return 1;
        }
    } else if (mode->composed) {
        if (kf->width != COMPOSED_W || kf->height != VIEW_H) {
            snprintf(detail, detail_size,
                     "a composed delivery went out at %dx%d, not %dx%d",
                     kf->width, kf->height, COMPOSED_W, VIEW_H);
            run->bad_geometry++;
            return 1;
        }
        if (kf->nb_stereo3d != 1) {
            snprintf(detail, detail_size,
                     "a composed frame carries %d arrangement tags, not the "
                     "one it owes", kf->nb_stereo3d);
            run->bad_arrangement++;
            return 1;
        }
        if (s3d->type != AV_STEREO3D_SIDEBYSIDE) {
            snprintf(detail, detail_size,
                     "a composed frame is tagged '%s', not side by side - "
                     "that is the container's claim travelling as the frame's "
                     "own", av_stereo3d_type_name(s3d->type));
            run->bad_arrangement++;
            return 1;
        }
        if (s3d->flags & AV_STEREO3D_FLAG_INVERT) {
            snprintf(detail, detail_size,
                     "a composed frame claims an inverted packing although "
                     "the compose stage always puts the left eye on the left");
            run->bad_arrangement++;
            return 1;
        }
        if (s3d->view != AV_STEREO3D_VIEW_PACKED) {
            snprintf(detail, detail_size,
                     "a composed frame says '%s' about its views, not packed",
                     av_stereo3d_view_name(s3d->view));
            run->bad_arrangement++;
            return 1;
        }
        if (s3d->primary_eye != AV_PRIMARY_EYE_LEFT) {
            snprintf(detail, detail_size,
                     "a composed frame names '%s' as the primary eye; it "
                     "packs the left eye first",
                     av_stereo3d_primary_eye_name(s3d->primary_eye));
            run->bad_arrangement++;
            return 1;
        }
        if (s3d->baseline) {
            snprintf(detail, detail_size,
                     "a composed frame states a %u um baseline that no "
                     "bitstream here carries", s3d->baseline);
            run->bad_arrangement++;
            return 1;
        }
        if (s3d->horizontal_disparity_adjustment.num ||
            s3d->horizontal_field_of_view.num) {
            snprintf(detail, detail_size,
                     "a composed frame states a disparity adjustment or a "
                     "field of view that no bitstream here carries");
            run->bad_arrangement++;
            return 1;
        }
        if (kf->nb_view_id) {
            snprintf(detail, detail_size,
                     "a composed frame carries %d per-view ids; both views "
                     "are in it, so it is not a view", kf->nb_view_id);
            run->bad_view_id++;
            return 1;
        }
    } else {
        if (kf->width != VIEW_W || kf->height != VIEW_H) {
            snprintf(detail, detail_size,
                     "a single-view delivery went out at %dx%d, not %dx%d",
                     kf->width, kf->height, VIEW_W, VIEW_H);
            run->bad_geometry++;
            return 1;
        }
        if (kf->nb_stereo3d) {
            snprintf(detail, detail_size,
                     "one eye delivered alone carries %d arrangement tag%s "
                     "('%s'): it is a mono picture, and a player would unpack "
                     "a second eye from that while an H.264 encoder would "
                     "re-issue it as a frame-packing SEI",
                     kf->nb_stereo3d, kf->nb_stereo3d > 1 ? "s" : "",
                     av_stereo3d_type_name(s3d->type));
            run->bad_arrangement++;
            return 1;
        }
        if (kf->nb_view_id == 1) {
            run->view_id_seen = 1;
            if (kf->view_id != mode->view_id) {
                snprintf(detail, detail_size,
                         "one eye delivered alone claims view id %d, not %d",
                         kf->view_id, mode->view_id);
                run->bad_view_id++;
                return 1;
            }
        } else if (run->view_id_seen) {
            snprintf(detail, detail_size,
                     "one eye delivered alone carries %d per-view ids after "
                     "the stream had begun stamping them: the id is what says "
                     "which eye this is, and a consumer cannot guess it",
                     kf->nb_view_id);
            run->bad_view_id++;
            return 1;
        } else {
            /* The fixture lags its secondary sequence header, and the per-view
             * id is stamped where the picture is started (h264_frame_start):
             * a picture started before that header arrives is started as a
             * plain one and carries no id at all. That is a property of when
             * the stream says how many views there are, not of the delivery
             * rule under test, so it is counted here and reported rather than
             * passed off as correct. The reference pins the count, so it
             * cannot quietly grow. */
            run->pre_registration++;
        }
    }

    if (kf->color_range     != COLOR_RANGE     ||
        kf->color_primaries != COLOR_PRIMARIES ||
        kf->color_trc       != COLOR_TRC       ||
        kf->colorspace      != COLOR_SPC) {
        snprintf(detail, detail_size,
                 "colour description is %s/%s/%s/%s, not the source's "
                 "%s/%s/%s/%s",
                 av_color_range_name(kf->color_range),
                 av_color_primaries_name(kf->color_primaries),
                 av_color_transfer_name(kf->color_trc),
                 av_color_space_name(kf->colorspace),
                 av_color_range_name(COLOR_RANGE),
                 av_color_primaries_name(COLOR_PRIMARIES),
                 av_color_transfer_name(COLOR_TRC),
                 av_color_space_name(COLOR_SPC));
        run->bad_color++;
        return 1;
    }

    return 0;
}

/* One decoder session, opened the way a consumer opens one for a delivery:
 * the views it asked for, one thread so the table is the decoder's answer,
 * the colour the source states, and the container's claim handed over the way
 * a demuxer hands it over (coded side data on the context). */
static AVCodecContext *open_session(const char *who, const AVCodec *codec,
                                    const Mode *mode, const Inherited *inh)
{
    AVCodecContext *avctx;
    int ret;

    if (!(avctx = avcodec_alloc_context3(codec)))
        return NULL;
    avctx->thread_count = 1;
    avctx->pkt_timebase = (AVRational){ 1, 90000 };
    avctx->color_range     = COLOR_RANGE;
    avctx->color_primaries = COLOR_PRIMARIES;
    avctx->color_trc       = COLOR_TRC;
    avctx->colorspace      = COLOR_SPC;

    if (mode->view_ids &&
        (ret = av_opt_set(avctx, "view_ids", mode->view_ids,
                          AV_OPT_SEARCH_CHILDREN)) < 0) {
        fprintf(stderr, "%s: view_ids=%s: %s\n", who, mode->view_ids,
                av_err2str(ret));
        avcodec_free_context(&avctx);
        return NULL;
    }

    if (inh->present) {
        size_t size;
        AVStereo3D *claim = av_stereo3d_alloc_size(&size);

        if (!claim) {
            avcodec_free_context(&avctx);
            return NULL;
        }
        *claim = inh->claim;
        if (!av_packet_side_data_add(&avctx->coded_side_data,
                                     &avctx->nb_coded_side_data,
                                     AV_PKT_DATA_STEREO3D,
                                     claim, size, 0)) {
            av_freep(&claim);
            avcodec_free_context(&avctx);
            return NULL;
        }
    }

    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        fprintf(stderr, "%s: open: %s\n", who, av_err2str(ret));
        avcodec_free_context(&avctx);
        return NULL;
    }

    return avctx;
}

/* Attach the same inherited claim to a packet: the other route a container's
 * claim travels (ff_decode_frame_props_from_pkt()). */
static int stamp_packet(AVPacket *pkt, const Inherited *inh)
{
    size_t size;
    AVStereo3D *claim;

    if (!inh->present)
        return 0;

    if (!(claim = av_stereo3d_alloc_size(&size)))
        return AVERROR(ENOMEM);
    *claim = inh->claim;
    if (!av_packet_side_data_add(&pkt->side_data, &pkt->side_data_elems,
                                 AV_PKT_DATA_STEREO3D, claim, size, 0)) {
        av_freep(&claim);
        return AVERROR(ENOMEM);
    }

    return 0;
}

/* Deliver every frame the session has and keep it. */
static int drain(Run *run, AVCodecContext *avctx, AVFrame *frame)
{
    for (;;) {
        int ret = avcodec_receive_frame(avctx, frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "%s: decode failed: %s\n", run->name,
                    av_err2str(ret));
            return ret;
        }
        ret = keep_frame(run, frame);
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }
}

/* Feed every access unit of the stream, split the way the demuxers of these
 * files split it, each part carrying the time the stream's own clock gives it.
 * A base-only delivery feeds only the first part of each unit - what a stream
 * with nothing but a base layer hands over - and a mode that names a head
 * count stops after that many units. */
static int feed(Run *run, AVCodecContext *avctx, const uint8_t *buf,
                size_t size, const size_t *cut, unsigned units,
                const Inherited *inh, const Mode *mode,
                AVPacket *pkt, AVFrame *frame)
{
    const int parts = mode->base_only ? 1 : 2;
    const unsigned n_units = mode->head_units && mode->head_units < units ?
                            mode->head_units : units;

    for (unsigned n = 0; n < n_units; n++) {
        const size_t from = n ? cut[n] : 0;
        const size_t to   = n + 1 < units ? cut[n + 1] : size;
        const size_t dep  = dependent_part(buf, from, to);

        for (int part = 0; part < parts; part++) {
            const size_t begin = part ? dep : from;
            const size_t end   = part ? to  : dep;
            int ret;

            if (end == begin)       /* an access unit may have no second part */
                continue;

            if ((ret = av_new_packet(pkt, end - begin)) < 0) {
                fprintf(stderr, "%s: packet: %s\n", run->name,
                        av_err2str(ret));
                return -1;
            }
            memcpy(pkt->data, buf + begin, end - begin);
            pkt->pts       = part ? AV_NOPTS_VALUE : (int64_t)n * PTS_STEP;
            pkt->dts       = pkt->pts;
            pkt->time_base = (AVRational){ 1, 90000 };
            pkt->flags     = !part && au_refreshes(buf, from, to)
                             ? AV_PKT_FLAG_KEY : 0;

            if ((ret = stamp_packet(pkt, inh)) < 0) {
                fprintf(stderr, "%s: packet side data: %s\n", run->name,
                        av_err2str(ret));
                av_packet_unref(pkt);
                return -1;
            }

            ret = avcodec_send_packet(avctx, pkt);
            av_packet_unref(pkt);
            if (ret == AVERROR_INVALIDDATA) {
                /* A refusal is recorded, not fatal: the frames missing from
                 * what follows are the defect this table has to make legible. */
                fprintf(stderr, "%s: access unit %u part %d refused: %s\n",
                        run->name, n, part, av_err2str(ret));
                run->rejected++;
                continue;
            }
            if (ret < 0) {
                fprintf(stderr, "%s: access unit %u part %d: %s\n",
                        run->name, n, part, av_err2str(ret));
                return -1;
            }
            if (drain(run, avctx, frame) < 0)
                return -1;
        }
    }

    {
        int ret = avcodec_send_packet(avctx, NULL);

        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return -1;
    }
    return drain(run, avctx, frame);
}

static void print_run(const Run *run, int ok)
{
    for (unsigned i = 0; i < run->delivered; i++) {
        const KeptFrame *kf = &run->frames[i];

        printf("%s: pts=%lld %dx%d arrangement=%d(%s/%s/%s inv=%d baseline=%u) "
               "view_id=%d(%d) colour=%s/%s/%s/%s halves=%d,%d\n",
               run->name, (long long)kf->pts, kf->width, kf->height,
               kf->nb_stereo3d,
               kf->nb_stereo3d ? av_stereo3d_type_name(kf->stereo3d.type) : "-",
               kf->nb_stereo3d ? av_stereo3d_view_name(kf->stereo3d.view) : "-",
               kf->nb_stereo3d ? av_stereo3d_primary_eye_name(kf->stereo3d.primary_eye) : "-",
               kf->nb_stereo3d ? !!(kf->stereo3d.flags & AV_STEREO3D_FLAG_INVERT) : 0,
               kf->nb_stereo3d ? kf->stereo3d.baseline : 0u,
               kf->nb_view_id, kf->view_id,
               av_color_range_name(kf->color_range),
               av_color_primaries_name(kf->color_primaries),
               av_color_transfer_name(kf->color_trc),
               av_color_space_name(kf->colorspace),
               kf->luma_left, kf->luma_right);
    }
    printf("%s: delivered=%u composed=%u rejected=%u bad_arrangement=%u "
           "bad_view_id=%u bad_color=%u bad_geometry=%u pre_registration=%u "
           "result=%s\n",
           run->name, run->delivered, run->composed, run->rejected,
           run->bad_arrangement, run->bad_view_id, run->bad_color,
           run->bad_geometry, run->pre_registration, ok ? "ok" : "FAIL");
}

/* Did the injected claim reach the frames at all? The compose stage reads the
 * arrangement off the half it is about to weld (that is how the eye order is
 * chosen), so a claim saying the packing is inverted must land the views the
 * other way round: the corner luma of the two halves, control run against
 * inverted run, at the same pts. Returns 0 with the reason when the witness
 * did not happen. */
static int claim_reached_the_frames(const Run *control, const Run *inverted,
                                    char *detail, size_t detail_size)
{
    int compared = 0;

    if (inverted->delivered != control->delivered) {
        snprintf(detail, detail_size,
                 "the inverted-claim run delivered %u frames where the "
                 "control delivered %u",
                 inverted->delivered, control->delivered);
        return 0;
    }

    for (unsigned i = 0; i < control->delivered; i++) {
        const KeptFrame *a = &control->frames[i];
        const KeptFrame *b = &inverted->frames[i];

        if (a->luma_left < 0 || a->luma_right < 0 ||
            a->luma_left == a->luma_right)
            continue;                       /* the halves say nothing here */
        if (a->pts != b->pts) {
            snprintf(detail, detail_size,
                     "frame %u of the two runs sits at another pts (%lld vs "
                     "%lld)", i, (long long)a->pts, (long long)b->pts);
            return 0;
        }
        if (a->luma_left != b->luma_right || a->luma_right != b->luma_left) {
            snprintf(detail, detail_size,
                     "the inverted claim did not swap the halves at pts "
                     "%lld: control halves=%d,%d, inverted halves=%d,%d - the "
                     "inherited claim never reached the frames the compose "
                     "stage judged",
                     (long long)a->pts, a->luma_left, a->luma_right,
                     b->luma_left, b->luma_right);
            return 0;
        }
        compared++;
    }

    if (!compared) {
        snprintf(detail, detail_size,
                 "no frame of the fixture let the two halves be told apart, "
                 "so the injection could not be witnessed");
        return 0;
    }

    return 1;
}

static int run_stream(const char *path)
{
    static Run runs[FF_ARRAY_ELEMS(inherited_claims)]
                          [FF_ARRAY_ELEMS(delivery_modes)];
    const char    *tag;              /* the fixture's own name, so the table a
                                      * reference is graded against does not
                                      * spell the sample directory it came from */
    uint8_t       *buf   = NULL;
    size_t         cut[MAX_AU];
    size_t         size  = 0;
    unsigned       units = 0;
    const AVCodec *codec;
    AVFrame       *frame = NULL;
    AVPacket      *pkt   = NULL;
    int            fails = 0;
    int            ret;

    tag = strrchr(path, '/');
    tag = tag ? tag + 1 : path;

    if ((ret = av_file_map(path, &buf, &size, 0, NULL)) < 0) {
        fprintf(stderr, "sidedata: %s: %s\n", path, av_err2str(ret));
        return 1;
    }
    if (!(units = access_units(buf, size, cut, FF_ARRAY_ELEMS(cut)))) {
        fprintf(stderr, "sidedata: %s: no access unit delimiter\n", path);
        av_file_unmap(buf, size);
        return 1;
    }
    if (!(codec = avcodec_find_decoder(AV_CODEC_ID_H264)) ||
        !(frame = av_frame_alloc()) || !(pkt = av_packet_alloc())) {
        av_file_unmap(buf, size);
        return 1;
    }

    printf("sidedata: %s units=%u step=%d view=%dx%d composed=%dx%d\n", tag,
           units, PTS_STEP, VIEW_W, VIEW_H, COMPOSED_W, VIEW_H);

    for (unsigned c = 0; c < FF_ARRAY_ELEMS(inherited_claims); c++) {
        const Inherited *inh = &inherited_claims[c];

        for (unsigned m = 0; m < FF_ARRAY_ELEMS(delivery_modes); m++) {
            const Mode     *mode = &delivery_modes[m];
            Run            *run  = &runs[c][m];
            AVCodecContext *avctx;
            char            reason[512];
            int             ok;

            run_init(run, tag, mode, inh);

            if (!(avctx = open_session(run->name, codec, mode, inh))) {
                print_run(run, 0);
                fprintf(stderr, "ASSERT-FAIL: %s: the session could not be "
                                "opened\n", run->name);
                fails++;
                continue;
            }

            if (feed(run, avctx, buf, size, cut, units, inh, mode,
                     pkt, frame) < 0) {
                avcodec_free_context(&avctx);
                print_run(run, 0);
                fprintf(stderr, "ASSERT-FAIL: %s: the stream could not be "
                                "delivered\n", run->name);
                fails++;
                continue;
            }
            avcodec_free_context(&avctx);

            /* the delivery itself has to be the delivery the mode names, or
             * the checks below would be grading the wrong thing */
            ok = run->delivered > 0 && !run->rejected;
            if (mode->composed)
                ok = ok && run->composed == run->delivered;
            else
                ok = ok && run->composed == 0;
            if (!ok) {
                fprintf(stderr, "ASSERT-FAIL: %s: not the '%s' delivery the "
                                "mode names: %u delivered, %u composed, %u "
                                "refused\n", run->name, mode->name,
                        run->delivered, run->composed, run->rejected);
            }

            for (unsigned i = 0; i < run->delivered && ok; i++) {
                reason[0] = 0;
                if (frame_defect(run, &run->frames[i], mode, inh, reason,
                                 sizeof(reason))) {
                    fprintf(stderr, "ASSERT-FAIL: %s: pts %lld: %s\n",
                            run->name, (long long)run->frames[i].pts, reason);
                    ok = 0;
                }
            }

            if (ok && !mode->composed && !mode->base_only &&
                !run->view_id_seen) {
                /* The rule under test says a single view keeps the per-view id
                 * that says which eye it is. That is only witnessed if the id
                 * was ever stamped: a run in which no frame ever carried one
                 * would grade the rule against nothing. */
                fprintf(stderr, "ASSERT-FAIL: %s: no frame of this single-view "
                                "delivery ever carried the per-view id that "
                                "says which eye it is\n", run->name);
                ok = 0;
            }

            if (!ok) {
                fprintf(stderr, "ASSERT-FAIL: %s: %u frames delivered (%u "
                                "composed), %u refused; arrangement %u, view "
                                "id %u, colour %u, geometry %u wrong\n",
                        run->name, run->delivered, run->composed,
                        run->rejected, run->bad_arrangement, run->bad_view_id,
                        run->bad_color, run->bad_geometry);
                fails++;
            }
            print_run(run, ok);
        }
    }

    /* The injection witness: without it, "no arrangement tag on a single
     * view" could be the trivial truth about a fixture that never carried
     * one. */
    {
        char detail[512] = "";

        if (!claim_reached_the_frames(&runs[CLAIM_NONE][MODE_ALLVIEWS],
                                      &runs[CLAIM_INVERTED][MODE_ALLVIEWS],
                                      detail, sizeof(detail))) {
            fprintf(stderr, "ASSERT-FAIL: %s|allviews|%s: the injected claim "
                            "was never witnessed: %s\n", tag,
                    inherited_claims[CLAIM_INVERTED].name, detail);
            fails++;
        } else {
            printf("%s|allviews|%s: injection witnessed - the inverted claim "
                   "swapped the composed halves, so the claim did reach the "
                   "frames\n", tag, inherited_claims[CLAIM_INVERTED].name);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_file_unmap(buf, size);

    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    int fails = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <annex-b two-view stream> [...]\n", argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1)
            printf("\n");
        fails |= run_stream(argv[i]);
    }

    return fails ? 1 : 0;
}
