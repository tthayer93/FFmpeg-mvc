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

/* Subtitle depth of a multiview stream, read back the way a consumer reads it.
 *
 * The crafted two-view fixture (tests/fate/h264-mvc/2view-ofmd.h264) carries the
 * dependent view's offset metadata for the first group of its pictures.  An
 * elementary stream has no schedule of its own - nothing in it says when a
 * picture is to be displayed - so this program supplies the schedule: it cuts the
 * stream at its access unit delimiters, hands each access unit the display time a
 * container would have carried, and prints what every delivered frame then says
 * about its depth.  That table is what the reference file pins.
 *
 * Three deliveries of one stream are the three a consumer can ask for: the
 * dependent view alone, which is the view the metadata is authored in; the
 * composed side-by-side route, which shows both views as one frame and therefore
 * has to answer the depth question for the pair; and the base view alone, which
 * carries none of it.  A frame may also be held by the decoder for several access
 * units after it was decoded, which is why the run states its thread count: the
 * rows a delivery prints must not depend on it.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#include "libavutil/file.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"

/* The access unit delimiter of H.264 (NAL unit type 9). The fixture puts one at
 * the start of every access unit, which is the cut a demuxer makes too. */
#define NAL_AUD 9

/* The display schedule of the fixture: its pictures are one twenty-fifth of a
 * second apart, in 90 kHz units, which is the picture rate the authored block
 * declares and the rate its group length is counted in. */
#define PTS_STEP 3600

/* Offsets of the start codes of the access units of an Annex-B stream, at most
 * max of them; returns how many were found. */
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

/* Where the dependent view's part of an access unit begins: the NAL units of a
 * view other than the base one - its slices and the prefix that introduces them
 * (H.264 types 14, 19 and 20) - and the base view's parameter sets, delimiter and
 * supplementary data travel ahead of that cut. This is the split the demuxers of
 * these files make, and the reason a dependent view picture arrives with no
 * timestamp of its own, which it then adopts from its access unit (see the h264
 * decoder) - the timestamp the subtitle depth of the picture is looked up by. */
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

/* Report one delivered frame the way a consumer reads it: when it is displayed,
 * what it looks like, which view or arrangement it carries, and what it says
 * about its depth. A frame with nothing to say about depth is the answer too -
 * an unknown depth is rendered flat. */
static void print_frame(const char *mode, const AVFrame *frame)
{
    const AVFrameSideData *ss =
        av_frame_get_side_data(frame, AV_FRAME_DATA_MVC_SS_OFFSETS);
    const AVFrameSideData *view =
        av_frame_get_side_data(frame, AV_FRAME_DATA_VIEW_ID);
    const AVFrameSideData *arrangement =
        av_frame_get_side_data(frame, AV_FRAME_DATA_STEREO3D);
    unsigned sequences = 0;

    printf("%s: pts=%lld %dx%d view=%s arrangement=%s depth=",
           mode, (long long)frame->pts, frame->width, frame->height,
           view ? "yes" : "none", arrangement ? "yes" : "none");

    if (ss && ss->size >= 2) {
        sequences = ss->data[0];
        if (sequences > (unsigned)ss->size - 2)
            sequences = (unsigned)ss->size - 2;   /* a row shorter than claimed */
    }
    if (!sequences) {
        printf("none\n");
        return;
    }

    printf("%u covered=%u offsets=", sequences, ss->data[1] & 1);
    for (unsigned i = 0; i < sequences; i++)
        printf("%s%d", i ? " " : "", (int)(int8_t)ss->data[2 + i]);
    printf("\n");
}

/* Deliver every frame the decoder has, in the order it delivers them. */
static int drain(const char *mode, AVCodecContext *avctx, AVFrame *frame,
                 int *delivered)
{
    for (;;) {
        int ret = avcodec_receive_frame(avctx, frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "%s: decode failed: %s\n", mode,
                    av_err2str(ret));
            return ret;
        }

        print_frame(mode, frame);
        (*delivered)++;
        av_frame_unref(frame);
    }
}

int main(int argc, char **argv)
{
    /* the name every line of output carries: which delivery produced it */
    const char *const mode = "h264-ofmd";
    const char       *path     = NULL;
    const char       *view_ids = "";
    uint8_t          *buf      = NULL;
    size_t            cut[64];
    size_t            size     = 0;
    unsigned          units    = 0;
    int               threads  = 1;
    int               delivered = 0;
    int               ret;
    AVCodecContext   *avctx;
    AVFrame          *frame;
    AVPacket         *pkt;
    const AVCodec    *codec;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <annex-b stream> <view ids> [threads]\n",
                argv[0]);
        return 1;
    }
    path     = argv[1];
    view_ids = argv[2];
    threads  = argc > 3 ? atoi(argv[3]) : 1;

    if ((ret = av_file_map(path, &buf, &size, 0, NULL)) < 0) {
        fprintf(stderr, "%s: %s: %s\n", mode, path, av_err2str(ret));
        return 1;
    }
    if (!(units = access_units(buf, size, cut, FF_ARRAY_ELEMS(cut)))) {
        fprintf(stderr, "%s: %s: no access unit delimiter\n", mode, path);
        av_file_unmap(buf, size);
        return 1;
    }

    if (!(codec = avcodec_find_decoder(AV_CODEC_ID_H264)))
        goto fail;
    if (!(avctx = avcodec_alloc_context3(codec)))
        goto fail;
    if (!(frame = av_frame_alloc()) || !(pkt = av_packet_alloc()))
        goto fail;

    /* The options of the delivery a consumer asks for: which views it wants and
     * how much help it gives the decoder. A stream is decoded the same way for
     * all of them, so what differs is only which frames arrive. */
    avctx->thread_count = threads;
    avctx->pkt_timebase = (AVRational){ 1, 90000 };
    if (*view_ids &&
        (ret = av_opt_set(avctx, "view_ids", view_ids,
                          AV_OPT_SEARCH_CHILDREN)) < 0) {
        fprintf(stderr, "%s: view_ids=%s: %s\n", mode, view_ids,
                av_err2str(ret));
        goto fail;
    }
    if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
        fprintf(stderr, "%s: open: %s\n", mode, av_err2str(ret));
        goto fail;
    }

    printf("%s: threads=%d units=%u step=%d\n", mode, threads, units,
           PTS_STEP);

    /* The fragments of each access unit, the way the demuxers of these files hand
     * them over: the display time of a unit travels with its first fragment, and
     * the dependent view's fragment has none, so a picture of that view is dated
     * by adopting its unit's. Subtitle depth is looked up by that display time,
     * which makes the adoption part of what this test reads. The parameter sets
     * ahead of the first delimiter belong to the first access unit, as they do to
     * the file that begins with them. */
    for (unsigned n = 0; n < units; n++) {
        const size_t from = n ? cut[n] : 0;
        const size_t to   = n + 1 < units ? cut[n + 1] : size;
        const size_t dep  = dependent_part(buf, from, to);

        for (int part = 0; part < 2; part++) {
            const size_t begin = part ? dep   : from;
            const size_t end   = part ? to    : dep;

            if (end == begin)          /* an access unit may have no second part */
                continue;

            if ((ret = av_new_packet(pkt, end - begin)) < 0) {
                fprintf(stderr, "%s: packet: %s\n", mode, av_err2str(ret));
                goto fail;
            }
            memcpy(pkt->data, buf + begin, end - begin);
            pkt->pts       = part ? AV_NOPTS_VALUE : (int64_t)n * PTS_STEP;
            pkt->dts       = pkt->pts;
            pkt->time_base = (AVRational){ 1, 90000 };

            ret = avcodec_send_packet(avctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) {
                fprintf(stderr, "%s: access unit %u part %d: %s\n", mode, n,
                        part, av_err2str(ret));
                goto fail;
            }
            if ((ret = drain(mode, avctx, frame, &delivered)) < 0)
                goto fail;
        }
    }

    /* What the schedule did not deliver, the end of stream does: a decoder that
     * holds frames behind its packets sends them out here, and with them the
     * last rows. */
    ret = avcodec_send_packet(avctx, NULL);
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        goto fail;
    if ((ret = drain(mode, avctx, frame, &delivered)) < 0)
        goto fail;

    printf("%s: delivered %d frames\n", mode, delivered);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&avctx);
    av_file_unmap(buf, size);

    /* a run that delivered nothing failed, whatever the decoder reported */
    return delivered ? 0 : 1;

fail:
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&avctx);
    av_file_unmap(buf, size);

    return 1;
}
