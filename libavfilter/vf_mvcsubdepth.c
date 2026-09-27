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
 * mvcsubdepth - put a subtitle layer into each eye of a composed stereoscopic
 * frame, at the depth that frame was authored with.
 *
 * Input 0 is the video: a frame carrying both eyes side by side, as a
 * multiview decode composes them.  Input 1 is the subtitle rendered to a
 * picture (the transcoder's subtitle-to-video canvas is what writes those, and
 * it also writes the marker telling which depth sequence that track belongs
 * to).  The output is input 0 with the subtitle burned into the left half and
 * into the right half, each at its own horizontal place.
 *
 * What makes the caption float is that the two places differ.  A stereoscopic
 * pair shows an object in front of the screen when the two views put it
 * inward from each other, and behind when they put it outward; so the per-eye
 * displacement is derived from the frame's authored depth for this subtitle's
 * plane by ff_mvc_sub_eye_shift(), which is where the sign is decided and
 * pinned.  With no authored depth the two halves get the same place, and the
 * caption sits on the screen - which is the whole behaviour of a plain double
 * overlay, and what this filter deliberately falls back to rather than
 * inventing a depth of its own.
 *
 * The arithmetic itself lives in mvcsubdepth.h so that it can be read and
 * tested apart from the filter.  The placement is described in both places as
 * "pad the subtitle with slack columns, then read one window per eye": the
 * compositor does not materialise that padded canvas, it clips the window
 * against the canvas instead, which is the same picture with less memory.
 */

#include <errno.h>
#include <string.h>

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/fifo.h"
#include "libavutil/frame.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rational.h"
#include "libavutil/stereo3d.h"
#include "libswscale/swscale.h"
#include "avfilter.h"
#include "filters.h"
#include "framesync.h"
#include "mvcsubdepth.h"
#include "video.h"

/* Columns of transparent slack assumed on each side of the subtitle canvas.
 * This is how far a window can be walked away from the caption's own place
 * before the canvas runs out: authored depth for a subtitle has been seen up
 * to the high tens of pixels at full raster, so 64 covers the authored range
 * with room to spare, and a requested shift beyond it is clamped rather than
 * dropped.  Documented with the filter. */
#define SUB_MARGIN 64

/* Bytes per pixel of the one working format, and the byte an RGBA pixel keeps
 * its alpha in. */
#define PIX_STEP 4

/* THE PTS-KEYED PAIRING CONTRACT (why this filter does not run on framesync)
 *
 * The rule this filter implements, in one line:
 *
 *   the canvas painted for an output frame at pts P is the subtitle epoch
 *   whose DISPLAY INTERVAL covers P - the last non-heartbeat epoch that started
 *   strictly before P and is superseded by no epoch that also starts strictly
 *   before P.  A successor takes a grid line only by starting strictly before
 *   it: an epoch whose pts is exactly P is the edge itself - it has taken
 *   effect for the frames after it, but the frame on that exact grid line is
 *   the last one carrying the previous state.  Nothing the filter paints
 *   depends on anything else; in particular it does not depend on when a frame
 *   happened to arrive.
 *
 * The generic framesync helper cannot express that rule for this filter's
 * shipping option combination.  The shipping graph runs mvcsubdepth with
 * eof_action=pass, which makes framesync drop the sync flag from the subtitle
 * input (framesync.c gives only the main input a sync level), so
 * ff_framesync_get_frame() hands back whatever canvas last ARRIVED, and
 * framesync_advance() replaces it only when a canvas pts lands exactly on the
 * event pts.  Arrival order is decided by the transcoder's subtitle heartbeat,
 * which runs on a different thread from the filter graph, so at a caption start
 * or end the canvas held for an output frame can flip between two runs of the
 * same command over the same input bytes.  That is a scheduling race, not
 * arithmetic: a cold, no-seek repeat of the shipping command was measured to
 * differ on 27 of 14386 overlay rows with every underlying pad byte-identical,
 * and adding -loglevel alone moved a caption start row.
 *
 * The transcoder side already supplies the interval information, unchanged:
 * every canvas the sub2video choke point pushes carries the pts at which its
 * content takes effect - a caption at its own start pts, a cleared canvas at
 * the previous caption's end pts - and the heartbeat re-pushes in between carry
 * the same content again under a heartbeat stamp.  Those duplicates are only
 * watermarks proving that the subtitle side has advanced; they do not enter the
 * display schedule.  The rule is therefore recoverable from the pts stream
 * alone, which is what the two queues below do:
 *
 *   sub_q   - display-schedule canvases held in ascending push pts; the head is
 *             the covering epoch.  A canvas arriving out of pts order (a caption
 *             cleared at an end pts that reaches us after a later push) is
 *             inserted into position, so the order the queue keeps is pts order,
 *             not arrival order.
 *   main_q  - main frames held until the subtitle side has passed their pts.
 *
 * A main frame at P is emitted once a canvas starting strictly after P has
 * arrived (that push is the watermark proving no future canvas can still land
 * at or before P), or once the subtitle side has ended.  Waiting for that
 * watermark is what removes the boundary race, and it costs about one frame of
 * latency.  Framesync cannot be told to "wait for the secondary past the
 * primary" at all under eof_action=pass, which is the second reason the pump
 * lives here.
 *
 * The framesync OPTION SURFACE is kept as it was: FRAMESYNC_DEFINE_CLASS still
 * defines eof_action/shortest/repeatlast/ts_sync_mode, and config_output()
 * mirrors the same normalisation and the same four behaviours (repeat, endall,
 * pass, and the ts_sync_mode=nearest choice) that ff_framesync_configure() and
 * framesync_advance() used to give, so no spelling of this filter changes
 * meaning.  Only the pump is bespoke.
 *
 * One visible difference from the old framesync dualinput path: the output now
 * rides the video input's time base (config_output sets outlink->time_base to
 * inlink->time_base) rather than the gcd of the two inputs that path chose.
 * For the constant-frame-rate caption case this filter ships with, the
 * delivered pts are numerically identical, but the coarser base is visible to
 * anything downstream that reads it - an enc_time_base consumer, for one. */
typedef struct MVCSubEpoch {
    AVFrame *frame;   /* a subtitle canvas the filter holds a reference to */
    int64_t  pts;     /* its push pts, in the OUTPUT time base */
} MVCSubEpoch;

typedef struct MVCSubDepthContext {
    const AVClass *class;
    FFFrameSync fs;    /* carries the option surface only; the pump is bespoke */

    const char *depth;    /* the option's spelling of the mode, parsed at init */
    int eye_width;        /* 0 = take half of the video input's width */

    int depth_mode;   /* FF_MVC_SUB_DEPTH_*, resolved from depth in init */
    int depth_param;  /* the pixels for SHIFT, the sequence for PLANE, else 0 */

    int eye_w;      /* the eye width in use, resolved at configuration */

    AVFrame *canvas;    /* the subtitle frame resized to one eye, if needed */
    struct SwsContext *sws;

    int marker_seen;    /* a composed side-by-side marker has been seen */
    int marker_warned;  /* the notice about its absence has been given */
    int scale_logged;   /* the subtitle resize has been reported once */
    int scale_warned;   /* the notice about a failed resize has been given */
    int clamp_logged;   /* the notice about clamped depth has been given */

    unsigned nb_frames; /* frames delivered, of which: */
    unsigned nb_placed; /* got a subtitle and a window pair */
    unsigned nb_shifted;/* of those, got a non-zero per-eye displacement */

    /* Bespoke, pts-keyed pairing state (see the contract above). */
    int eof_action;         /* EOF_ACTION_*, after the framesync normalisation */
    int ts_nearest;         /* ts_sync_mode=nearest, mirrored from the class   */
    AVRational in1_tb;      /* subtitle input time base (canvas pts live here) */
    AVRational out_tb;      /* output (== video input) time base */
    MVCSubEpoch **sub_q;    /* held canvases, ascending pts, head = covering   */
    unsigned sub_q_nb;      /* entries in sub_q                                */
    unsigned sub_q_cap;     /* slots allocated for sub_q                       */
    AVFifo *main_q;         /* of AVFrame*, ascending pts, awaiting their turn */
    int64_t sub_frontier;   /* greatest subtitle push pts seen, output base    */
    int64_t sub_eof_pts;    /* pts the subtitle side ended with, output base   */
    int64_t last_out_pts;   /* pts last given to ff_outlink_set_status         */
    int64_t hb_bias;        /* one subtitle tick in the output base, >= 1      */
    int     sub_eof;        /* subtitle input reached EOF */
    int     main_eof;       /* main input reached EOF */
    int     out_eof_set;    /* EOF status already set on the output */
} MVCSubDepthContext;

#define OFFSET(x) offsetof(MVCSubDepthContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption mvcsubdepth_options[] = {
    { "depth",  "how the caption's depth is chosen: auto (the sequence the "
                "track is marked with), flat (the screen plane), "
                "shift=<pixels> (a constant displacement) or plane=<0..31> "
                "(a named sequence); 0 and 1 are flat and auto",
      OFFSET(depth), AV_OPT_TYPE_STRING, { .str = "auto" }, 0, 0, FLAGS },
    { "eye_width", "width of one eye; 0 uses half of the video input's width",
      OFFSET(eye_width), AV_OPT_TYPE_INT, { .i64 = 0 },
      0, INT_MAX, FLAGS },
    { NULL }
};

FRAMESYNC_DEFINE_CLASS(mvcsubdepth, MVCSubDepthContext, fs);

static int check_marker(MVCSubDepthContext *s, AVFilterContext *ctx,
                        const AVFrame *main)
{
    const AVFrameSideData *sd = av_frame_get_side_data(main,
                                                       AV_FRAME_DATA_STEREO3D);

    if (sd && sd->size >= (int)sizeof(AVStereo3D)) {
        const AVStereo3D *st = (const AVStereo3D *)sd->data;

        if (st->type == AV_STEREO3D_SIDEBYSIDE &&
            st->view == AV_STEREO3D_VIEW_PACKED) {
            s->marker_seen = 1;
            return 1;
        }
    }
    if (s->marker_seen)
        return 0;
    if (!s->marker_warned) {
        av_log(ctx, AV_LOG_WARNING, "No composed side-by-side marker on the "
               "video input; taking its two halves as the two eyes. Place "
               "this filter directly after the source that composes them: a "
               "stack filter drops this marker along with the depth "
               "metadata.\n");
        s->marker_warned = 1;
    }
    return 0;
}

/* Is this subtitle canvas a heartbeat duplicate rather than a display edge?
 * The transcoder stamps the answer on the frame; without the stamp, the safe
 * reading is that a push is content, not a clock witness. */
static int heartbeat_of(const AVFrame *sub)
{
    const AVFrameSideData *sd = av_frame_get_side_data(sub,
                                                       AV_FRAME_DATA_MVC_SUB_PLANE);

    return ff_mvc_sub_is_heartbeat(sd ? sd->data : NULL, sd ? sd->size : 0);
}

/* The depth sequence this frame's caption belongs to: the one the user named
 * in plane= mode, or what the subtitle frame is marked with. The other modes
 * ignore the answer - it is still read so that the debug line in the frame
 * path can report a plane in every mode. */
static int plane_of(const MVCSubDepthContext *s, const AVFrame *sub)
{
    const AVFrameSideData *sd;

    if (s->depth_mode == FF_MVC_SUB_DEPTH_PLANE)
        return s->depth_param;
    sd = av_frame_get_side_data(sub, AV_FRAME_DATA_MVC_SUB_PLANE);
    return ff_mvc_sub_plane(sd ? sd->data : NULL, sd ? sd->size : 0);
}

/* The displacement for one frame, in video pixels and in the authored sign:
 * positive toward the viewer. The mode says which of the four answers this
 * filter ever gives: shift= answers its own number, flat= answers zero, and
 * auto= and plane= read the authored table for the sequence plane_of named. */
static int offset_of(const MVCSubDepthContext *s, const AVFrame *main,
                     int plane)
{
    const AVFrameSideData *sd;

    if (s->depth_mode == FF_MVC_SUB_DEPTH_SHIFT)
        return s->depth_param;
    if (s->depth_mode == FF_MVC_SUB_DEPTH_FLAT)
        return 0;
    sd = av_frame_get_side_data(main, AV_FRAME_DATA_MVC_SS_OFFSETS);
    return ff_mvc_sub_offset(sd ? sd->data : NULL, sd ? sd->size : 0, plane);
}

/* The subtitle frame as one eye will see it: at eye size, in the working
 * format. Frames already of the right size and format - the ordinary case,
 * since a subtitle canvas is often sized from the video - are used where they
 * are. When a resize is needed, one canvas the size of an eye is kept and
 * re-filled; the subtitle input itself is never written to. */
static const AVFrame *scale_to_eye(AVFilterContext *ctx, const AVFrame *sub,
                                   int eye_w, int eye_h)
{
    MVCSubDepthContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;

    if (sub->width == eye_w && sub->height == eye_h &&
        sub->format == outlink->format)
        return sub;

    if (!s->canvas || s->canvas->width != eye_w ||
        s->canvas->height != eye_h || s->canvas->format != outlink->format) {
        av_frame_free(&s->canvas);
        s->canvas = ff_get_video_buffer(outlink, eye_w, eye_h);
        if (!s->canvas)
            return NULL;
    }

    if (!s->scale_logged) {
        av_log(ctx, AV_LOG_VERBOSE, "subtitle input %dx%d scaled to the eye "
               "%dx%d\n", sub->width, sub->height, eye_w, eye_h);
        s->scale_logged = 1;
    }

    ret = av_frame_make_writable(s->canvas);
    if (ret < 0)
        return NULL;

    /* Both sides are the filter's one working format, so the only thing to
     * tell the scaler about is how to treat the edges of a resampled caption:
     * bilinear, and no chroma question to answer. */
    s->sws = sws_getCachedContext(s->sws, sub->width, sub->height, sub->format,
                                  eye_w, eye_h, outlink->format,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!s->sws) {
        if (!s->scale_warned) {
            av_log(ctx, AV_LOG_WARNING, "Cannot scale the subtitle input from "
                   "%dx%d to the eye %dx%d; leaving those frames unchanged\n",
                   sub->width, sub->height, eye_w, eye_h);
            s->scale_warned = 1;
        }
        return NULL;
    }

    ret = sws_scale(s->sws, (const uint8_t * const *)sub->data, sub->linesize,
                    0, sub->height, s->canvas->data, s->canvas->linesize);
    if (ret <= 0) {
        if (!s->scale_warned) {
            av_log(ctx, AV_LOG_WARNING, "Scaling the subtitle input from %dx%d "
                   "to the eye %dx%d failed; leaving those frames unchanged\n",
                   sub->width, sub->height, eye_w, eye_h);
            s->scale_warned = 1;
        }
        return NULL;
    }

    return s->canvas;
}

static void blend_row(uint8_t *dst, const uint8_t *src, int npix)
{
    for (int i = 0; i < npix; i++) {
        unsigned a = src[3];

        if (a) {
            if (a < 255) {
                for (int k = 0; k < PIX_STEP; k++)
                    dst[k] = (uint8_t)((src[k] * a + dst[k] * (255 - a) + 127) / 255);
            } else {
                for (int k = 0; k < PIX_STEP; k++)
                    dst[k] = src[k];
            }
        }
        dst += PIX_STEP;
        src += PIX_STEP;
    }
}

/* Burn one eye's window of the subtitle canvas into one half of the frame.
 *
 * A window is identified by the column of the padded canvas it starts at: the
 * canvas with SUB_MARGIN transparent columns added on each side, which is what
 * ff_mvc_sub_window_start() counts in. Those columns are never allocated; a
 * window reaching into them simply finds the canvas short, and the columns it
 * covers beyond the canvas stay as the picture already was. That is exactly
 * the picture of cropping a padded canvas, and the same clamping, at a fraction
 * of the memory. */
static void blend_window(AVFrame *dst, int dst_x0, const AVFrame *src,
                         int start, int eye_w, int eye_h)
{
    int sx0 = FFMAX(0, start - SUB_MARGIN);
    int sx1 = FFMIN(src->width, start - SUB_MARGIN + eye_w);
    int dx0 = dst_x0 + sx0 + SUB_MARGIN - start;
    int y;

    av_assert2(dx0 >= dst_x0 && dx0 + (sx1 - sx0) <= dst_x0 + eye_w);
    if (sx1 <= sx0)
        return;

    for (y = 0; y < eye_h; y++) {
        const uint8_t *s = src->data[0] + (ptrdiff_t)y * src->linesize[0] +
                           (ptrdiff_t)sx0 * PIX_STEP;
        uint8_t *d = dst->data[0] + (ptrdiff_t)y * dst->linesize[0] +
                     (ptrdiff_t)dx0 * PIX_STEP;

        blend_row(d, s, sx1 - sx0);
    }
}

/* Burn the covering epoch's caption into both eyes of one main frame.
 *
 * main is owned by the caller and made writable here; sub is the covering
 * subtitle canvas (owned by the epoch queue), or NULL to leave the frame
 * unchanged.  This is the whole of the filter's work; the pts-keyed decision
 * of WHICH sub (if any) reaches it is made by the activation pump. */
static int place_main(AVFilterContext *ctx, AVFrame *main, const AVFrame *sub)
{
    MVCSubDepthContext *s = ctx->priv;
    const AVFrame *layer;
    int eye_w, eye_h, plane, off;
    int start_l, start_r, ret;

    if (!sub)
        return ff_filter_frame(ctx->outputs[0], main);

    s->nb_frames++;
    eye_w = s->eye_w;
    eye_h = main->height;
    if (eye_w <= 0 || main->height <= 0 || eye_w > main->width / 2) {
        return ff_filter_frame(ctx->outputs[0], main);
    }
    check_marker(s, ctx, main);

    plane = plane_of(s, sub);
    off   = offset_of(s, main, plane);

    layer = scale_to_eye(ctx, sub, eye_w, eye_h);
    if (!layer) {
        return ff_filter_frame(ctx->outputs[0], main);
    }

    /* The filter's only geometry change is the depth displacement.  The window
     * starts at the canvas' own margin unless depth moves it, so the caption
     * keeps the horizontal position painted by sub2video and is not re-centred
     * into the eye. */
    start_l = ff_mvc_sub_window_start(off, FF_MVC_SUB_EYE_LEFT,  SUB_MARGIN);
    start_r = ff_mvc_sub_window_start(off, FF_MVC_SUB_EYE_RIGHT, SUB_MARGIN);

    if (off && (start_l != SUB_MARGIN + ff_mvc_sub_eye_shift(off, FF_MVC_SUB_EYE_LEFT) ||
                start_r != SUB_MARGIN + ff_mvc_sub_eye_shift(off, FF_MVC_SUB_EYE_RIGHT))) {
        if (!s->clamp_logged) {
            av_log(ctx, AV_LOG_WARNING, "depth %+d px exceeds the %d px of "
                   "canvas slack; both eye windows clamp symmetrically at that "
                   "slack, which places that caption nearer the screen than "
                   "authored\n", off, SUB_MARGIN);
            s->clamp_logged = 1;
        }
    }

    av_log(ctx, AV_LOG_DEBUG, "plane %d depth %+d px window %d/%d\n",
           plane, off, start_l, start_r);

    blend_window(main, 0,       layer, start_l, eye_w, eye_h);
    blend_window(main, eye_w,   layer, start_r, eye_w, eye_h);

    s->nb_placed++;
    s->nb_shifted += off != 0;

    ret = ff_filter_frame(ctx->outputs[0], main);
    return ret;
}

static av_cold int init(AVFilterContext *ctx)
{
    MVCSubDepthContext *s = ctx->priv;
    int mode, param;

    if (ff_mvc_sub_depth_parse(s->depth, &mode, &param) < 0) {
        av_log(ctx, AV_LOG_ERROR, "Invalid depth '%s': the accepted values "
               "are auto, flat, 0, 1, shift=<signed pixels> and "
               "plane=<0..31>\n", s->depth ? s->depth : "");
        return AVERROR(EINVAL);
    }
    s->depth_mode  = mode;
    s->depth_param = param;
    /* The pairing pump is bespoke (see the pts-keyed contract above); s->fs is
     * present only to carry the framesync option surface (eof_action and
     * friends), whose default is resolved by the framesync child class and
     * whose normalisation config_output() mirrors. */
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    MVCSubDepthContext *s = ctx->priv;
    AVFrame *m;

    av_log(ctx, AV_LOG_VERBOSE, "%u frames, %u with a subtitle placed, %u of "
           "those displaced per eye\n", s->nb_frames, s->nb_placed,
           s->nb_shifted);

    /* s->fs was never ff_framesync_init()'d, so there is nothing to hand to
     * ff_framesync_uninit(); the queues are ours to drain. */
    for (unsigned i = 0; i < s->sub_q_nb; i++) {
        av_frame_free(&s->sub_q[i]->frame);
        av_freep(&s->sub_q[i]);
    }
    av_freep(&s->sub_q);
    s->sub_q_nb = s->sub_q_cap = 0;
    if (s->main_q) {
        while (av_fifo_can_read(s->main_q) > 0) {
            av_fifo_read(s->main_q, &m, 1);
            av_frame_free(&m);
        }
        av_fifo_freep2(&s->main_q);
    }

    sws_freeContext(s->sws);
    s->sws = NULL;
    av_frame_free(&s->canvas);
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    MVCSubDepthContext *s = ctx->priv;

    s->eye_w = s->eye_width > 0 ? s->eye_width : inlink->w / 2;
    if (inlink->w < 2 || inlink->h < 1 || s->eye_w < 1 ||
        s->eye_w > inlink->w / 2) {
        av_log(ctx, AV_LOG_ERROR, "Video input %dx%d and eye width %d cannot "
               "hold two eyes side by side\n", inlink->w, inlink->h, s->eye_w);
        return AVERROR(EINVAL);
    }

    outlink->w = inlink->w;
    outlink->h = inlink->h;
    outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    outl->frame_rate = inl->frame_rate;

    /* The output rides the video input's clock; main frames reach the pump with
     * pts already in this base, so a held main frame is emitted with its pts
     * unchanged (exactly the identity that the framesync dualinput path used).
     * Canvas pts are brought into this same base at the same rounding the old
     * path used, so stable caption rows land on the same grid as before. */
    s->out_tb = outlink->time_base = inlink->time_base;
    s->in1_tb = ctx->inputs[1]->time_base;
    s->hb_bias = FFMAX(1, av_rescale_q(1, s->in1_tb, s->out_tb));
    s->sub_frontier = AV_NOPTS_VALUE;
    s->sub_eof_pts  = AV_NOPTS_VALUE;
    s->last_out_pts = AV_NOPTS_VALUE;

    /* Mirror what ff_framesync_configure() did to the option before the pump
     * ever sees it (framesync.c:141-148): two INDEPENDENT ifs, not an if/else
     * chain.  The first collapses repeatlast=0 or an explicit eof_action=pass
     * to pass; the second then promotes to endall if shortest is set OR the
     * action already is endall.  Because they are independent, shortest=1 wins
     * over a pass preselection: shortest=1:eof_action=pass and
     * shortest=1:repeatlast=0 both mean endall, exactly as they did before the
     * bespoke pump.  Seeding a local from the spelled action and re-running the
     * same two ifs reproduces that ordering.  Doing it here (rather than
     * calling into framesync, which this filter does not run on) keeps every
     * spelling of the option meaning exactly what it meant. */
    s->eof_action = s->fs.opt_eof_action;
    if (!s->fs.opt_repeatlast || s->eof_action == EOF_ACTION_PASS)
        s->eof_action = EOF_ACTION_PASS;
    if (s->fs.opt_shortest || s->eof_action == EOF_ACTION_ENDALL)
        s->eof_action = EOF_ACTION_ENDALL;
    s->ts_nearest = s->fs.opt_ts_sync_mode > TS_DEFAULT;

    /* AUTO_GROW matters: a fifo allocated without it refuses a write once it is
     * full rather than growing, and this queue is never sized in advance - the
     * point is that it holds whatever the subtitle side has run ahead of the
     * video clock by, which is not known at configuration time. */
    s->main_q = av_fifo_alloc2(0, sizeof(AVFrame *), AV_FIFO_FLAG_AUTO_GROW);
    if (!s->main_q)
        return AVERROR(ENOMEM);

    return 0;
}

/* Hold one arriving canvas frame, inserted in pts order.
 *
 * Its push pts is normalised into the output base with the same rounding the
 * framesync path used.  Insertion (rather than append) matters because the
 * push stream is only nearly monotonic: a canvas cleared at a caption's end pts
 * is pushed when that end is reached, which is normally before anything later
 * arrives but can land behind an earlier push of a different epoch.  Keeping
 * pts order here is what lets trim_covering() and cover_of() read the queue as
 * a display schedule instead of as an arrival log.  Heartbeat re-pushes of the
 * same content advance the watermark but do not enter the queue: they are the
 * subtitle clock catching up, not a new display edge. */
static int hold_canvas(AVFilterContext *ctx, AVFrame *frame)
{
    MVCSubDepthContext *s = ctx->priv;
    MVCSubEpoch *e;
    unsigned i;
    int hb;
    int64_t pts;

    pts = av_rescale_q_rnd(frame->pts, s->in1_tb, s->out_tb,
                           AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    hb = heartbeat_of(frame);
    if (s->sub_frontier == AV_NOPTS_VALUE || pts > s->sub_frontier)
        s->sub_frontier = pts;
    if (hb) {
        /* A heartbeat duplicate only proves that the subtitle side has reached
         * this pts.  It carries a canvas that is already on screen, so it must
         * not become a schedule edge or displace the epoch that is covering. */
        av_frame_free(&frame);
        return 0;
    }

    e = av_mallocz(sizeof(*e));
    if (!e) {
        av_frame_free(&frame);
        return AVERROR(ENOMEM);
    }
    e->frame = frame;
    e->pts = pts;

    /* slots: 8 to start, then geometric - the queue holds one covering epoch,
     * its successor, and whatever the subtitle side has run ahead of the
     * video clock by. */
    if (s->sub_q_nb == s->sub_q_cap) {
        unsigned cap = s->sub_q_cap ? s->sub_q_cap * 2 : 8;
        MVCSubEpoch **t = av_realloc_array(s->sub_q, cap, sizeof(*s->sub_q));
        if (!t) {
            av_frame_free(&e->frame);
            av_freep(&e);
            return AVERROR(ENOMEM);
        }
        s->sub_q = t;
        s->sub_q_cap = cap;
    }
    /* behind the last entry with a pts at or before this one, so that of two
     * pushes sharing a pts the later (the one actually on screen at that
     * instant) is the one that ends up covering */
    for (i = 0; i < s->sub_q_nb && s->sub_q[i]->pts <= e->pts; i++)
        ;
    if (i < s->sub_q_nb)
        memmove(s->sub_q + i + 1, s->sub_q + i,
                (s->sub_q_nb - i) * sizeof(*s->sub_q));
    s->sub_q[i] = e;
    s->sub_q_nb++;
    return 0;
}

/* Drop the held canvases no output frame can ever select again.
 *
 * Output pts only increase, so a canvas is dead as soon as a later canvas
 * starts strictly before the current P: with the queue in pts order, everything
 * up to the last entry starting before P except the head candidate goes.  A
 * later entry at exactly P is the edge itself: this frame is the last one on
 * which the previous epoch can cover, so the trim is deliberately strict and
 * leaves the predecessor in place until the *next* frame.  Called once per
 * emitted frame, so the queue stays at the covering epoch plus whatever the
 * subtitle side has run ahead of the video clock by. */
static void trim_covering(AVFilterContext *ctx, int64_t P)
{
    MVCSubDepthContext *s = ctx->priv;

    while (s->sub_q_nb >= 2 && s->sub_q[1]->pts < P) {
        av_frame_free(&s->sub_q[0]->frame);
        av_freep(&s->sub_q[0]);
        memmove(s->sub_q, s->sub_q + 1, (s->sub_q_nb - 1) * sizeof(*s->sub_q));
        s->sub_q_nb--;
    }
}

/* The canvas that covers output pts P, or NULL for "paint nothing".
 *
 * trim_covering() has run, so the head of sub_q is the last display edge that
 * started strictly before P, if there is one; otherwise it is the first edge at
 * or after P.  The successor, when present, is the edge exactly at P or later.
 * Four rules decide, all of them the framesync behaviour this filter had before
 * the pump, restated on pts:
 *
 *   - nothing has started strictly before P: the frame passes through (framesync
 *     EXT_NULL before the secondary's first frame);
 *   - P is exactly the edge at which a new epoch starts: in the default mode the
 *     successor is not allowed to cover P, so that frame keeps the previous
 *     display state and the new state takes the next grid line;
 *   - eof_action=pass beyond the subtitle side's EOF: no secondary at all;
 *   - ts_sync_mode=nearest: the successor may still win when its start is nearer
 *     to P than the covering epoch's start is (the framesync NEAREST rule for a
 *     non-sync secondary, which took the next frame when it was the closer one). */
static const AVFrame *cover_of(const MVCSubDepthContext *s, int64_t P)
{
    const MVCSubEpoch *e0, *e1 = NULL;

    if (s->sub_q_nb == 0)
        return NULL;
    e0 = s->sub_q[0];
    if (s->sub_q_nb >= 2)
        e1 = s->sub_q[1];

    if (s->ts_nearest && e0->pts < P && e1 &&
        e1->pts - P < P - e0->pts)
        return e1->frame;
    if (e0->pts >= P)
        return NULL;
    if (s->sub_eof && s->eof_action == EOF_ACTION_PASS &&
        s->sub_eof_pts != AV_NOPTS_VALUE && P > s->sub_eof_pts)
        return NULL;
    return e0->frame;
}

/* May the epoch covering P be considered final yet?
 *
 * A canvas that starts strictly after P is the watermark proving that no future
 * push can still land at or before P; subtitle EOF is the other proof, and it
 * ends the schedule outright.  Until one of them arrives the frame waits in
 * main_q - that wait is the fix.
 *
 * The end of the video clock is a third, narrower case.  The heartbeat that
 * drives the subtitle side deliberately stamps itself one subtitle tick
 * EARLIER than the video packet it rides (see sub2video_heartbeat() in
 * fftools), so the watermark from the very last video packet can never satisfy
 * a strict "later than P" test for the final frame.  Once the main input has
 * sent its EOF status, that frame's pts is the last word on the video clock,
 * so accept a watermark within one subtitle tick of it: the held frames then go
 * out with the epoch covering them as of the watermark, instead of being
 * stranded by a bias meant for a stream that has not ended. */
static int epoch_decided(const MVCSubDepthContext *s, int64_t P)
{
    if (s->sub_eof)
        return 1;
    if (s->sub_frontier == AV_NOPTS_VALUE)
        return 0;
    if (s->sub_frontier > P)
        return 1;
    return s->main_eof && s->sub_frontier + s->hb_bias >= P;
}

static int activate(AVFilterContext *ctx)
{
    MVCSubDepthContext *s = ctx->priv;
    AVFilterLink *in0 = ctx->inputs[0];   /* video, the output clock */
    AVFilterLink *in1 = ctx->inputs[1];   /* subtitle canvas */
    AVFilterLink *out = ctx->outputs[0];
    AVFrame *frame = NULL;
    int ret, status;
    int64_t pts;

    FF_FILTER_FORWARD_STATUS_BACK_ALL(out, ctx);

    /* 1. ingest subtitle canvas frames (kept in ascending push pts). */
    while ((ret = ff_inlink_consume_frame(in1, &frame)) > 0) {
        ret = hold_canvas(ctx, frame);
        if (ret < 0)
            return ret;
    }
    if (ret < 0)
        return ret;
    if (!s->sub_eof && ff_inlink_acknowledge_status(in1, &status, &pts)) {
        s->sub_eof = 1;
        s->sub_eof_pts = av_rescale_q_rnd(pts, in1->time_base, s->out_tb,
                                          AV_ROUND_NEAR_INF |
                                          AV_ROUND_PASS_MINMAX);
        /* eof_action=endall (and its synonym shortest=1) ends the whole graph
         * at the secondary's EOF - framesync set both inputs to EXT_STOP and
         * closed the output once the secondary ran out.  Anything still held
         * goes with it, which is the point of the option. */
        if (s->eof_action == EOF_ACTION_ENDALL && !s->out_eof_set) {
            ff_outlink_set_status(out, AVERROR_EOF, s->sub_eof_pts);
            s->out_eof_set = 1;
            return 0;
        }
    }

    /* 2. ingest main frames; they wait in main_q until the subtitle side has
     *    passed their pts (or the subtitle side ends), which is the whole of
     *    the last-arrived race fix. */
    while ((ret = ff_inlink_consume_frame(in0, &frame)) > 0) {
        ret = av_fifo_write(s->main_q, &frame, 1);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret;
        }
    }
    if (ret < 0)
        return ret;
    if (!s->main_eof && ff_inlink_acknowledge_status(in0, &status, &pts)) {
        s->main_eof = 1;
        s->last_out_pts = pts;
    }

    /* 3. emit every held main frame whose covering epoch is decided.  Output
     *    pts increase, so stop at the first frame still short of its
     *    watermark. */
    while (av_fifo_can_read(s->main_q) > 0) {
        AVFrame *main;
        int64_t P;

        av_fifo_peek(s->main_q, &main, 1, 0);
        P = main->pts;
        if (!epoch_decided(s, P))
            break;

        trim_covering(ctx, P);

        av_fifo_drain2(s->main_q, 1);   /* take ownership of the head */
        ret = ff_inlink_make_frame_writable(in0, &main);
        if (ret < 0) {
            av_frame_free(&main);
            return ret;
        }
        ret = place_main(ctx, main, cover_of(s, P));  /* owns and emits main */
        if (ret < 0)
            return ret;
    }

    /* 4. EOF: the video clock drives the output.  The held frames above went
     *    out in step 3 (sub_eof lifts the watermark gate outright, and the end
     *    of the video clock covers the last one). */
    if (!s->out_eof_set && s->main_eof && av_fifo_can_read(s->main_q) == 0) {
        ff_outlink_set_status(out, AVERROR_EOF, s->last_out_pts);
        s->out_eof_set = 1;
        return 0;
    }

    /* 5. requests: advance the subtitle side only as far as the oldest held
     *    main needs (keeps the canvas queue tiny and just-in-time), and keep
     *    the video clock fed.  One request per undecided frame is enough: the
     *    graph will come back to us when the canvas arrives. */
    if (!s->out_eof_set && ff_outlink_frame_wanted(out)) {
        if (av_fifo_can_read(s->main_q) > 0 && !s->sub_eof) {
            AVFrame *main;
            av_fifo_peek(s->main_q, &main, 1, 0);
            if (!epoch_decided(s, main->pts))
                ff_inlink_request_frame(in1);
        }
        if (!s->main_eof)
            ff_inlink_request_frame(in0);
        else if (!s->sub_eof)
            ff_inlink_request_frame(in1);
        return FFERROR_NOT_READY;
    }

    return 0;
}

static const AVFilterPad mvcsubdepth_inputs[] = {
    {
        .name = "video",
        .type = AVMEDIA_TYPE_VIDEO,
    }, {
        .name = "subtitle",
        .type = AVMEDIA_TYPE_VIDEO,
    },
};

static const AVFilterPad mvcsubdepth_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

static const enum AVPixelFormat pix_fmts[] = {
    AV_PIX_FMT_RGBA,
    AV_PIX_FMT_NONE,
};

const FFFilter ff_vf_mvcsubdepth = {
    .p.name        = "mvcsubdepth",
    .p.description = NULL_IF_CONFIG_SMALL("Place a subtitle layer in each eye "
                                          "of a composed stereoscopic frame at "
                                          "authored depth."),
    .p.priv_class  = &mvcsubdepth_class,
    .priv_size     = sizeof(MVCSubDepthContext),
    .preinit       = mvcsubdepth_framesync_preinit,
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    FILTER_INPUTS(mvcsubdepth_inputs),
    FILTER_OUTPUTS(mvcsubdepth_outputs),
    FILTER_PIXFMTS_ARRAY(pix_fmts),
};
