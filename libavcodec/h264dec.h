/*
 * H.26L/H.264/AVC/JVT/14496-10/... encoder/decoder
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

#ifndef AVCODEC_H264DEC_H
#define AVCODEC_H264DEC_H

#include "libavutil/mem_internal.h"

#include "cabac.h"
#include "error_resilience.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include "h264_sei.h"
#include "h2645_parse.h"
#include "h264chroma.h"
#include "h264dsp.h"
#include "h264pred.h"
#include "h264qpel.h"
#include "mpegutils.h"
#include "threadframe.h"
#include "videodsp.h"

/* Multiview: the views accumulate an output backlog of one picture per
 * access unit (the decode API delivers one frame per input packet) until
 * stream end, so the DPB must hold a 60 s decode window (~1500 pictures)
 * of pending frames on top of the usual reference frames. */
#define H264_MAX_PICTURE_COUNT 2048

/* Compiling in interlaced support reduces the speed
 * of progressive decoding by about 2%. */
#define ALLOW_INTERLACE

#define FMO 0

/**
 * The maximum number of slices supported by the decoder.
 * must be a power of 2
 */
#define MAX_SLICES 32

#ifdef ALLOW_INTERLACE
#define MB_MBAFF(h)    (h)->mb_mbaff
#define MB_FIELD(sl)  (sl)->mb_field_decoding_flag
#define FRAME_MBAFF(h) (h)->mb_aff_frame
#define FIELD_PICTURE(h) ((h)->picture_structure != PICT_FRAME)
#define LEFT_MBS 2
#define LTOP     0
#define LBOT     1
#define LEFT(i)  (i)
#else
#define MB_MBAFF(h)      0
#define MB_FIELD(sl)     0
#define FRAME_MBAFF(h)   0
#define FIELD_PICTURE(h) 0
#undef  IS_INTERLACED
#define IS_INTERLACED(mb_type) 0
#define LEFT_MBS 1
#define LTOP     0
#define LBOT     0
#define LEFT(i)  0
#endif
#define FIELD_OR_MBAFF_PICTURE(h) (FRAME_MBAFF(h) || FIELD_PICTURE(h))

#ifndef CABAC
#define CABAC(h) (h)->ps.pps->cabac
#endif

#define CHROMA(h)    ((h)->ps.sps->chroma_format_idc)
#define CHROMA422(h) ((h)->ps.sps->chroma_format_idc == 2)
#define CHROMA444(h) ((h)->ps.sps->chroma_format_idc == 3)

#define IS_REF0(a)         ((a) & MB_TYPE_REF0)
#define IS_8x8DCT(a)       ((a) & MB_TYPE_8x8DCT)
#define IS_SUB_8X8(a)      ((a) & MB_TYPE_16x16) // note reused
#define IS_SUB_8X4(a)      ((a) & MB_TYPE_16x8)  // note reused
#define IS_SUB_4X8(a)      ((a) & MB_TYPE_8x16)  // note reused
#define IS_SUB_4X4(a)      ((a) & MB_TYPE_8x8)   // note reused
#define IS_DIR(a, part, list) ((a) & (MB_TYPE_P0L0 << ((part) + 2 * (list))))

// does this mb use listX, note does not work if subMBs
#define USES_LIST(a, list) ((a) & ((MB_TYPE_P0L0 | MB_TYPE_P1L0) << (2 * (list))))

/**
 * Memory management control operation.
 */
typedef struct MMCO {
    MMCOOpcode opcode;
    int short_pic_num;  ///< pic_num without wrapping (pic_num & max_pic_num)
    int long_arg;       ///< index, pic_num, or num long refs depending on opcode
} MMCO;

typedef struct H264Picture {
    AVFrame *f;
    ThreadFrame tf;

    AVFrame *f_grain;

    int8_t *qscale_table_base;        ///< RefStruct reference
    int8_t *qscale_table;

    int16_t (*motion_val_base[2])[2]; ///< RefStruct reference
    int16_t (*motion_val[2])[2];

    uint32_t *mb_type_base;           ///< RefStruct reference
    uint32_t *mb_type;

    /// RefStruct reference for hardware accelerator private data
    void *hwaccel_picture_private;

    int8_t *ref_index[2];   ///< RefStruct reference

    int field_poc[2];       ///< top/bottom POC
    int poc;                ///< frame POC
    int frame_num;          ///< frame_num (raw frame_num from slice header)
    int mmco_reset;         /**< MMCO_RESET set this 1. Reordering code must
                                 not mix pictures before and after MMCO_RESET. */
    int pic_id;             /**< pic_num (short -> no wrap version of pic_num,
                                 pic_num & max_pic_num; long -> long_pic_num) */
    int long_ref;           ///< 1->long term reference 0->short term reference
    int ref_poc[2][2][32];  ///< POCs of the frames/fields used as reference (FIXME need per slice)
    int ref_count[2][2];    ///< number of entries in ref_poc         (FIXME need per slice)
    int mbaff;              ///< 1 -> MBAFF frame 0-> not MBAFF
    int field_picture;      ///< whether or not picture was encoded in separate fields

/**
 * H264Picture.reference has this flag set,
 * when the picture is held for delayed output.
 */
#define DELAYED_PIC_REF  (1 << 2)
    int reference;
    int recovered;          ///< picture at IDR or recovery point + recovery count
    int invalid_gap;
    int sei_recovery_frame_cnt;
    int needs_fg;           ///< whether picture needs film grain synthesis (see `f_grain`)

    const PPS   *pps;

    int mb_width, mb_height;
    int mb_stride;

    /// RefStruct reference; its pointee is shared between decoding threads.
    atomic_int *decode_error_flags;

    int gray;

    /**
     * Multiview (H.264 Annex E) view the picture belongs to: view_id is the
     * stream-wide identifier from the SPS view list, view_idx indexes
     * H264Context.views; both are 0 for non-multiview streams.
     */
    int view_id;
    int view_idx;

    /**
     * Identity token for the DPB slot's current occupant. h264_frame_start()
     * gives every newly allocated picture a fresh, strictly increasing value;
     * ff_h264_unref_picture() zeroes it with the picture tail. It lets
     * cross-access-unit state (for example the output-band duplicate source)
     * prove it still names the same picture rather than a recycled slot.
     */
    uint64_t slot_epoch;

    /**
     * Multiview: set once the picture's frame has been delivered
     * (finalize_frame() succeeded). H264Picture objects are shared between
     * frame-thread worker contexts, so a context sync can alias a
     * committed-but-not-yet-emitted picture into a second context; the
     * flag lets any context retire such a stale alias instead of
     * re-delivering the frame. Must stay after f_grain:
     * ff_h264_unref_picture() zeroes the struct tail from there on, and
     * h264_frame_start() resets it explicitly.
     */
    int output_delivered;

    /**
     * Multiview: set on a queued (or parked) picture that is the reorder
     * tail of a previous POC epoch when a deep POC-epoch wrap is detected
     * (h264_select_output_frame()); the output picker and the park re-emit
     * path deliver flush-flagged pictures in POC order ahead of the new
     * epoch. Cleared on slot release (ff_h264_unref_picture() zeroes the
     * tail from f_grain on) and at frame start. Never set for
     * single-view streams.
     */
    int flush_old_epoch;

    /**
     * 2D+delta MVC output-band fix: the correct base-view output omits
     * this picture (POC watermark untouched); dropped undelivered by
     * h264_select_output_frame(). Set by the first-slice detector in
     * ff_h264_build_ref_list(); multiview only.
     */
    int output_omit;

    /**
     * Output-band fix, dependent half: the correct output contains one
     * extra frame before this picture - a duplicate of output_dup_src,
     * captured at commit (h264_select_output_frame()) and emitted from
     * h264_decode_frame(); output_dup_done latches it once delivered.
     * output_dup_src is NOT marked delivered: it is delivered again at
     * its own reorder slot.
     */
    int output_dup_before;
    int output_dup_done;
    struct H264Picture *output_dup_src;

    /**
     * Output-band fix: identity token of output_dup_src at capture time
     * (H264Picture.slot_epoch). The duplicate source is a raw DPB-slot
     * pointer only because the same access unit normally emits it; when the
     * dependent half is parked for a later packet, that slot may be released
     * and reused before the duplicate is emitted. The token lets the emit
     * site recognize a stale pointer (slot_epoch == 0 after unref, or a
     * different epoch after reuse) instead of duplicating somebody else's
     * picture. Zeroed with the rest of the picture tail.
     */
    uint64_t output_dup_epoch;
} H264Picture;

typedef struct H264Ref {
    uint8_t *data[3];
    int linesize[3];

    int reference;
    int poc;
    int pic_id;

    const H264Picture *parent;
} H264Ref;

/**
 * Per-view decoding state (H.264 Annex E). A non-multiview stream uses a
 * single view (views[0], cur_view == 0) so the code path is identical to
 * plain H.264 decoding.
 */
typedef struct H264ViewState {
    int view_id;                  ///< stream-wide view id, -1 if not yet registered

    H264POCContext poc;
    H264Ref default_ref[2];

    H264Picture *short_ref[32];   ///< short term reference frames
    int short_ref_count;          ///< number of actual short term references
    H264Picture *long_ref[32];    ///< long term reference frames
    int long_ref_count;           ///< number of actual long term references

    /* Multiview: per-view decode-order backlog of decoded, not-yet-
     * committed pictures (bounded by the reorder depth). Committed
     * pictures are parked in parked_pic, not re-queued here. */
    H264Picture *delayed_pic[H264_MAX_PICTURE_COUNT];
    int last_pocs[H264_MAX_DPB_FRAMES];

    H264Picture *next_output_pic; ///< picture committed for delayed output
    H264Picture *parked_pic;      ///< committed picture waiting to be re-emitted
    int next_outputed_poc;
} H264ViewState;

typedef struct H264SliceContext {
    const struct H264Context *h264;
    GetBitContext gb;
    int nal_size;               ///< byte length of this slice's NAL (full
                                 ///< buffer extent; see the dependent-CABAC
                                 ///< copy bound in decode_slice())
    ERContext *er;

    int slice_num;
    int slice_type;
    int slice_type_nos;         ///< S free slice type (SI/SP are remapped to I/P)
    int slice_type_fixed;

    int qscale;
    int chroma_qp[2];   // QPc
    int qp_thresh;      ///< QP threshold to skip loopfilter
    int last_qscale_diff;

    // deblock
    int deblocking_filter;          ///< disable_deblocking_filter_idc with 1 <-> 0
    int slice_alpha_c0_offset;
    int slice_beta_offset;

    H264PredWeightTable pwt;

    int prev_mb_skipped;
    int next_mb_skipped;

    int chroma_pred_mode;
    int intra16x16_pred_mode;

    int8_t intra4x4_pred_mode_cache[5 * 8];
    int8_t(*intra4x4_pred_mode);

    int topleft_mb_xy;
    int top_mb_xy;
    int topright_mb_xy;
    int left_mb_xy[LEFT_MBS];

    int topleft_type;
    int top_type;
    int topright_type;
    int left_type[LEFT_MBS];

    const uint8_t *left_block;
    int topleft_partition;

    unsigned int topleft_samples_available;
    unsigned int top_samples_available;
    unsigned int topright_samples_available;
    unsigned int left_samples_available;

    ptrdiff_t linesize, uvlinesize;
    ptrdiff_t mb_linesize;  ///< may be equal to s->linesize or s->linesize * 2, for mbaff
    ptrdiff_t mb_uvlinesize;

    int mb_x, mb_y;
    int mb_xy;
    int resync_mb_x;
    int resync_mb_y;
    unsigned int first_mb_addr;
    // index of the first MB of the next slice
    int next_slice_idx;
    int mb_skip_run;
    int is_complex;

    int picture_structure;
    int mb_field_decoding_flag;
    int mb_mbaff;               ///< mb_aff_frame && mb_field_decoding_flag

    int redundant_pic_count;

    /**
     * number of neighbors (top and/or left) that used 8x8 dct
     */
    int neighbor_transform_size;

    int direct_spatial_mv_pred;
    int col_parity;
    int col_fieldoff;

    int cbp;
    int top_cbp;
    int left_cbp;

    int dist_scale_factor[32];
    int dist_scale_factor_field[2][32];
    int map_col_to_list0[2][16 + 32];
    int map_col_to_list0_field[2][2][16 + 32];

    /**
     * num_ref_idx_l0/1_active_minus1 + 1
     */
    unsigned int ref_count[2];          ///< counts frames or fields, depending on current mb mode
    unsigned int list_count;
    H264Ref ref_list[2][48];        /**< 0..15: frame refs, 16..47: mbaff field refs.
                                         *   Reordered version of default_ref_list
                                         *   according to picture reordering in slice header */
    struct {
        uint8_t op;
        uint32_t val;
    } ref_modifications[2][32];
    int nb_ref_modifications[2];

    /**
     * Multiview (Annex E) inter-view reference list reordering state
     * (modification_of_pic_nums_idc 4/5, see 8.2.4.2.3 as modified by E.2.1).
     */
    int mvc_anchor;         ///< slice NAL anchor_pic_flag (MV extension)
    int mvc_view_cur[2];    ///< index cursor per list, starts at -1
    int degenerate;         ///< slice header was non-conformant
                            ///< (delta-only anchor slices); the picture
                            ///< is dropped instead of decoded

    unsigned int pps_id;

    const uint8_t *intra_pcm_ptr;

    uint8_t *bipred_scratchpad;
    uint8_t *edge_emu_buffer;
    uint8_t (*top_borders[2])[(16 * 3) * 2];
    int bipred_scratchpad_allocated;
    int edge_emu_buffer_allocated;
    int top_borders_allocated[2];

    /**
     * non zero coeff count cache.
     * is 64 if not available.
     */
    DECLARE_ALIGNED(8, uint8_t, non_zero_count_cache)[15 * 8];

    /**
     * Motion vector cache.
     */
    DECLARE_ALIGNED(16, int16_t, mv_cache)[2][5 * 8][2];
    DECLARE_ALIGNED(8,  int8_t, ref_cache)[2][5 * 8];
    DECLARE_ALIGNED(16, uint8_t, mvd_cache)[2][5 * 8][2];
    uint8_t direct_cache[5 * 8];

    DECLARE_ALIGNED(8, uint16_t, sub_mb_type)[4];

    /// as a DCT coefficient is int32_t in high depth, we need to reserve twice the space.
    DECLARE_ALIGNED(16, int16_t, mb)[16 * 48 * 2];
    DECLARE_ALIGNED(16, int16_t, mb_luma_dc)[3][16 * 2];
    /// as mb is addressed by scantable[i] and scantable is uint8_t we can either
    /// check that i is not too large or ensure that there is some unused stuff after mb
    int16_t mb_padding[256 * 2];

    uint8_t (*mvd_table[2])[2];

    /**
     * Cabac
     */
    CABACContext cabac;
    uint8_t cabac_state[1024];
    int cabac_init_idc;

    MMCO mmco[H264_MAX_MMCO_COUNT];
    int  nb_mmco;
    int explicit_ref_marking;

    int frame_num;
    int idr_pic_id;
    int poc_lsb;
    int delta_poc_bottom;
    int delta_poc[2];
    int curr_pic_num;
    int max_pic_num;
} H264SliceContext;

/**
 * H264Context
 */
typedef struct H264Context {
    const AVClass *class;
    AVCodecContext *avctx;
    VideoDSPContext vdsp;
    H264DSPContext h264dsp;
    H264ChromaContext h264chroma;
    H264QpelContext h264qpel;

    H264Picture DPB[H264_MAX_PICTURE_COUNT];
    H264Picture *cur_pic_ptr;
    H264Picture cur_pic;
    H264Picture last_pic_for_ec;

    H264SliceContext *slice_ctx;
    int            nb_slice_ctx;
    int            nb_slice_ctx_queued;

    H2645Packet pkt;

    /** dts of the packet currently being decoded. Multiview: captured
     *  into each picture at frame start so pictures keep the dts of
     *  their access unit. Single view: assigned to the delivered frame
     *  in finalize_frame(), reproducing the baseline
     *  frame->pkt_dts = pkt->dts behavior (suppressed by
     *  FF_CODEC_CAP_SETS_PKT_DTS in the core) */
    int64_t pkt_dts;

    /** dts of the last input packet with a valid dts; keeps the latched
     *  pkt_dts monotonic across non-monotonic demuxer dts; multiview
     *  only */
    int64_t last_in_dts;

    /** dts of the last delivered frame; output dts must be strictly
     *  increasing for the muxer, while two views of one access unit
     *  (and the fields of a frame-coded picture) share one dts;
     *  multiview only */
    int64_t last_out_dts;

    /** Shared multiview delivery watermark: one int64 instance
     *  (AV_NOPTS_VALUE until first claim) that every delivery claims from,
     *  since delivery order under frame threading is not decode order and
     *  a late-delivered picture could otherwise carry a lower pkt_dts than
     *  one delivered before it. Canonically stored in the first decoding
     *  context (ff_thread_shared_priv_data); must NOT be copied by
     *  ff_h264_update_thread_context() - the copies keep NULL and resolve
     *  through the helper; the owning context (mvc_out_dts_owned) frees
     *  the instance (h264_decode_end). NULL until the first multiview
     *  delivery */
    int64_t *mvc_out_dts;
    int      mvc_out_dts_owned;

    /** Base-view POC and timestamps of the current access unit, latched
     *  in h264_field_start() once the base view's POC is finalized.
     *  Scalars, not an H264Picture pointer: DPB slots are reused as soon
     *  as unreferenced, so a held pointer can silently turn into a
     *  different picture. Not cleared per packet: the base and dependent
     *  views of one access unit may arrive in consecutive packets, so the
     *  latch must survive until the dependent view's field start. Used by
     *  h264_adopt_base_view_poc() (frame_num matching alone is unreliable
     *  there: the dependent NALs' frame_num runs offset from the base) */
    int au_base_poc;
    int au_base_field_poc[2];
    int64_t au_base_pts;
    int64_t au_base_pkt_dts;
    int au_base_valid;

    /**
     * Display-ordinal pairing FIFO for the allviews-composed (native SBS)
     * output: the k-th output picture of the base view pairs with
     * the k-th output picture of the dependent view. One pending queue per
     * compose role ([0] = base view half, [1] = dependent view half) of
     * committed pictures pinned with DELAYED_PIC_REF while held; heads pop
     * together as soon as both queues hold a picture (see
     * h264_sbs_process()). Plain context-local fields: composing runs with
     * frame threading turned off (ff_h264_allviews_composition()), so
     * a single context ever touches them; they are NOT shared and NOT
     * synced by ff_h264_update_thread_context() (which copies field by
     * field, so they never travel), and they key on nothing but queue order
     * (never a DPB slot index or a POC). A queued half has already left its
     * view's delayed output queue, so the queues are held state in their own
     * right: ff_h264_pic_held_for_compose() reports them to the reference
     * maintenance that keeps a pending picture's pin alive. Cleared - pins
     * retired first - by ff_h264_flush_change(): a seek drops the pending
     * halves.
     */
#define H264_SBS_PAIR_Q_SIZE 32
    H264Picture *sbs_pair_q[2][H264_SBS_PAIR_Q_SIZE];
    int sbs_pair_head[2];         ///< ring read position per role
    int sbs_pair_count[2];        ///< queued halves per role

    /** One-shot warning flag for unpaired composed deliveries (the pairing
     *  queue overflow and the end-of-stream leftover shipment). */
    int sbs_pair_unpaired_warned;

    /** Trip counter for the rate-limited composed-pair pts-delta warning. */
    int sbs_pair_delta_warnings;

    /** Trip counter for standalone composed-half deliveries. The composed
     *  output normally pairs every base half with a dependent half; a half
     *  that goes out on its own (duplicate-predecessor emission, pairing
     *  queue overflow, failed assembly, or end-of-stream leftovers) is
     *  counted here so the failure is visible after the first occurrence. */
    int sbs_standalone_emitted;

    /** Trip counter for duplicate-predecessor sources whose DPB slot was
     *  recycled before the output-band picture reached its emission slot.
     *  Rate-limited: first ten individually, then every hundredth. */
    int sbs_dup_stale;

    /** Monotonic token generator for H264Picture.slot_epoch. Not synced:
     *  composing with cross-context state requires a serialized pipeline, and
     *  the output-band duplicate capture/emit sites run in the same
     *  allviews decode context. */
    uint64_t pic_slot_epoch;

    /** One-shot warning flag for an allviews decode that is composing while
     *  frame-threaded (a view selection made after the decoder was opened;
     *  the runtime path of h264_post_receive_frame()). Latched in the
     *  canonical context, so the main-thread warning fires once per decode
     *  session. */
    int sbs_threaded_compose_warned;

    int pixel_shift;    ///< 0 for 8-bit H.264, 1 for high-bit-depth H.264

    /* coded dimensions -- 16 * mb w/h */
    int width, height;
    int chroma_x_shift, chroma_y_shift;

    int droppable;

    int context_initialized;
    int flags;
    int workaround_bugs;
    int x264_build;
    /* Set when slice threading is used and at least one slice uses deblocking
     * mode 1 (i.e. across slice boundaries). Then we disable the loop filter
     * during normal MB decoding and execute it serially at the end.
     */
    int postpone_filter;

    /*
     * Set to 1 when the current picture is IDR, 0 otherwise.
     */
    int picture_idr;

    /*
     * Set to 1 when the current picture contains only I slices, 0 otherwise.
     */
    int picture_intra_only;

    int crop_left;
    int crop_right;
    int crop_top;
    int crop_bottom;

    int8_t(*intra4x4_pred_mode);
    H264PredContext hpc;

    uint8_t (*non_zero_count)[48];

#define LIST_NOT_USED -1 // FIXME rename?

    /**
     * block_offset[ 0..23] for frame macroblocks
     * block_offset[24..47] for field macroblocks
     */
    int block_offset[2 * (16 * 3)];

    uint32_t *mb2b_xy;  // FIXME are these 4 a good idea?
    uint32_t *mb2br_xy;
    int b_stride;       // FIXME use s->b4_stride

    uint16_t *slice_table;      ///< slice_table_base + 2*mb_stride + 1

    // interlacing specific flags
    int mb_aff_frame;
    int picture_structure;
    int first_field;

    uint8_t *list_counts;               ///< Array of list_count per MB specifying the slice type

    /* 0x100 -> non null luma_dc, 0x80/0x40 -> non null chroma_dc (cb/cr), 0x?0 -> chroma_cbp(0, 1, 2), 0x0? luma_cbp */
    uint16_t *cbp_table;

    /* chroma_pred_mode for i4x4 or i16x16, else 0 */
    uint8_t *chroma_pred_mode_table;
    uint8_t (*mvd_table[2])[2];
    uint8_t *direct_table;

    uint8_t scan_padding[16];
    uint8_t zigzag_scan[16];
    uint8_t zigzag_scan8x8[64];
    uint8_t zigzag_scan8x8_cavlc[64];
    uint8_t field_scan[16];
    uint8_t field_scan8x8[64];
    uint8_t field_scan8x8_cavlc[64];
    uint8_t zigzag_scan_q0[16];
    uint8_t zigzag_scan8x8_q0[64];
    uint8_t zigzag_scan8x8_cavlc_q0[64];
    uint8_t field_scan_q0[16];
    uint8_t field_scan8x8_q0[64];
    uint8_t field_scan8x8_cavlc_q0[64];

    int mb_y;
    int mb_height, mb_width;
    int mb_stride;
    int mb_num;

    // =============================================================
    // Things below are not used in the MB or more inner code

    int nal_ref_idc;
    int nal_unit_type;

    int has_slice;          ///< slice NAL is found in the packet, set by decode_nal_units, its state does not need to be preserved outside h264_decode_frame()

    /**
     * Used to parse AVC variant of H.264
     */
    int is_avc;           ///< this flag is != 0 if codec is avc1
    int nal_length_size;  ///< Number of bytes used for nal length (1, 2 or 4)

    int bit_depth_luma;         ///< luma bit depth from sps to detect changes
    int chroma_format_idc;      ///< chroma format from sps to detect changes

    H264ParamSets ps;

    /**
     * Multiview (H.264 Annex E) state.
     */
    const SPS *mvc_sps;   ///< RefStruct reference to the first SPS carrying mvc_sps_data
    int mv_view_id;       ///< view_id of the last seen slice extension NAL, or -1

    /*
     * View selection / export options, see h264_options in h264dec.c.
     * The nb_xxx fields must directly follow the respective array
     * pointers, that is how libavutil's array option machinery stores
     * the element count.
     */
    /** Array of view IDs that should be decoded and output */
    int *view_ids;
    unsigned nb_view_ids;
    /** Array of the available view IDs (exported) */
    unsigned *view_ids_available;
    unsigned nb_view_ids_available;
    /**
     * Array of the view positions for view_ids_available (exported).
     * Not populated: H.264 MVC carries no view position information.
     */
    unsigned *view_pos_available;
    unsigned nb_view_pos_available;

    uint16_t *slice_table_base;

    /**
     * Multiview view state. views[0] is the base view; for non-multiview
     * streams view_count is 1 and cur_view is always 0.
     */
    H264ViewState views[H264_MAX_MVC_VIEWS];
    int view_count;          ///< number of registered views
    int cur_view;            ///< index into views[] of the picture being decoded

    /**
     * Access-unit latch set when a picture is dropped because a view it
     * references is missing (delta-only streams); the unit's remaining
     * slice NALs are consumed without parsing. Reset per packet by
     * decode_nal_units().
     */
    int drop_view_slices;

    /** One-shot warning flag for the drop above. */
    int mvc_missing_view_warned;

    /** One-shot warning flag for the hardware-acceleration software fallback. */
    int mvc_hw_fallback_warned;

    /** Per-packet latch set when a VCL NAL is skipped because its view is
     *  not user-selected (view_ids option); lets the "no frame!" check in
     *  h264_decode_frame() consume fully-deselected packets. Reset per
     *  packet by decode_nal_units(). */
    int view_sel_skipped;

    /**
     * Dependent-view "2D+delta" anchor completion (see h264_slice.c):
     * first macroblock address of the region completed as skipped
     * macroblocks, so error resilience accounts for it once per picture;
     * -1 when not filled. Reset per picture by h264_frame_start().
     */
    int dep_fill_first;

    int poc_offset;         ///< PicOrderCnt_offset from SMPTE RDD-2006

    /**
     * memory management control operations buffer.
     */
    MMCO mmco[H264_MAX_MMCO_COUNT];
    int  nb_mmco;
    int mmco_reset;
    int explicit_ref_marking;

    /**
     * @name Members for slice based multithreading
     * @{
     */
    /**
     * current slice number, used to initialize slice_num of each thread/context
     */
    int current_slice;

    /** @} */

    /**
     * Complement sei_pic_struct
     * SEI_PIC_STRUCT_TOP_BOTTOM and SEI_PIC_STRUCT_BOTTOM_TOP indicate interlaced frames.
     * However, soft telecined frames may have these values.
     * This is used in an attempt to flag soft telecine progressive.
     */
    int prev_interlaced_frame;

    /**
     * Are the SEI recovery points looking valid.
     */
    int valid_recovery_point;

    /**
     * recovery_frame is the frame_num at which the next frame should
     * be fully constructed.
     *
     * Set to -1 when not expecting a recovery point.
     */
    int recovery_frame;

/**
 * We have seen an IDR, so all the following frames in coded order are correctly
 * decodable.
 */
#define FRAME_RECOVERED_IDR  (1 << 0)
/**
 * Sufficient number of frames have been decoded since a SEI recovery point,
 * so all the following frames in presentation order are correct.
 */
#define FRAME_RECOVERED_SEI  (1 << 1)
/**
 * Recovery point detected by heuristic
 */
#define FRAME_RECOVERED_HEURISTIC  (1 << 2)

    /**
     * Initial frame has been completely recovered.
     *
     * Once this is set, all following decoded as well as displayed frames will be marked as recovered
     * If a frame is marked as recovered frame_recovered will be set once this frame is output and thus
     * all subsequently output fraames are also marked as recovered
     *
     * In effect, if you want all subsequent DECODED frames marked as recovered, set frame_recovered
     * If you want all subsequent DISPLAYED frames marked as recovered, set the frame->recovered
     */
    int frame_recovered;

    int has_recovery_point;

    int missing_fields;

    /* for frame threading, this is set to 1
     * after finish_setup() has been called, so we cannot modify
     * some context properties (which are supposed to stay constant between
     * slices) anymore */
    int setup_finished;

    int cur_chroma_format_idc;
    int cur_bit_depth_luma;
    int16_t slice_row[MAX_SLICES]; ///< to detect when MAX_SLICES is too low

    /* original AVCodecContext dimensions, used to handle container
     * cropping */
    int width_from_caller;
    int height_from_caller;

    int enable_er;
    ERContext er;
    int16_t *dc_val_base;

    H264SEIContext sei;

    struct AVRefStructPool *qscale_table_pool;
    struct AVRefStructPool *mb_type_pool;
    struct AVRefStructPool *motion_val_pool;
    struct AVRefStructPool *ref_index_pool;
    struct AVRefStructPool *decode_error_flags_pool;
    int ref2frm[MAX_SLICES][2][64];     ///< reference to frame number lists, used in the loop filter, the first 2 are for -2,-1

    int non_gray;                       ///< Did we encounter a intra frame after a gray gap frame
    int noref_gray;
    int skip_gray;
} H264Context;

extern const uint16_t ff_h264_mb_sizes[4];

/**
 * Reconstruct bitstream slice_type.
 */
int ff_h264_get_slice_type(const H264SliceContext *sl);

/**
 * Allocate tables.
 * needs width/height
 */
int ff_h264_alloc_tables(H264Context *h);

int ff_h264_decode_ref_pic_list_reordering(H264SliceContext *sl, void *logctx);
int ff_h264_build_ref_list(H264Context *h, H264SliceContext *sl);
void ff_h264_remove_all_refs(H264Context *h);

/**
 * Execute the reference picture marking (memory management control operations).
 */
int ff_h264_execute_ref_pic_marking(H264Context *h);

int ff_h264_decode_ref_pic_marking(H264SliceContext *sl, GetBitContext *gb,
                                   const H2645NAL *nal, void *logctx);

void ff_h264_hl_decode_mb(const H264Context *h, H264SliceContext *sl);
void ff_h264_decode_init_vlc(void);

/**
 * Decode a macroblock
 * @return 0 if OK, ER_AC_ERROR / ER_DC_ERROR / ER_MV_ERROR on error
 */
int ff_h264_decode_mb_cavlc(const H264Context *h, H264SliceContext *sl);

/**
 * Decode a CABAC coded macroblock
 * @return 0 if OK, ER_AC_ERROR / ER_DC_ERROR / ER_MV_ERROR on error
 */
int ff_h264_decode_mb_cabac(const H264Context *h, H264SliceContext *sl);

void ff_h264_init_cabac_states(const H264Context *h, H264SliceContext *sl);

void ff_h264_direct_dist_scale_factor(const H264Context *const h, H264SliceContext *sl);
void ff_h264_direct_ref_list_init(const H264Context *const h, H264SliceContext *sl);
void ff_h264_pred_direct_motion(const H264Context *const h, H264SliceContext *sl,
                                int *mb_type);

void ff_h264_filter_mb_fast(const H264Context *h, H264SliceContext *sl, int mb_x, int mb_y,
                            uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr,
                            unsigned int linesize, unsigned int uvlinesize);
void ff_h264_filter_mb(const H264Context *h, H264SliceContext *sl, int mb_x, int mb_y,
                       uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr,
                       unsigned int linesize, unsigned int uvlinesize);

/*
 * o-o o-o
 *  / / /
 * o-o o-o
 *  ,---'
 * o-o o-o
 *  / / /
 * o-o o-o
 */

/* Scan8 organization:
 *    0 1 2 3 4 5 6 7
 * 0  DY    y y y y y
 * 1        y Y Y Y Y
 * 2        y Y Y Y Y
 * 3        y Y Y Y Y
 * 4        y Y Y Y Y
 * 5  DU    u u u u u
 * 6        u U U U U
 * 7        u U U U U
 * 8        u U U U U
 * 9        u U U U U
 * 10 DV    v v v v v
 * 11       v V V V V
 * 12       v V V V V
 * 13       v V V V V
 * 14       v V V V V
 * DY/DU/DV are for luma/chroma DC.
 */

#define LUMA_DC_BLOCK_INDEX   48
#define CHROMA_DC_BLOCK_INDEX 49

/**
 * Get the chroma qp.
 */
static av_always_inline int get_chroma_qp(const PPS *pps, int t, int qscale)
{
    return pps->chroma_qp_table[t][qscale];
}

int ff_h264_field_end(H264Context *h, H264SliceContext *sl, int in_setup);

/**
 * Multiview: pick the output picture from a view's delayed picture queue
 * (flag-free queues yield exactly the plain lowest-POC entry).
 *
 * @param v view whose queue is scanned (must be non-empty)
 * @return index of the chosen entry in v->delayed_pic
 */
int ff_h264_mv_queue_pick(const H264ViewState *v);

/**
 * Remove all reference frames (short and long term) of one multiview view.
 * ff_h264_remove_all_refs() does this for every registered view.
 */
void ff_h264_remove_view_refs(H264Context *h, int view);

int ff_h264_ref_picture(H264Picture *dst, const H264Picture *src);
int ff_h264_replace_picture(H264Picture *dst, const H264Picture *src);
void ff_h264_unref_picture(H264Picture *pic);

void ff_h264_slice_context_init(H264Context *h, H264SliceContext *sl);

void ff_h264_draw_horiz_band(const H264Context *h, H264SliceContext *sl, int y, int height);

/**
 * Submit a slice for decoding.
 *
 * Parse the slice header, starting a new field/frame if necessary. If any
 * slices are queued for the previous field, they are decoded.
 */
int ff_h264_queue_decode_slice(H264Context *h, const H2645NAL *nal);
int ff_h264_execute_decode_slices(H264Context *h);

/**
 * 2D+delta MVC: at picture end, detect the single contiguous run of
 * macroblocks a truncated dependent slice NAL left undecoded (unaccounted
 * entries in the error resilience state) and complete it as skipped
 * macroblocks against the last slice's list 0 reference (the base view
 * frame of the same POC).
 *
 * Safe to call after every picture: plain H.264 and base views never
 * match the internal gate, and intact pictures leave no unaccounted run.
 * Call before reading h->er.error_occurred: the path clears it when the
 * picture ends fully decoded and accounted.
 *
 * Returns the number of completed macroblocks, 0 when nothing applied.
 */
int ff_h264_complete_truncated_region(H264Context *h);
int h264_view_selected(const H264Context *h, int slot);
int h264_base_view_id(const H264Context *h);
int ff_h264_update_thread_context(AVCodecContext *dst,
                                  const AVCodecContext *src);
int ff_h264_update_thread_context_for_user(AVCodecContext *dst,
                                           const AVCodecContext *src);

void ff_h264_flush_change(H264Context *h);

/**
 * True when the picture is pending in a compose pairing queue, i.e. held for
 * the allviews side-by-side output (see the sbs_pair_q fields and
 * h264_sbs_process()). A queued half has already left its view's delayed
 * output queue, so the reference maintenance that keeps a pending picture
 * pinned has to be told about the queues separately or it will let the slot
 * be released and recycled under the queue. Implemented in h264dec.c.
 */
int ff_h264_pic_held_for_compose(const H264Context *h,
                                 const H264Picture *pic);

/**
 * True while the allviews composed output is the one being produced: exactly
 * two registered views, both selected. That is the view-count and
 * view-selection half of the h264_sbs_should_assemble() gate in h264dec.c
 * (its remaining terms judge the picture itself, which the callers of this
 * helper have no picture for), i.e. the condition under which
 * h264_sbs_process() owns a picture's delivery and pairs it, by display
 * ordinal, with the other view's picture.
 *
 * The output-band repair (see H264Picture.output_omit and .output_dup_before)
 * must stay out of that mode: it retires a base picture undelivered and
 * inserts an out-of-band duplicate ahead of a dependent picture, so both of
 * its moves perturb the per-view display sequences the compose pairing
 * consumes. The pairing has no clock of its own - it never re-syncs after a
 * perturbation - so what it ships afterwards is a pairing shifted against the
 * grid, permanently, and the receiver sees both eyes re-anchor at the mark.
 * A single-view selection (either view alone, or the bare default) is not this
 * predicate and keeps the repair exactly as it was; so does plain H.264.
 */
static inline int h264_compose_active(const H264Context *h)
{
    return h->view_count == 2 &&
           h264_view_selected(h, 0) && h264_view_selected(h, 1);
}

void ff_h264_free_tables(H264Context *h);

void ff_h264_set_erpic(ERPicture *dst, const H264Picture *src);

#endif /* AVCODEC_H264DEC_H */
