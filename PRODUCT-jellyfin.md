# FFmpeg-mvc `jellyfin-8.1` product branch

Product branch that turns the FFmpeg-mvc H.264/MVC work into a drop-in
`ffmpeg` / `ffprobe` build for media servers whose libraries carry
stereoscopic 3D movies encoded as H.264/MVC (Annex E) with a 2D-compatible
base view. It is based on the FFmpeg `release/8.1` line (8.1.2) plus the
fork's MVC decoder, and is intended to be installed as a
binary-compatible replacement wherever an unmodified FFmpeg 8 build is used
today.

## What this branch does

Media-server pipelines invoke `ffmpeg` without view-selection arguments. An
MVC bitstream is one picture per view per access unit, so "decode everything"
is the wrong default for such a pipeline: a plain transcode would receive a
double-width side-by-side (SBS) frame instead of the plain 2D video the
stream is compatible with. This branch therefore changes one default:

- **A bare decode of an H.264/MVC stream yields the base view only** - one
  clean full-size frame per access unit (the same plain 2D video a 2D player
  would show), matching the MV-HEVC decoder's base-layer default.
- **All-views delivery is explicit opt-in.** Set the decoder's `view_ids`
  option to a single `-1` (ffmpeg CLI: `-view_ids -1`, or
  `-map 0:v:view:all`) and a two-view stream is again delivered as one
  native side-by-side frame per access unit, exactly as before the flip.
  Individual eyes remain selectable by ID (`view:<id>` / `view_ids <id>`).

The behavior, the options and the CLI recipes are documented in
`doc/decoders.texi` (h264 section) and `doc/ffmpeg.texi`
("Multiview video (H.264/MVC)"). A one-line `AV_LOG_INFO` notice names the
default once per decoder context (see "Expected log line" under Deployment
notes), so server logs record why the base view was chosen and how to
select views.

Everything else is the release-line behavior: multiview view-list export,
view-ID validation and error handling, view specifiers on `-map`, software
decoding with a warning when hardware acceleration is requested for an MVC
stream, and the CBS parameter-set tests. The only behavioral delta versus
the FFmpeg-mvc release line is the base-view default described above; no
wire formats, demuxers, encoders or muxers change.

An explicit selection always wins over that default, including when it
arrives late. A view specifier names its view from the stream's view list,
which the decoder can only report once it has parsed the stream's multiview
sequence header; a seek into an open-GOP stream can land somewhere that
header has not yet appeared, so such a request is completed shortly after
decoding starts. In earlier builds of this branch the decoder used that
moment to apply the base-view default, and the pictures at the front of the
requested view were dropped for the duration of the delay - so a transcode
started from a seek delivered a window shifted a few frames into the
stream, while the same request through the decoder option (configured before
decoding starts, so never exposed to that moment) delivered the intended
frames. The default no longer occupies that position: it is resolved where a
view is used rather than written into the selection when the multiview
header is adopted, and the tool asks for all views while a request for a
non-base view is in flight. A view specifier and the equivalent decoder
option now deliver the same frames at every seek point checked, as they did
before the default existed; the bare-decode default above is unchanged.

## Build identity

A release string of this branch appears in three places and all three are
the same string: the git tag, the `VERSION` file and the `ffmpeg -version`
banner:

    tag == VERSION == banner

For example, a tag `n8.1.2-mvc1-jf4` is built from a `VERSION` file holding
`n8.1.2-mvc1-jf4` and prints the banner

    ffmpeg version n8.1.2-mvc1-jf4 Copyright (c) 2000-2026 the FFmpeg developers

so the identity of an installed binary can be read from any server log.
The string is built from three parts, plus one optional part:

- `n8.1.2` - the FFmpeg release the build is based on. The leading `n`
  mirrors the shape of upstream's own tags; the server-side version regex
  (see the consumer-contract ledger below) tolerates it.
- `-mvc<F>` (`-mvc1` in the example) - which build of this fork's own code
  sits on that FFmpeg base. It bumps whenever this branch's code changes
  and restarts at `mvc1` when the FFmpeg base moves.
- an optional `.S` directly after it (`n8.1.2-mvc1.1`) - a dot-numbered
  sub-build within one build generation: a further issue of the same
  `-mvc<F>` code, numbered forward from `.1`. The first issue of a
  generation is always written bare (`-mvc1`, never `-mvc1.0`), and the
  three-place identity rule above covers the whole string, sub-build
  included.
- `-jf<N>` (`-jf4` in the example) - which build of the matching
  `jellyfin/jellyfin-ffmpeg` line this drop-in targets and has been
  validated against (there `v8.1.2-4`). This part is a compatibility
  pointer, not an upstream claim and not a parity claim: it moves only
  when their build line moves and this branch realigns to the new build;
  our own code changes bump `mvc`, never `jf`. What it asserts is that
  this build was validated against that build's behavior surface, not
  that it contains that build - the only content taken from their line
  is the short queue listed under "Ported fixes" below, and a `-jf<N>`
  suffix claims nothing beyond it.

Servers that gate on a version number never see the suffixes: the
validation regex in the consumer-contract ledger below reads `8.1.2` out
of the full banner above, while the complete identity remains visible in
every log line.

## Ported fixes

Everything this branch takes from `jellyfin/jellyfin-ffmpeg` is listed
here, one line per item, and each of them is attributed in the commit
that carries it. Their build `v8.1.2-4` keeps its source delta as a
queue of 98 patch files under `debian/patches/` applied at package build
time; a `-jf4` suffix is a pointer to that build as the drop-in target
this branch is validated against and is not a statement that this build
contains that queue. Reading it as blanket parity - "compatible with
v8.1.2-4, therefore carries its fixes" - would be wrong in both
directions: most of their queue (encoder, hardware-filter and packaging
work) has no counterpart here, and their build is not current with
upstream's `release/8.1` maintenance fixes either.

| Their patch file | Taken from | What it does |
|---|---|---|
| `0076-fix-seeking-h264-open-gop-videos-with-d3d11va-on-amd.patch` | jellyfin-ffmpeg v8.1.2-4 | Re-query the pixel format every time the decoder reinitializes, so a seek inside an open GOP no longer keeps a format the hardware backend would not pick. Not an upstream change: it locally reverses an upstream optimization. |
| `0098-backport-a-fix-to-not-stall-sub2video-on-stream-eof.patch` | jellyfin-ffmpeg v8.1.2-4 | Queue a subtitle stream's end-of-stream while the filtergraph does not exist yet, so a burned-in subtitle track that ends early cannot leave the run waiting for it, and report the failure of the replayed call. Not an upstream change. |
| `0090-backport-a-fix-to-use-sw-pix-fmt-in-codec-par-if-set.patch` | upstream `140d708d65f65dbfb10ee44d87964c66554f4373` | Copy `sw_pix_fmt` into codec parameters when it is set, so a hardware pixel format cannot hide the bit depth from muxers. Master-only upstream: `release/8.1` has not taken it, which is why their queue and this branch both need it. |

Two of the three carry their patch name because the work is theirs; the
third is taken from upstream with its own commit id, and their queue is
only how this branch heard about it. What this branch does not take from
that queue is deliberately not itemised: the table above is the whole
list, not a sample of it.

Carrying them changes this branch's code, so it moves the build
identity: the build that ships these ports is `n8.1.2-mvc2-jf4` - `mvc2`
because branch code changed, `-jf4` unchanged because their build line
did not.

## Consumer-contract ledger

Facts about the consuming server that this branch's behavior is calibrated
against, each cited to the server's public source (clone of the default
branch at commit `7c463f5`, 2026-09-05; line numbers re-checked 2026-09-07):

- The server validates the `ffmpeg -version` banner with the anchored regex
  `^ffmpeg version n?((?:[0-9]+\.?)+)`, requires at least version 4.4 and
  sets no maximum version; any banner this branch emits (for example
  `ffmpeg version n8.1.2-mvc1-jf4`) parses as `8.1.2` - the regex tolerates
  the leading `n` and the number match stops at the first `-`, so the
  build suffixes are invisible to the version check - and passes. —
  jellyfin/jellyfin MediaBrowser.MediaEncoding/Encoder/EncoderValidator.cs:211-216
  @ master (verified 2026-09-05/07)
- Feature detection is done by capability list-probes of the binary
  (`-decoders` / `-encoders` at :580-584, `-filters` at :614, `-hwaccels` at
  :471), not by version checks; this branch adds no CLI surface and removes
  no capabilities, so probe results are unchanged. —
  jellyfin/jellyfin MediaBrowser.MediaEncoding/Encoder/EncoderValidator.cs:471,580-614
  @ master (verified 2026-09-07)
- MVC is flagged from the `mvc` filename 3D tag / NFO `<format3d>` value
  (`Set3DFormat` at :201, `mvc` branch at :233), not from stream probing, so
  nothing on the server side depends on the decoder's default; server-side
  MVC transcode handling is currently a plain 2D scale. —
  jellyfin/jellyfin Emby.Server.Implementations/Library/Resolvers/BaseVideoResolver.cs:201-233
  @ master (verified 2026-09-07)
- A custom binary is selected through the `--ffmpeg` CLI option /
  `JELLYFIN_FFMPEG` environment variable (prefix `JELLYFIN_` at
  Jellyfin.Server/Program.cs:366, option at Jellyfin.Server/StartupOptions.cs:56)
  or `<EncoderAppPath>` in `encoding.xml`; `ffprobe` is derived from the
  resolved `ffmpeg` path and must sit next to it. —
  jellyfin/jellyfin MediaBrowser.MediaEncoding/Encoder/MediaEncoder.cs:180-238
  @ master (verified 2026-09-07)
- The official Docker image installs its FFmpeg through the
  `FFMPEG_PACKAGE` build argument and points the server at it via
  `ENV JELLYFIN_FFMPEG`, so replacing the binary at that path (or overriding
  the variable) is the supported seam for a custom build. —
  jellyfin/jellyfin-packaging docker/Dockerfile @ master (verified 2026-09-05)

**Era notes (re-verify these):**

1. Server-side MVC transcode handling is sparse today (metadata-only) and
   may evolve; the two probe-style facts above (validation regex, capability
   probes) should be re-checked against the server source whenever the
   server major version is bumped.
2. Probe metadata captured from early transport-stream test material differs
   from newer material in a few ffprobe stream-level fields; this is
   metadata-only and irrelevant to decode behavior, but re-baseline any
   probe-output expectations accordingly.

## Deployment notes

- **Source.** Build from this public branch (or its release tag) with the
  usual `./configure && make`; no out-of-tree inputs are required.
- **Build identity.** Ship from a tagged release: the tag, the `VERSION`
  file and the banner are one string (e.g. `n8.1.2-mvc1-jf4`, see "Build
  identity" above), the banner satisfies the server's version regex while
  keeping the fork's identity visible in every log.
- **Capabilities.** No option names, decoders, encoders, filters, hwaccels
  or CLI flags are added or removed relative to FFmpeg 8.1.2, so capability
  probes behave identically.
- **Artifact naming.** Ship the result under this project's own identity
  (e.g. `ffmpeg-mvc-n8.1.2-mvc1-jf4-linux64.tar.xz`). Never reuse the
  `jellyfin-ffmpeg*` binary or package names: those identify a different
  build with its own patches and update channel, and overriding a package
  with foreign content breaks the server's upgrade path. If installing next
  to a package-managed FFmpeg, place this build in its own directory (for
  example `/opt/ffmpeg-mvc/`) and point `JELLYFIN_FFMPEG` / `--ffmpeg` at
  it. Keep `ffprobe` in the same directory as `ffmpeg`.
- **Expected log line.** The informational notice naming the base-view
  default and the `view_ids` option is logged once per decoder context,
  not once per stream: a run that probes an input file and then decodes
  it creates two decoder contexts for the same video stream - an `.m2ts`
  input yields two copies of the notice, one from the probe context and
  one from the decode context. That is the designed behavior, not an error.
  The notice names a default that was actually applied: a decode that asked
  for views - including one whose request from a stream specifier is
  completed shortly after decoding starts, which is what a seek into an
  open-GOP stream can do - prints no notice at all.


## License and patents

The FFmpeg codebase is licensed under the LGPLv2.1 (optionally GPLv3); this
branch changes no licensing posture. An open-source software license is not
a patent grant: decoding H.264/MVC may require patent licensing depending on
your jurisdiction and use case.

## Ship ledger

- Ship identity (2026-09-09): the late-adoption fix ships as n8.1.2-mvc2-jf4. The ports-only build previously issued under this name (commit 09a7dcb635) is withdrawn by owner direction; its tag was deleted and the name reused for this combined ship. The mvc counter is shared across lines; see tag messages.

The entry above closes the ports-only era of this branch. What follows is the
containment record of the ship that ends it: this tree now holds the queue the
`-jf4` part of its name points at, so the ledger states that with a count,
names what is not carried, and keeps the two consequences of taking a queue
verbatim - one attribution and one test inconsistency - on the record.

- **jf98 full inclusion (2026-09-09).** `jellyfin/jellyfin-ffmpeg` `v8.1.2-4`
  keeps its source delta as a queue of 98 patch files listed in
  `debian/patches/series`. **94 of those 98 are now landed on this branch as 94
  separate commits** - one patch per commit, in the order of that queue, each
  patch landing as the bytes the queue holds: nothing folded, squashed or
  hand-adapted, so any one of the 94 can be byte-compared against the patch
  file its message names. Each of them carries its own provenance in its commit
  message - an `Origin: jellyfin-ffmpeg v8.1.2-4 patch 00NN` trailer on all of
  them, an `Upstream:` line naming the upstream commit and the class of the
  match wherever one was identified, and a `Provenance:` line where the mapping
  is honestly incomplete rather than simply absent. The landings ran as five
  waves on 2026-09-09, one build per wave on this branch, and the last patch
  commit of the last wave is `b492b43fc1`. The wave is 96 commits wide: those
  94 plus the two named in "Our own two commits" below.
- **The four queue entries this branch does not land (2026-09-09).** Each is a
  decision with a stated reason, not an omission.

  - `0054` - **REFUSED-BY-LICENCE.**
    `0054-add-ac4-decoder-for-atsc-3-0.patch` imports the AC-4 parser and
    decoder from Librempeg verbatim, which makes them GPL-3-or-later code. That
    alone would be a posture question, because the decoder and parser are gated
    behind a `gplv3` configure switch this build never enables. What decides it
    is the rest of the patch: it lifts the float reverse-multiply DSP out of
    `libavutil/float_dsp.c` into a new `libavutil/float_fmul_reverse.c` - under
    the same GPL-3 header - and adds that file to libavutil's **unconditional**
    object list, so any tree holding this patch ships GPL-3-only code inside
    the library this product delivers as LGPL-2.1 or later, whatever
    `./configure` is told. AC-4 is not part of anything this branch exists for.
    The tree is proved free of the patch at the landing: neither file it would
    add exists here, `ac4` appears in none of `configure`,
    `libavcodec/Makefile`, `allcodecs.c` or `parsers.c`, and the upstream form
    of that DSP function is still the one in place. The licensing review of
    2026-09-08 holds the finding, including the route back if AC-4 is ever
    wanted: keep the gated decoder files, restore the small LGPL function to
    `float_dsp.c`, and leave the two GPL-3 files out.
  - `0076`, `0090` and `0098` - **already carried.** These three are contents
    this branch shipped in its ports-only build, listed by name in the "Ported
    fixes" table above; landing their queue entries again would re-apply a
    change the tree already holds. They are recorded as carried rather than
    applied, each with the commit that carries it: `0076` by
    `68f273e87eaaac6362c50fc27799da6c19e0b655`, `0090` by
    `8ca9ad8afc21c08bd2a9c1fae4e4df891b7ea88e`, `0098` by
    `3510cfeef290cbfca71ac1dc1b49c3cb38719f4d`. Each carry was proved four ways
    at the landing: the queue patch does not apply to this tree, the same bytes
    reverse-apply against it, the content is present in the file it names, and
    the carrier commit is an ancestor of this branch.
- **Our own two commits inside the series (2026-09-09).** Landing the queue
  verbatim showed two places where verbatim text needs a line from us. Both
  went in as commits of their own, at the position in the series where the need
  appears, and never as an edit inside somebody else's patch commit.

  - After `0042`: `d0c0aef7f6`, `lavc/x264: keep pixel-format mappings explicit
    after the NV20 repurpose`. That patch repurposes the `AV_PIX_FMT_NV20`
    name - it drops the endian-alias macro and reuses the name for a packed
    4:2:2 10-bit format - and `libx264.c` is not one of the files it edits.
    Left alone, the x264 wrapper would have answered with the semi-planar
    colour-space constant for the packed format and dropped the mapping for the
    semi-planar pair that constant actually describes. Spelling the alias out in
    place of the name keeps the encoder advertising and mapping exactly what it
    did before the repurpose.
  - After `0053`: `avfilter/tonemapx: restore libplacebo attribution`. Their
    `0007` deletes the comment in the OpenCL tone-mapping shader naming where
    its peak/average detection and BT.2390 lineage came from, and the curves of
    the filter that `0053` adds were ported out of that same shader. This
    branch names the origin again at the site that now carries the logic.
    Libplacebo is MIT and porting an algorithm needs no notice; the credit
    costs nothing, and a tree shipping this lineage should say where it came
    from.
- **What carrying `0058` leaves inconsistent (2026-09-09).** `0058` changes
  when the sub2video heartbeat pushes a frame, so a burn-in run can emit
  different frames at the same timestamps, and its patch file rewrites exactly
  one test reference to match: `tests/ref/fate/filter-overlay-dvdsub-2397`. The
  three sub2video references - `sub2video`, `sub2video_basic` and
  `sub2video_time_limited` - it does not touch, so their bookkeeping is still
  the pre-patch one, and one of them was last written by the very upstream
  commit `0058` reverts. Carrying `0058` verbatim carries that unfinished
  bookkeeping with it, which is what a verbatim carry means; this branch does
  not correct it locally, because rewriting those three references here would be
  a hand edit of somebody else's test data and would break the byte-identity
  this ledger rests on. Measured on this build the inconsistency is dormant
  rather than failing: all three tests match their untouched references, and the
  DVD-subtitle burn-in matches the reference `0058` rewrote. It is on the record
  for the day the test set this project gates on (`fate-h264` and `fate-cbs`) is
  widened to the whole FATE suite - if those three ever do report a difference,
  that difference is this carried behavior, not a regression introduced by the
  port.
- **Patents, kept apart from the code licence (2026-09-09).** A code licence is
  not a patent licence, and this wave brings in two codecs that belong in the
  second column. AC-4 - the ATSC 3.0 audio system, whose stream-type detection
  lands with `0084` while its decoder is the entry refused above - sits under a
  patent pool administered by Dolby. The DTS:X detection of `0087` touches
  holdings of Xperi/DTS. The H.264/MVC decoder this branch is built around sits
  in the AVC pool, as it did before this wave, and the HEVC-adjacent work of
  `0033`, `0039` and `0086` in the HEVC pools. None of that changes the
  copyright posture of this public source tree. It does mean that whoever
  deploys a build of this tree into something that decodes or encodes those
  formats needs their own patent analysis, because that obligation follows the
  deployment and not the source host. Wherever this document names a codec,
  read the two columns separately: the code is licensed, the patents are not.
- **What the `-jf4` part of the name claims on this ship (2026-09-09).** "Build
  identity" above says that part is a pointer to a build line and not a parity
  claim, and the "Ported fixes" table is the ledger of the ship for which that
  was the whole story. From this ship there is a second half. This tree now
  holds the applicable whole of their queue: 94 patch commits landed one per
  patch, 3 queue entries already carried, 1 entry refused on licence and named
  with its reason. So `-jf4` states a containment this branch can show, and in
  every other respect it stays what it has always been here - a pointer to
  their build line, unmoved by our own code changes, and not a claim that their
  queue is current with upstream's `release/8.1` maintenance fixes, which this
  branch tracks directly instead. The version string does not move for this
  ship either: by the owner ruling of 2026-09-09, `n8.1.2-mvc2-jf4` is the name
  of the jf4-complete build, and the tag is reminted on this tree at ship time,
  so tag, `VERSION` file and banner state the same identity over the same
  content. `-mvc2` is unchanged and stays true: not one of the wave's 96
  commits touches the fork delta that counter counts, which this branch shares
  with the release lines.

