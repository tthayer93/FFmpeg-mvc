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
  one from the decode context. That is the designed behavior, not an
  error.

## License and patents

The FFmpeg codebase is licensed under the LGPLv2.1 (optionally GPLv3); this
branch changes no licensing posture. An open-source software license is not
a patent grant: decoding H.264/MVC may require patent licensing depending on
your jurisdiction and use case.
