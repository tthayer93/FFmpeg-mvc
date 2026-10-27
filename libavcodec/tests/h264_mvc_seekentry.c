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

/* What a session that starts mid-stream must deliver, measured against the
 * session that started at the front.
 *
 * The fixture (tests/fate/h264-mvc/2view-ssps-lag.h264) is authored to the
 * shape a seekable two-view disc has: the base view refreshes on a period and
 * re-announces its parameter sets at every group head, while the multiview
 * registration - the subset SPS and the dependent view's picture parameter
 * set - repeats on a far wider period than the group.  One access unit of
 * that stream is therefore a landing point carrying a dependent picture that
 * nothing at the entry has registered yet: the refresh's own dependent slice
 * rides one whole access unit ahead of the registration which alone would
 * make it decodable, and a session opened at that refresh meets the slice
 * first.
 *
 * Three deliveries of the same bytes, which are what a consumer can ask a
 * seek to mean:
 *
 *   cold        the whole stream into one session - a decode from the front.
 *               Its table is the yardstick the entries are read against:
 *               what these bytes are worth.
 *   flush       the stream decoded up to the landing access unit, the decoder
 *               flushed there, and the landing onwards fed to the same
 *               session - the accurate-seek entry, which finds the views
 *               already registered when the flush was done.
 *   seekentry   a fresh session opened at the landing access unit with no
 *               history at all - the fast-seek entry, what a viewer's seek
 *               bar asks for and what a media tool makes of a seek position
 *               given ahead of the input.
 *
 * The entry runs are compared against the cold run's tail in the strictest
 * sense available: same pts, same geometry, same announcement, every pixel.
 * A composed pair and a bare view differ in width; a composed pair and a
 * half-filled frame match in width and separate on pixels - which is why
 * pixels are kept and compared rather than the shape a frame announces.
 * Nothing here names a cure: only the delivery an entry owes is pinned, and
 * the reference file pins that answer, not whichever answer the decoder
 * happens to give today.
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

/* The access unit delimiter of H.264 (NAL unit type 9). The fixture puts one
 * at the start of every access unit, which is the cut a demuxer makes too,
 * and the cut a fast seek lands on. */
#define NAL_AUD 9

/* An IDR slice of the base view (NAL unit type 5). The fixture refreshes the
 * base view on a period; the first refresh after the stream front is this
 * program's landing access unit, the keyframe a container's seek table would
 * carry. */
#define NAL_IDR 5

/* The display schedule of the fixture, as in its sibling fixtures: pictures
 * one twenty-fifth of a second apart in 90 kHz units. The landing access
 * unit keeps the time the stream's own clock gives it in every run, which is
 * what a container does on a seek - it moves where it reads from, not when
 * the pictures it reads fall due. */
#define PTS_STEP 3600

/* The geometry of the fixture's views and of the composed frame the fixture
 * delivers to a consumer that wants both views: the two views side by side.
 * A delivery of bare views is half this width; a half-filled frame is this
 * width and differs from a pair in its pixels alone. */
#define VIEW_W 16
#define VIEW_H 16
#define COMPOSED_W (2 * VIEW_W)

/* The fixture is small on purpose - six access units, refreshes two units
 * apart - and these are more room than any of its tables needs. */
#define MAX_AU 16
#define MAX_FRAMES 32

/* One delivered frame, kept whole: what it announced and every pixel it
 * carried, so that a later run can answer "the same as the front run?" in
 * the strictest sense available. */
typedef struct KeptFrame {
    int64_t pts;
    int width, height;
    int has_view, has_arrangement;
    int pixfmt;
    int luma_left, luma_right;      /* the luma each half of the delivered
                                     * width shows at its corner: the
                                     * fixture paints its views with two
                                     * flat levels, so a frame's halves say
                                     * whether both views are in it, one
                                     * view and a filler, or one view
                                     * alone. -1 marks an absent half. */
    size_t size;                    /* bytes kept: luma and both chroma
                                     * planes at picture size, re-strided */
    uint8_t *pixels;
} KeptFrame;

/* One delivery: its frames and the tallies its summary line prints. */
typedef struct Run {
    const char *name;
    KeptFrame frames[MAX_FRAMES];
    unsigned delivered;
    unsigned composed;              /* frames that announce an arrangement
                                     * and carry both views side by side */
    unsigned rejected;              /* fragments the session refused with
                                     * "invalid data" - what a decoder does
                                     * to a picture it has no right to
                                     * refuse, and the first symptom an
                                     * entry defect leaves on the wire */
} Run;

/* Offsets of the start codes of the access units of an Annex-B stream, at
 * most max of them; returns how many were found. The same cut the sibling
 * decoder test makes, and the one the landing search reads. */
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

/* Where the dependent view's part of an access unit begins: the first NAL
 * unit belonging to a view other than the base one (H.264 types 14, 19 and
 * 20). The base view's delimiter, parameter sets and registration travel
 * ahead of that cut; the demuxers of these files split an access unit here,
 * and the dependent fragment starts without a timestamp of its own. */
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

/* Whether an access unit refreshes the base view. The landing point of a
 * seek is an access unit that does; this program opens its entry runs at the
 * first one past the stream front. */
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

static void frame_clear(KeptFrame *kf)
{
    av_freep(&kf->pixels);
    kf->size = 0;
}

static void run_clear(Run *run)
{
    while (run->delivered > 0)
        frame_clear(&run->frames[--run->delivered]);
    run->composed = 0;
}

static void run_reset(Run *run)
{
    run_clear(run);
    run->rejected = 0;
}

/* Keep a delivered frame whole. Pixels are re-strided to the picture width
 * as they are copied, so that the padding one session may carry and another
 * not can never speak in a later comparison. */
static int keep_frame(Run *run, const AVFrame *frame)
{
    const AVFrameSideData *view =
        av_frame_get_side_data(frame, AV_FRAME_DATA_VIEW_ID);
    const AVFrameSideData *arrangement =
        av_frame_get_side_data(frame, AV_FRAME_DATA_STEREO3D);
    KeptFrame *kf;
    size_t off = 0;

    if (run->delivered >= MAX_FRAMES) {
        fprintf(stderr, "%s: more frames than this program can keep\n",
                run->name);
        return AVERROR(ENOMEM);
    }
    kf = &run->frames[run->delivered];

    kf->pts             = frame->pts;
    kf->width           = frame->width;
    kf->height          = frame->height;
    kf->has_view        = !!view;
    kf->has_arrangement = !!arrangement;
    kf->pixfmt          = frame->format;
    kf->size            = (size_t)frame->width * frame->height +
                          2 * ((size_t)frame->width / 2) *
                             ((size_t)frame->height / 2);
    if (!(kf->pixels = av_malloc(kf->size))) {
        kf->size = 0;
        return AVERROR(ENOMEM);
    }

    for (int y = 0; y < frame->height; y++)
        memcpy(kf->pixels + off + (size_t)y * frame->width,
               frame->data[0] + (size_t)y * frame->linesize[0],
               frame->width);
    off += (size_t)frame->width * frame->height;
    for (int c = 1; c <= 2; c++) {
        for (int y = 0; y < frame->height / 2; y++)
            memcpy(kf->pixels + off + (size_t)y * (frame->width / 2),
                   frame->data[c] + (size_t)y * frame->linesize[c],
                   frame->width / 2);
        off += (size_t)(frame->width / 2) * (frame->height / 2);
    }

    /* The fixture's pictures are flat, so its corner luma is its whole
     * story: two levels, one per view, and nothing else should ever be
     * seen at either corner of a frame as wide as both views. */
    kf->luma_left  = kf->pixels[0];
    kf->luma_right = frame->width >= 2 * VIEW_W
                     ? kf->pixels[frame->width / 2] : -1;

    if (arrangement && frame->width == COMPOSED_W && frame->height == VIEW_H)
        run->composed++;
    run->delivered++;

    return 0;
}

/* What a delivered frame says, in one row. */
static void print_frame(const Run *run, const KeptFrame *kf)
{
    if (kf->luma_right < 0)
        printf("%s: pts=%lld %dx%d view=%s arrangement=%s halves=%d,-\n",
               run->name, (long long)kf->pts, kf->width, kf->height,
               kf->has_view ? "yes" : "none",
               kf->has_arrangement ? "yes" : "none", kf->luma_left);
    else
        printf("%s: pts=%lld %dx%d view=%s arrangement=%s halves=%d,%d\n",
               run->name, (long long)kf->pts, kf->width, kf->height,
               kf->has_view ? "yes" : "none",
               kf->has_arrangement ? "yes" : "none",
               kf->luma_left, kf->luma_right);
}

/* The question a seek bar actually asks of an entry frame: is this what the
 * same bytes are worth from the front? Geometry, announcement and every
 * pixel. Returns 1 with the reason written into detail (when one is
 * wanted), 0 when the frames are one and the same frame. */
static int frame_vs_cold(const KeptFrame *kf, const Run *cold,
                         char *detail, size_t detail_size)
{
    const KeptFrame *want = NULL;

    for (unsigned i = 0; i < cold->delivered; i++)
        if (cold->frames[i].pts == kf->pts) {
            want = &cold->frames[i];
            break;
        }

    if (!want) {
        if (detail)
            snprintf(detail, detail_size,
                     "no frame at this pts from the front");
        return 1;
    }
    if (kf->width           != want->width           ||
        kf->height          != want->height          ||
        kf->pixfmt          != want->pixfmt          ||
        kf->has_view        != want->has_view        ||
        kf->has_arrangement != want->has_arrangement) {
        if (detail)
            snprintf(detail, detail_size,
                     "announces something else than the front's frame: "
                     "front %dx%d pixfmt %d view %s arrangement %s",
                     want->width, want->height, want->pixfmt,
                     want->has_view ? "yes" : "none",
                     want->has_arrangement ? "yes" : "none");
        return 1;
    }
    if (kf->size != want->size) {
        if (detail)
            snprintf(detail, detail_size,
                     "picture of another size (%zu vs %zu bytes)",
                     kf->size, want->size);
        return 1;
    }

    {
        size_t diffs = 0, shown = 0, len = 0;
        char shown_buf[160] = "";

        for (size_t i = 0; i < kf->size; i++) {
            if (kf->pixels[i] == want->pixels[i])
                continue;
            diffs++;
            if (shown < 4) {
                shown++;
                len += (size_t)snprintf(shown_buf + len,
                                        sizeof(shown_buf) - len,
                                        "%s%zu:%02x!=%02x",
                                        shown > 1 ? " " : "",
                                        i, kf->pixels[i], want->pixels[i]);
                if (len >= sizeof(shown_buf) - 1)
                    len = sizeof(shown_buf) - 1;
            }
        }
        if (diffs) {
            if (detail)
                snprintf(detail, detail_size,
                         "%zu byte%s differ from the front's frame at this "
                         "pts (first: %s)",
                         diffs, diffs > 1 ? "s" : "", shown_buf);
            return 1;
        }
    }

    return 0;
}

/* The table of a run. With a cold run to answer to, every frame row carries
 * a second row saying whether it is the frame the front delivers at that
 * time; without one, the row speaks for itself. */
static void print_run(const Run *run, const Run *cold, int ok)
{
    for (unsigned i = 0; i < run->delivered; i++) {
        char reason[256] = "";

        print_frame(run, &run->frames[i]);
        if (cold) {
            int diff = frame_vs_cold(&run->frames[i], cold, reason,
                                     sizeof(reason));

            printf("%s:   ^ %s\n", run->name,
                   diff ? reason
                        : "the frame the front delivered, identical");
        }
    }
    printf("%s: delivered=%u composed=%u rejected=%u result=%s\n", run->name,
           run->delivered, run->composed, run->rejected,
           ok ? "ok" : "FAIL");
}

/* One decoder session, opened the way a consumer opens one to watch a
 * multiview delivery: every view wanted and composed with it, and one thread
 * so that the table is the decoder's answer and not a schedule artifact. */
static AVCodecContext *open_session(const char *who, const AVCodec *codec)
{
    AVCodecContext *avctx;
    int ret;

    if (!(avctx = avcodec_alloc_context3(codec)))
        return NULL;
    avctx->thread_count = 1;
    avctx->pkt_timebase = (AVRational){ 1, 90000 };
    if ((ret = av_opt_set(avctx, "view_ids", "-1",
                          AV_OPT_SEARCH_CHILDREN)) < 0 ||
        (ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        fprintf(stderr, "%s: open: %s\n", who, av_err2str(ret));
        avcodec_free_context(&avctx);
        return NULL;
    }

    return avctx;
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

/* Feed the access units [first, last) to a session, each split at the
 * dependent view's part the way the demuxers of these files split them, and
 * each carrying the time the stream's own clock gives it - which is what
 * makes an entry run comparable against the front run frame for frame.
 * A refreshing access unit is flagged the seek point a container's table
 * would call it. */
static int feed(Run *run, AVCodecContext *avctx, const uint8_t *buf,
                size_t size, const size_t *cut, unsigned units,
                unsigned first, unsigned last, AVPacket *pkt, AVFrame *frame)
{
    for (unsigned n = first; n < last; n++) {
        const size_t from = n ? cut[n] : 0;
        const size_t to   = n + 1 < units ? cut[n + 1] : size;
        const size_t dep  = dependent_part(buf, from, to);

        for (int part = 0; part < 2; part++) {
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

            ret = avcodec_send_packet(avctx, pkt);
            av_packet_unref(pkt);
            if (ret == AVERROR_INVALIDDATA) {
                /* A session that refuses a fragment it was handed records
                 * the refusal here rather than dying by it: the fragment
                 * missing from what follows is the defect, and the tables
                 * below are where it has to be legible. */
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

    /* what the schedule did not deliver, the end of stream does */
    {
        int ret = avcodec_send_packet(avctx, NULL);

        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return -1;
    }
    return drain(run, avctx, frame);
}

static int run_stream(const char *path)
{
    uint8_t       *buf         = NULL;
    size_t         cut[MAX_AU];
    size_t         size        = 0;
    unsigned       units       = 0, landing = 0;
    Run            cold        = { "cold" };
    Run            flushed     = { "flush" };
    Run            seekentry   = { "seekentry" };
    unsigned       lost[3]     = { 0, 0, 0 }; /* frames an entry run lost or
                                               * invented against the front */
    Run           *runs[3]     = { &cold, &flushed, &seekentry };
    int            fails       = 0;
    int            ret;
    AVCodecContext *cold_ctx   = NULL, *flush_ctx = NULL, *entry_ctx = NULL;
    AVFrame       *frame       = NULL;
    AVPacket      *pkt         = NULL;
    const AVCodec *codec;

    if ((ret = av_file_map(path, &buf, &size, 0, NULL)) < 0) {
        fprintf(stderr, "seek: %s: %s\n", path, av_err2str(ret));
        return 1;
    }
    if (!(units = access_units(buf, size, cut, FF_ARRAY_ELEMS(cut)))) {
        fprintf(stderr, "seek: %s: no access unit delimiter\n", path);
        av_file_unmap(buf, size);
        return 1;
    }
    /* the landing: the first refresh past the stream front, which is the
     * first entry a container's seek table would offer */
    for (landing = 1; landing < units; landing++) {
        const size_t to = landing + 1 < units ? cut[landing + 1] : size;

        if (au_refreshes(buf, cut[landing], to))
            break;
    }
    if (landing >= units) {
        fprintf(stderr, "seek: %s: no refresh access unit past the front to "
                        "land an entry on (not a seekable fixture)\n", path);
        av_file_unmap(buf, size);
        return 1;
    }

    if (!(codec = avcodec_find_decoder(AV_CODEC_ID_H264)) ||
        !(frame = av_frame_alloc()) || !(pkt = av_packet_alloc()))
        goto fail;

    printf("seek: threads=1 units=%u landing=%u step=%d\n", units, landing,
           PTS_STEP);

    /* cold: the yardstick run - everything the bytes are worth from the front */
    if (!(cold_ctx = open_session(cold.name, codec)))
        goto fail;
    if (feed(&cold, cold_ctx, buf, size, cut, units, 0, units, pkt, frame) < 0)
        goto fail;

    /* flush: the accurate-seek entry - decoded to the landing, flushed
     * there, fed on from it. What the flush took away is not this run's
     * answer, so its frames are dropped and only what the flush let through
     * is kept and compared. */
    if (!(flush_ctx = open_session(flushed.name, codec)))
        goto fail;
    if (feed(&flushed, flush_ctx, buf, size, cut, units, 0, landing, pkt,
             frame) < 0)
        goto fail;
    avcodec_flush_buffers(flush_ctx);
    run_reset(&flushed);
    if (feed(&flushed, flush_ctx, buf, size, cut, units, landing, units, pkt,
             frame) < 0)
        goto fail;

    /* seekentry: the fast-seek entry - a session with no history opened at
     * the landing itself, seeing these bytes from their first one, which is
     * where a dependent picture can precede the registration that registers
     * its view */
    if (!(entry_ctx = open_session(seekentry.name, codec)))
        goto fail;
    if (feed(&seekentry, entry_ctx, buf, size, cut, units, landing, units,
             pkt, frame) < 0)
        goto fail;

    /* What each delivery owes: the front owes its six frames composed as
     * they are; each entry owes the front's tail - the frames from the
     * landing on, the same ones, to the last pixel. */
    for (int r = 1; r < 3; r++) {
        const Run *run = runs[r];
        const unsigned tail = cold.delivered > landing
                              ? cold.delivered - landing : 0;

        for (unsigned i = 0; i < run->delivered; i++)
            if (frame_vs_cold(&run->frames[i], &cold, NULL, 0))
                lost[r]++;
        lost[r] += tail > run->delivered ? tail - run->delivered   /* lost */
                                         : run->delivered - tail;  /* made up */
    }

    {
        int ok[3];

        ok[0] = cold.delivered == units && cold.composed == cold.delivered &&
                !cold.rejected;
        for (int r = 1; r < 3; r++)
            ok[r] = runs[r]->delivered == (units > landing ? units - landing
                                                           : 0) &&
                    runs[r]->composed == runs[r]->delivered &&
                    !runs[r]->rejected && !lost[r];

        for (int r = 0; r < 3; r++) {
            print_run(runs[r], r ? &cold : NULL, ok[r]);
            if (!ok[r])
                fails++;
        }

        if (!ok[0])
            fprintf(stderr, "ASSERT-FAIL: %s: the front itself misdelivered "
                            "(%u frames, %u of them composed, %u expected, "
                            "%u fragments refused)\n", cold.name,
                    cold.delivered, cold.composed, units, cold.rejected);
        for (int r = 1; r < 3; r++)
            if (!ok[r])
                fprintf(stderr, "ASSERT-FAIL: %s: an entry into the landing "
                                "access unit must deliver the %u composed "
                                "frames the front delivers from it, "
                                "identical; this run delivered %u (%u "
                                "composed, %u not matching the front, %u "
                                "fragments refused)\n", runs[r]->name,
                        units > landing ? units - landing : 0,
                        runs[r]->delivered, runs[r]->composed, lost[r],
                        runs[r]->rejected);
    }

    ret = fails ? 1 : 0;

    for (int r = 0; r < 3; r++)
        run_clear(runs[r]);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&cold_ctx);
    avcodec_free_context(&flush_ctx);
    avcodec_free_context(&entry_ctx);
    av_file_unmap(buf, size);

    return ret;

fail:
    for (int r = 0; r < 3; r++)
        run_clear(runs[r]);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&cold_ctx);
    avcodec_free_context(&flush_ctx);
    avcodec_free_context(&entry_ctx);
    av_file_unmap(buf, size);

    return 1;
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
