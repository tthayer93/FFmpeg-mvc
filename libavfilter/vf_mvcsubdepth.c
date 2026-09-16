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
#include "libavutil/frame.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
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

/* The value of the shift option that means "the user asked for nothing"; a
 * real shift of INT_MIN pixels is not a thing anyone can ask for. */
#define SHIFT_UNSET INT_MIN

typedef struct MVCSubDepthContext {
    const AVClass *class;
    FFFrameSync fs;

    int plane;      /* depth sequence to read, -1 = ask the subtitle frame */
    int depth;      /* 0 = ignore what was authored, place flat */
    int shift;      /* requested displacement, SHIFT_UNSET when not asked for */
    int eye_width;  /* 0 = take half of the video input's width */

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
} MVCSubDepthContext;

#define OFFSET(x) offsetof(MVCSubDepthContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption mvcsubdepth_options[] = {
    { "plane",  "depth sequence of the subtitle track, -1 for the one it is "
                "marked with", OFFSET(plane),  AV_OPT_TYPE_INT,    { .i64 = -1 },
      -1, 31,        FLAGS },
    { "depth",  "place the caption at the authored depth",          OFFSET(depth),  AV_OPT_TYPE_BOOL, { .i64 = 1 },
      0, 1,          FLAGS },
    { "shift",  "displace the caption by this many pixels per eye, "
                "whatever was authored", OFFSET(shift), AV_OPT_TYPE_INT, { .i64 = SHIFT_UNSET },
      INT_MIN, INT_MAX, FLAGS },
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

/* The depth sequence this frame's caption belongs to: what the user named, or
 * what the subtitle frame is marked with. */
static int plane_of(const MVCSubDepthContext *s, const AVFrame *sub)
{
    const AVFrameSideData *sd;

    if (s->plane >= 0)
        return s->plane;
    sd = av_frame_get_side_data(sub, AV_FRAME_DATA_MVC_SUB_PLANE);
    return ff_mvc_sub_plane(sd ? sd->data : NULL, sd ? sd->size : 0);
}

/* The displacement for one frame, in video pixels and in the authored sign:
 * positive toward the viewer. The user's shift wins over everything; the
 * authored depth wins over flat; nothing at all is flat. */
static int offset_of(const MVCSubDepthContext *s, const AVFrame *main,
                     int plane)
{
    const AVFrameSideData *sd;

    if (s->shift != SHIFT_UNSET)
        return s->shift;
    if (!s->depth)
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

static int place_frame(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    MVCSubDepthContext *s = ctx->priv;
    AVFrame *main, *sub = NULL;
    const AVFrame *layer;
    const AVFrameSideData *sd;
    int eye_w, eye_h, plane, off, centre_x, raw_centre_x, base;
    int start_l, start_r, ret;
    int has_centre;

    ret = ff_framesync_dualinput_get_writable(fs, &main, &sub);
    if (ret < 0)
        return ret;
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
    sd    = av_frame_get_side_data(sub, AV_FRAME_DATA_MVC_SUB_PLANE);
    raw_centre_x = ff_mvc_sub_center_x(sd ? sd->data : NULL, sd ? sd->size : 0);
    has_centre = raw_centre_x != INT_MIN;
    centre_x   = has_centre ? ff_mvc_sub_scale_x(raw_centre_x, sub->width, eye_w)
                            : 0;

    layer = scale_to_eye(ctx, sub, eye_w, eye_h);
    if (!layer) {
        return ff_filter_frame(ctx->outputs[0], main);
    }

    /* ff_mvc_sub_center_x() answers INT_MIN when there is no caption centre to
     * work from; the scaling maps every unusable coordinate to 0, so the
     * flag is carried separately.  The centre is measured on the canvas as the
     * eye sees it; the window starts count the padded canvas, so move the
     * scaled centre into that coordinate system before centring a window on it. */
    if (has_centre)
        centre_x += SUB_MARGIN;
    base = ff_mvc_sub_base_start(has_centre, centre_x, eye_w, SUB_MARGIN);
    start_l = ff_mvc_sub_window_start(base, off, FF_MVC_SUB_EYE_LEFT,  SUB_MARGIN);
    start_r = ff_mvc_sub_window_start(base, off, FF_MVC_SUB_EYE_RIGHT, SUB_MARGIN);

    if (off && (start_l != base + ff_mvc_sub_eye_shift(off, FF_MVC_SUB_EYE_LEFT) ||
                start_r != base + ff_mvc_sub_eye_shift(off, FF_MVC_SUB_EYE_RIGHT))) {
        if (!s->clamp_logged) {
            av_log(ctx, AV_LOG_WARNING, "depth %+d px exceeds the %d px of "
                   "canvas slack; the further eye is clamped, which places "
                   "that caption nearer the screen than authored\n", off,
                   SUB_MARGIN);
            s->clamp_logged = 1;
        }
    }

    av_log(ctx, AV_LOG_DEBUG, "plane %d depth %+d px base %d window %d/%d\n",
           plane, off, base, start_l, start_r);

    blend_window(main, 0,       layer, start_l, eye_w, eye_h);
    blend_window(main, eye_w,   layer, start_r, eye_w, eye_h);

    s->nb_placed++;
    s->nb_shifted += off != 0;

    return ff_filter_frame(ctx->outputs[0], main);
}

static av_cold int init(AVFilterContext *ctx)
{
    MVCSubDepthContext *s = ctx->priv;

    s->fs.on_event = place_frame;
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    MVCSubDepthContext *s = ctx->priv;

    av_log(ctx, AV_LOG_VERBOSE, "%u frames, %u with a subtitle placed, %u of "
           "those displaced per eye\n", s->nb_frames, s->nb_placed,
           s->nb_shifted);

    ff_framesync_uninit(&s->fs);
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
    int ret;

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

    if ((ret = ff_framesync_init_dualinput(&s->fs, ctx)) < 0)
        return ret;

    ret = ff_framesync_configure(&s->fs);
    outlink->time_base = s->fs.time_base;
    return ret;
}

static int activate(AVFilterContext *ctx)
{
    MVCSubDepthContext *s = ctx->priv;

    return ff_framesync_activate(&s->fs);
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
