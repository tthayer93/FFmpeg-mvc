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
  :471), not by version checks; this branch adds no CLI surface and removes no
  capabilities, so probe results are unchanged - with exactly one exception
  since `n8.1.2-mvc5-jf4`: that build registers one filter, `mvcsubdepth`, so
  `-filters` answers with exactly one entry more than plain FFmpeg 8.1.2 does
  and no other option name, decoder, encoder, filter or hwaccel appears or
  disappears. The extra entry is inert to a list-probe consumer: a filter has
  no effect until a filter graph names it, so no capability the server probes
  for changes state because of it (see the subtitle-depth ship in the ship
  ledger below). —
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
- **Capabilities.** Relative to FFmpeg 8.1.2 exactly one thing is added from
  `n8.1.2-mvc5-jf4` on: the `mvcsubdepth` filter, an opt-in element of a filter
  graph. It is therefore the one new line `-filters` prints, and the rest of
  the capability lists are byte-identical - no option names, decoders,
  encoders, hwaccels or CLI flags are added or removed, and no other filter
  is. Nothing a server does can trip over the extra line: the filter acts only
  inside a graph that names it, so a probe that does not ask for it sees a
  binary that behaves as it always did (see the subtitle-depth ship in the
  ship ledger below).
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
- **Composed display-order pairing ship (2026-09-12).** The composed
  multiview route - the explicit request for every view in one decode,
  answered with one side-by-side picture per output slot - now pairs each
  view's pictures by their output order in context-local state instead of
  keying on decoder state that a frame-threaded decode does not share, and
  it delivers an unpairable half rather than losing it. That fix builds and
  ships here as **`n8.1.2-mvc3-jf4`**, superseding `n8.1.2-mvc2-jf4`. On top
  of the jf98-complete tree recorded above, the delta is exactly two
  commits: the squashed fix `3ccfe7e3c5` and the version commit that names
  the build `n8.1.2-mvc3-jf4`; the release tag for this state is that same
  string, so tag, `VERSION` file and banner state one identity over one
  tree.
- **Why the name moved, and what did not (2026-09-12).** The `mvc` part of
  the name is the revision of the fork's own code delta - one counter,
  shared by every tagged line that carries the delta - and this fix changes
  that delta, so the part moves to 3 here as it does on the release lines
  carrying the same fix. The `-jf4` part does not move: this ship lands no
  entry from the jellyfin-ffmpeg patch queue, so the compatibility line is
  **unchanged - `jellyfin-ffmpeg v8.1.2-4`** - and with it the containment
  recorded above stands whole, including the queue entry refused on licence
  (`0054`: AC-4 is not provided, the finding and the route back are
  unchanged).
- **What this fix does not touch (2026-09-12).** The product default is
  untouched: an invocation that names no views still decodes the base view
  and still logs the base-view notice exactly once, and an explicit view
  selection still beats the default. The fix acts only on the opt-in
  composed route requested with `-view_ids -1`. And the single-view
  invariant holds: every single-view selection on this build decodes
  byte-for-byte identical to the build this ship supersedes - the frozen
  payload rows of the acceptance suite, including a deep-seek dependent-view
  leg and the single-view selections beside the composed route, replayed
  equal on the gated binary at this tip.
- **Docs wave and geometry-stable composition ship (2026-09-15).** This
  tree builds and ships as **`n8.1.2-mvc4-jf4`**, superseding
  `n8.1.2-mvc3-jf4`. On top of the tree recorded above, the delta is
  twelve commits: the five documentation commits, the four commits of the
  decoder change and the merge that carries them (`11b3bd7546`), the
  version commit that names this build, and this entry. The release tag
  for this state is that same string, so tag, `VERSION` file and banner
  state one identity over one tree.
- **The documentation wave (2026-09-15).** Five commits over the README
  and the contributor guide, approved by the owner as one wave: how to
  build this branch and install the result, the everyday multiview usage
  patterns in the shapes people actually run them, the statement that
  hardware acceleration is not available for H.264/MVC - a request for it
  warns and decoding falls back to software - and where a patch belongs,
  which is here for multiview work and upstream for general FFmpeg work.
  Documentation only: no source, test or build file moves with any of the
  five, so what this ledger describes is the build under it.
- **Geometry-stable standalone composition (2026-09-15).** The composed
  route now ships a half it cannot pair the way it ships a paired frame:
  composed into a full double-width frame, that half in the eye position
  its view owns and the opposite half black. Before this change such a
  half went out at single-view width, so a composed run whose pairing
  degraded changed its output width at every unpaired frame, and every
  one of those changes flushed the frames a filter graph or encoder had
  in hand. After it one output geometry holds from the first frame of the
  run to the last: what a viewer sees is the missing eye, what a consumer
  sees is a frame that never changes size. The delivery is deterministic
  and pinned on this tree - generated two-view streams whose view lists
  end one access unit apart, decoded as a whole and eye by eye, with the
  black halves CRC-matching an independently generated black frame. The
  composed route a caller requests before the decoder opens is untouched,
  and every single-view decode stays byte-for-byte frozen.
- **What this ship was measured on (2026-09-15).** Product gate on this
  exact tree: `checkasm` 14903 checks, FATE 343 tests with 0 failures -
  the 336 this branch already gated on plus the 7 new `h264-mvc-uneven-*`
  entries - and the provenance self-test harness at 191 passed, 0 failed.
  On the live composed route, 720 of 720 frames of the reference title
  were delivered over a 30 s run in which the build this ship supersedes
  lost 145, with zero filter-graph reconfigures and one output geometry
  throughout.
- **Compatibility, unchanged (2026-09-15).** The `-jf4` part of the new
  name points where it has always pointed: **`jellyfin-ffmpeg v8.1.2-4`**,
  unmoved, because no update from that build line landed on this ship. The
  part a wave of ours increments is the `mvc` counter, which counts this
  fork's own code delta and moves on every tagged line carrying the same
  change. The containment recorded above therefore stands whole under the
  new number - 94 queue entries landed as individual commits, 3 already
  carried by earlier ports, 1 refused on licence with its finding and its
  route back (`0054`: AC-4 is not provided) - and this ship neither
  restates nor relaxes any of it.
- **Subtitle-depth ship (2026-09-16).** This tree builds and ships as
  **`n8.1.2-mvc5-jf4`**, superseding `n8.1.2-mvc4-jf4`. On top of the tree
  recorded above, the delta is four commits: the port commit that carries the
  feature (`e2932bf84c`), the merge that lands it on this branch
  (`eba4adec65`), the version commit that names this build (`cf0d569920`), and
  this entry. The release tag for this state is that same string, so tag,
  `VERSION` file and banner state one identity over one tree.
- **Subtitle depth for authored multiview subtitle streams (2026-09-16).**
  Where a release authors depth for its subtitles, this build can put them at
  that depth instead of flat on the screen plane. The decoder reads the
  offset-metadata (OFMD) user-data SEI of the dependent view and hands the
  per-frame answer out as frame side data: one signed offset per depth
  sequence, positive toward the viewer, in native picture pixels, attached to
  a dependent-view frame as it leaves the decoder and copied onto the
  assembled side-by-side frame beside its stereo tag. The transcoder reads a
  subtitle track's `3d-plane` / `3d-plane-<lang>` stream metadata tag as the
  sequence that track floats in and stamps the track's rendered frames with
  it; a value that is not a sequence number is treated as absent, not guessed.
  The `mvcsubdepth` filter does the placing: given a composed frame and a
  subtitle rendered to a picture, it burns one copy into each eye at the
  horizontal place that sequence's offset asks for, taking the sequence from
  the track's stamp unless the graph names one, and falling back to a flat
  pair when there is no stamp, no offset or no authored depth at all. All of
  it is opt-in: nothing here displays a subtitle that the caller's own graph
  does not feed to that filter, the depth travels as two additive frame
  side-data types (`libavutil` `60.27.100`) that no caller sees unless it asks
  for them, and a run that names no such graph decodes and transcodes exactly
  as the build this ship supersedes.
- **Composed-view anchor hold (2026-09-16).** The composed route hands a base
  half over to the assembled frame, and until this ship that was the last time
  anything pinned it: the pairing queue had already taken the picture out of
  every output list reference maintenance reads, so it could be released and
  its slot recycled while the dependent picture of the same access unit was
  still predicting against it as its inter-view anchor. Pairing lifetime and
  anchor lifetime are not one lifetime, and the fix says so - the hold now
  transfers to a small bounded, context-local anchor queue when the compose
  stage takes a half over, and both release paths keep that pin alive until
  the anchor queue retires the picture. Only the composed route enters this
  code: the base-view default and every single-view selection are untouched by
  it, and the composed golden below shows what the route delivers under the
  new pin.
- **The one surface this ship adds (2026-09-16).** `mvcsubdepth` registers, so
  `-filters` answers with exactly one entry more than plain FFmpeg 8.1.2 does
  and every other capability list is byte-for-byte what it always was. That is
  why the two capability claims in this document - the list-probe fact in the
  consumer-contract ledger and the "Capabilities" note under Deployment notes -
  name this exception as of this ship: the entry is inert to a consumer that
  only lists capabilities, since a filter acts only inside a graph that names
  it, and the acceptance below is what the unchanged behavior looks like on a
  live run.
- **The documentation that ships with it (2026-09-16).** The README gains the
  usage shape for a subtitle at its authored depth - the graph that renders it,
  the filter's options, and the placement rule that keeps both markers alive as
  far as the filter - and the manuals carry the matching references: the filter
  itself, the decoder's depth side data, the stream tag the transcoder reads,
  and the API note for the two new side-data types.
- **What this ship was measured on (2026-09-16).** Product gate on this exact
  tree: `checkasm` 14903 checks, FATE 357 tests with 0 failures - the 343 this
  branch already gated on plus the 14 new subtitle-depth targets, ten on the
  decoder side of that metadata and four rendering through the filter - and the
  provenance self-test harness at 191 passed, 0 failed. The ship oracle's
  composed golden replays byte-identical on this build: the composed 30 s
  contract row-set, 720 rows, sha256
  `de08be58becc22788ee4ea462dc57caa9292a7aabc713d80903280ac20ba537a`. Live
  acceptance over the three routes an operator actually drives: the default
  transcode route is unchanged, plain 2D with zero MVC warnings in the log; the
  composed route delivers its side-by-side-tagged frames with no drops; and a
  burn-in through the new filter runs the reference title's 720 frames.
- **Compatibility, unchanged (2026-09-16).** The `-jf4` part of the new name
  points where it has always pointed: **`jellyfin-ffmpeg v8.1.2-4`**, unmoved,
  because no update from that build line landed on this ship - which is why
  that digit stands still while the `mvc` counter, which counts this fork's own
  code delta, moves to 5. The containment recorded above therefore stands
  whole under the new number - 94 queue entries landed as individual commits, 3
  already carried by earlier ports, 1 refused on licence with its finding and
  its route back (`0054`: AC-4 is not provided) - and this ship neither
  restates nor relaxes any of it.
- **Subtitle-depth option consolidation ship (2026-09-17).** This tree builds
  and ships as **`n8.1.2-mvc6-jf4`**, superseding `n8.1.2-mvc5-jf4`. On top of
  the tree recorded above, the delta is three commits: the port commit that
  carries the consolidation (`ccd9a9a350`, taken from the master line as one
  commit), the version commit that names this build (`c1c9f07271`), and this
  entry. The release tag for this state is that same string, so tag, `VERSION`
  file and banner state one identity over one tree.
- **The one surface this ship moves (2026-09-17).** The `mvcsubdepth` filter
  now takes one option where it took three. `depth=` is a mode and answers one
  of four spellings: `auto`, the default, places the caption at the authored
  depth of the depth sequence the subtitle track's marker names; `flat` puts
  both eye copies on the screen plane and ignores any authored depth;
  `shift=<pixels>` displaces the caption by that signed constant in each eye;
  `plane=<n>` reads the authored depth of sequence `n` (`0` to `31`) instead of
  asking the track's marker which sequence belongs to it. `0` and `1` are
  accepted as synonyms of `flat` and `auto`, so a graph already written against
  the boolean keeps its meaning. `eye_width` and the framesync options are as
  they were. A value that is none of these - an unknown word, an empty value,
  an incomplete `shift=` or `plane=`, a value with stray text past the number,
  or a mode word given an argument of its own - is refused when the filter is
  initialised, with a notice naming the accepted forms, and is never quietly
  reinterpreted.
- **What that removes, said plainly (2026-09-17).** The separate `plane`,
  `shift` and boolean `depth` spellings are removed and not deprecated, by
  owner ruling: they were live for a single issued build, and keeping both
  surfaces would leave two ways to ask for one placement. A graph naming one of
  the removed spellings therefore fails at filter initialisation on this build
  where it rendered on the one it supersedes. That is the whole breaking change
  and it is the only interface delta of this ship. Nothing else moves: the
  decoder's reading of the authored depth, the transcoder's reading of the
  stream tag, the two side-data types, the composed route and the base-view
  default are untouched by the port, and a request that means the same thing
  renders byte-identical.
- **The claims that therefore need no edit (2026-09-17).** The two capability
  claims made at `n8.1.2-mvc5-jf4` - the list-probe fact in the
  consumer-contract ledger and the "Capabilities" note under Deployment notes -
  stand as written, because this ship adds no capability and removes none:
  `mvcsubdepth` is still exactly the one line `-filters` answers with more than
  plain FFmpeg 8.1.2 does, and every other capability list is byte-for-byte what
  it always was. What moved is the private option spelling of that one filter,
  which no list-probe reads and which acts only inside a graph that names it.
- **What this ship was measured on (2026-09-17).** Product gate on this exact
  tree: `checkasm` 14903 checks, FATE 357 tests with 0 failures - the same set
  this branch gated on at the ship above, three of those targets renamed to the
  modes they now drive and none added or dropped - and the provenance self-test
  harness at 191 passed, 0 failed. The ship validation's composed golden
  replays byte-identical on this build: the composed 30 s contract row-set the
  ship above names in full - 720 rows, its sha256 beginning `de08be58` -
  regenerated from this build's own binary and byte-compared row for row against
  it. Live
  acceptance on the caption window the depth feature was accepted on: a 30 s
  burn-in driven through the bare default spelling `mvcsubdepth=eof_action=pass`
  runs the reference title's 720 frames with 718 of them placed at a nonzero
  depth and the sequence taken from the track's own marker, and the same window
  through `depth=flat` runs the same 720 frames with none displaced.
- **Compatibility, unchanged (2026-09-17).** The `-jf4` part of the new name
  points where it has always pointed: **`jellyfin-ffmpeg v8.1.2-4`**, unmoved,
  because no update from that build line landed on this ship - which is why that
  digit stands still while the `mvc` counter, which counts this fork's own code
  delta, moves to 6. The containment recorded above therefore stands whole under
  the new number, and this ship neither restates nor relaxes any of it.
- **Pairing-alignment ship (2026-09-17).** This tree builds and ships
  as **`n8.1.2-mvc7-jf4`**, superseding `n8.1.2-mvc6-jf4`. On top of
  the tree recorded above, the delta is ten commits: the seven
  commits of the port (`1c55db4aae` .. `92061a825e`) and the merge
  that carries them, the version commit that names this build
  (`595e7f9c82`), and this entry. The release tag for this state is
  that same string, so tag, `VERSION` file and banner state one
  identity over one tree.
- **Composed pairing by access-unit identity (2026-09-17).** The
  composed route now pairs a base half with a dependent half only
  when the two carry the same delivery timestamp - the shared stamp
  the two pictures of one access unit always arrive with - and an
  untimed stream keeps the old queue-order rule as its fallback.
  Queue order alone used to be the key, so a start perturbation of
  one view's output sequence, and the traces show one within the
  first pairs of every seek-started session, became a permanent
  one-frame lead of one half that no shipped measurement could see.
  A queued half whose partner is gone is now delivered standalone,
  in its own black-half frame, so a loss stays visible at its own
  frames instead of sliding every later frame of the run. What an
  operator sees: a composed run started from a seek - the shape of
  every real playback start - delivers its window frame-aligned in
  both halves, while a composed cold start delivers exactly what
  the build this ship supersedes delivered, byte for byte. The
  default decode and every single-view selection are outside this
  code.
- **The fixtures that make a shift visible (2026-09-17).** A new
  two-view fixture whose view lists end one access unit apart, cut
  on a timed grid, and five FATE entries driven from it
  (`h264-mvc-baselead-*`: the composed answer, each half beside the
  matching single-view selection, and the default decode). Every
  composed half is now compared against the decode of that view
  alone, so a one-frame shift of either half is byte-visible to the
  product gate; the older composed fixtures are flat-colour per
  frame, where any shift read identical.
- **What this ship was measured on (2026-09-17).** Product gate on
  this exact tree: `checkasm` 14903 checks, FATE 362 tests with 0
  failures - the 357 this branch already gated on plus the 5 new
  `h264-mvc-baselead-*` entries - and the provenance self-test
  harness at 191 passed, 0 failed. The ship validation's composed
  row-sets replay byte-identical on this build, regenerated from
  its own binary: the composed 30 s contract row-set the ship
  above names (720 rows, its sha256 beginning `de08be58`),
  byte-compared with the stored authority, and the composed 60 s
  grid (1439 rows, its sha256 beginning `2675c6da`); the composed
  contract row-sets of this fix replayed byte-identical on its
  four builds - the master line, the two release-line ports and
  this ship build. The owner demo of the fix - composed
  cold-start and seek-start windows of the reference titles - was
  accepted.
- **Compatibility, unchanged (2026-09-17).** The `-jf4` part of the
  new name points where it has always pointed: **`jellyfin-ffmpeg
  v8.1.2-4`**, unmoved, because no update from that build line
  landed on this ship - which is why that digit stands still while
  the `mvc` counter, which counts this fork's own code delta,
  moves to 7. The containment recorded above stands whole under
  the new number, and this ship neither restates nor relaxes any
  of it.
- **Same-version reissue (2026-09-17).** This leg reissues the
  build already named **`n8.1.2-mvc7-jf4`**; it opens no new
  release number. The delta under that name is one compile fix
  carried on a merge of its own, set under the state the pairing
  ship above records. The release name is deliberately held
  (the owner's decision): the code-delta counter stays at seven
  and the jellyfin digit stays at four, so tag, `VERSION` file
  and banner keep naming the one identity they already named,
  now over a tree that also carries this fix.
- **The compile fix carried (2026-09-17).** A build configured for
  the `libvpl` hardware path has failed to compile since
  `n8.1.2-mvc5-jf4`. The break sat at a seam rather than in fresh
  decode logic: this fork's shared SEI decode routine grew an
  argument, while the call into it from the jellyfin queue's QSV
  path kept the older argument list, so a libvpl-enabled build
  stopped at that call and could not compile. A software-only
  build, which never reaches the QSV path, kept building untouched
  throughout. The fix is one line: the QSV call now passes the
  argument the wider routine asks for, restoring the behavior that
  site held before the break. Software decode never enters the
  code the fix edits, so its output is byte-unaffected; the
  goldens below are the witness.
- **What this reissue keeps the name over (2026-09-17).** No feature
  moves and no output moves, so the release string does not move.
  The counter that counts this fork's own code delta stays at
  seven; the jellyfin digit, tied to **`jellyfin-ffmpeg v8.1.2-4`**,
  stays at four. The containment recorded in the entries above
  stands whole under the held number, and this reissue neither
  restates nor relaxes any of it.
- **What this reissue was certified on (2026-09-17).** A libvpl
  compile leg on the fixed tree, which is the class the fix lives
  in and where the break used to stop the build; the product gate
  on this same tree - `checkasm` 14903 checks, FATE 362 tests with
  0 failures, and the provenance self-test harness at 191 passed,
  0 failed; and the composed contract row-sets, byte-identical to
  the stored goldens: the composed 30 s row-set (720 rows, its
  sha256 beginning `de08be58`) and the composed 60 s grid (1439
  rows, its sha256 beginning `2675c6da`), replayed on the gate
  build's own binary. Certification for a fix that moves no byte
  of software output is this compile leg, this gate, and these
  byte-identical goldens together.
- **Queue-sync ship onto their -5 build (2026-09-23).** This tree builds
  and ships as **`n8.1.2-mvc8-jf5`**, superseding `n8.1.2-mvc7-jf4`. On
  top of the tree recorded above, the delta is nine commits: the seven
  queue commits named below, the version commit that names this build,
  and this entry. The release tag for this state is that same string,
  so tag, `VERSION` file and banner state one identity over one tree.
- **Why both parts of the suffix moved (2026-09-23).** "Build identity"
  above reserves its `-jf<N>` part for one event: their build line
  moving, and this branch realigning to the new build. That event
  happened - `jellyfin/jellyfin-ffmpeg` issued `v8.1.2-5` - so the
  pointer moves from `-jf4` to `-jf5`, and this build is the drop-in for
  **`jellyfin-ffmpeg v8.1.2-5`**. The `mvc` part moved for the reason
  every entry above records for it: content from that build line is
  source in this tree now, and that part counts this branch's own code
  delta, so it moves from 7 to 8 - the same counter moving the same way
  it moved when this branch first took their queue.
- **The FFmpeg base stays 8.1.2, and that is the design (2026-09-23).**
  Their `v8.1.2-5` is the fifth issue of an FFmpeg 8.1.2 build with a
  patch queue on top of it, not a newer FFmpeg. So the `n8.1.2` part of
  this branch's name - the part stating which FFmpeg release a build
  sits on - does not move here, and this line stays an 8.1.2 line even
  while this fork's own 8.1 release line advances past 8.1.2. What this
  branch keeps is the rule that its FFmpeg base follows the base of the
  build its own name points at: taking a newer FFmpeg micro than their
  build uses would put a `-jf5` pointer on a build line that does not
  exist, and would leave this drop-in ahead of the server packages it
  exists to replace. The other half of that rule is on the record with
  it: this ship lands no FFmpeg maintenance fix newer than 8.1.2, so
  the only content moving here is theirs.
- **The seven queue entries this ship lands (2026-09-23).** Their delta
  from `v8.1.2-4` to `v8.1.2-5` is nineteen commits, and their queue
  under `-5` holds a hundred patch files where `-4` held ninety-eight.
  Nine of those commits are source this line cares about, and they land
  here as seven commits - one per queue entry, in the order of their
  queue, each carrying the bytes of the `-5` form of its patch file and
  naming the commit of theirs it comes from in its own message. Five of
  the seven update an entry this tree already held; two are new entries.
  Every commit of this branch is titled after the queue entry it lands,
  which is the handle to look it up by.
  - `0019`, updated - queue file
    `0019-add-fixes-for-qsv-vpp-filters.patch`, their commit "Fix the
    hard cap on frame pool size for D3D11 hwctx". The fixed array-size
    ceiling, and the clamp that applied it while a D3D11 frame pool was
    being initialised, are deleted: a pool asking for more slots than
    that constant died instead of growing.
  - `0042`, updated - queue file
    `0042-add-full-hwa-pipeline-for-rockchip-rk3588-platform.patch`,
    from their two commits moving the Rockchip decoder onto the shared
    decoder-side helpers for the two kinds of HDR metadata, and their
    follow-up syncing further fixes from the out-of-tree decoder of the
    same platform. The hardware decoder now asks those helpers to
    allocate and attach its mastering-display and light-level side data
    instead of open-coding that allocation, keeps an all-zero payload off
    the frame instead of publishing it, covers AV1 frames as it already
    covered HEVC ones, and passes the codec context's flags into the
    buffer-pool request its frame init makes. The same queue entry also
    mends the short-row tails of the packed ten-bit to planar software
    conversion, where a width not divisible by the vector step lost its
    last samples unseen.
  - `0061`, updated - queue file
    `0061-add-remove-dovi-hdr10plus-bsf.patch`, their commit "Make
    HDR10+ removal more precise". The filter that strips HDR10+ dynamic
    metadata from an HEVC stream used to sniff the leading bytes of
    every SEI packet and delete the whole packet on a match, which took
    unrelated messages that merely started the same way; the `-5` form
    reads the packet's message list and removes only the registered
    user-data messages that are HDR10+, dropping the packet only once no
    message is left in it. Asking "is this HDR10+" is now one shared
    helper the HEVC and AV1 filters use alike, and the AV1 path no
    longer re-reads a Dolby-Vision packet as something else there.
  - `0074`, updated - queue file
    `0074-fix-mapped-hwframe-to-swframe-swscale-conversion.patch`, their
    commit "Fix using hwmap with swscale unstable", and its follow-up on
    the test reference that assumption had been written into. Setting up
    a software-scale frame now derives both pixel-format descriptors up
    front and asks for a hardware frames context only where the format
    really is a hardware one, so mapping a frame into the scaler no
    longer rests on the assumption that every such frame arrives with a
    pool of its own.
  - `0079`, updated - queue file
    `0079-add-fixes-for-vaapi-drm-prime-vulkan-interop.patch`, their
    commit "Fix potential RADV hang with linear images on ARM64".
    Binding a linear Vulkan image now asks for device-local memory
    first and falls back to host-visible memory only when that request
    is refused, instead of preferring the host-visible path outright;
    that preference is what could hang the driver.
  - `0099`, new - queue file
    `0099-fix-qsv-av1-decoder-exporting-hdr-side-data.patch`, from their
    commit "Fix QSV AV1 decoder exporting HDR side data" and its polish
    follow-up. The QSV path's HDR metadata export is now one routine
    shared by the HEVC and AV1 hardware decoders, choosing its mapping
    and its luminance denominators per codec, running for AV1 once the
    runtime reports itself ready for it. Before this, the AV1 decoder
    exported HEVC's reading of the same bytes.
  - `0100`, new - queue file
    `0100-backport-trim-bitstream-filter.patch`, their commit "Backport
    trim bitstream filter". A trim at packet level: a start bound and an
    end bound, each naming the quantity it is counted on, the unit it is
    given in and what it is counted from; packets a bound falls inside
    are trimmed rather than only kept or dropped, and an optional
    preroll exports the packets ahead of the range that a decoder needs
    in order to open on the first packet kept - flagged for that decoder
    to drop, so the frames delivered still begin where the caller asked.
- **The standing refusal, re-checked on the -5 queue (2026-09-23).**
  `0054-add-ac4-decoder-for-atsc-3-0.patch` is the one queue entry this
  branch refuses, and `-5` carries work on exactly that lineage: one of
  the nineteen commits in their window syncs that parser and decoder.
  The finding of 2026-09-08 stands unchanged. That import is
  GPL-3-or-later code, and the same patch lifts the float
  reverse-multiply routine out of `libavutil/float_dsp.c` into a new
  GPL-3 file that libavutil builds unconditionally - which would place
  GPL-3-only code inside the library this product ships as LGPL-2.1 or
  later, whatever `./configure` is told. This ship relaxes none of it,
  and the tree is proved clean of it again here: neither file that patch
  would add exists, `ac4` appears in none of `configure`,
  `libavcodec/Makefile`, `allcodecs.c` or `parsers.c`, and the LGPL form
  of that routine is still the one in place where it has always lived.
  The route back is unchanged and remains the honest answer if that
  audio system is ever wanted: keep the gated decoder and parser,
  restore the small LGPL function to the file it came from, and leave
  the two GPL-3 files out. Of their nineteen commits, the nine of
  source interest are the seven entries above, the one on that lineage
  is refused with its reason, and the remaining nine are their own
  release bookkeeping - changelog entries, packaging and
  continuous-integration work in trees this branch does not carry.
- **What this ship was measured on (2026-09-23).** Product gate on this
  exact tree: `checkasm` 14903 checks, FATE 362 tests with 0 failures -
  the same set this branch gated on at the ship above, none added and
  none dropped - and the provenance self-test harness at 220 passed, 0
  failed. That is the bar of the shared toolchain this ship runs under:
  its self-test carries more assertions than the 191 the ships above
  quoted. The composed contract row-sets replay byte-identical on this
  build, regenerated from its own binary and compared row for row: the
  composed 30 s row-set (720 rows, sha256 beginning `de08be58`) and the
  composed 60 s grid (1439 rows, sha256 beginning `2675c6da`). The
  libvpl hardware compile leg is green on this tree, and it is the leg
  that builds the two new entries' code: the shared HDR export this ship
  introduces sits in the same translation unit as the AV1 hardware
  decoder it now serves, and the packet-trimming filter is its own
  object there.
- **The surface this ship adds, and the identity held (2026-09-23).**
  One bitstream filter name is registered where it was not before, so
  the bitstream-filter list answers with one entry more than the build
  this ship supersedes prints. No decoder, encoder, filter-graph filter
  or hardware acceleration method is added or removed: none of those
  registrations is in this ship's diff. That keeps both capability
  claims of this document standing as written - the lists the consuming
  server probes are the ones it has always seen. The build-identity
  claims hold for the same reason and by the same evidence: no library
  version header moves with this ship, so the libav* versions a consumer
  links against, and the SONAMEs derived from them, are exactly the ones
  the build this ship supersedes had. That is the whole of what makes
  this build a drop-in.
- **Compatibility under the new number (2026-09-23).** The containment of
  every entry above stands, and its counts read against their new build:
  their queue at `v8.1.2-5` is a hundred patch files. Ninety-six of them
  are contents this tree holds - the ninety-four landed one per commit
  in the wave recorded above, the two `-5` entries this ship lands the
  same way, and five of them now in their `-5` form. Three are contents
  the earlier ports already carried, listed by name in "Ported fixes"
  above, and one is the entry refused on licence. `-jf5` therefore
  claims what `-jf4` claimed, one build line later: the applicable whole
  of that build's queue is in this tree and can be shown entry by entry,
  and nothing beyond it is claimed - not parity with their packaging,
  and not that their queue is current with upstream's maintenance fixes,
  which this branch tracks directly instead.

