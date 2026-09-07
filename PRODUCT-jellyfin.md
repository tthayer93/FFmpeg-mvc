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
default the first time a multiview stream is decoded, so server logs record
why the base view was chosen and how to select views.

Everything else is the release-line behavior: multiview view-list export,
view-ID validation and error handling, view specifiers on `-map`, software
decoding with a warning when hardware acceleration is requested for an MVC
stream, and the CBS parameter-set tests. The only behavioral delta versus
the FFmpeg-mvc release line is the base-view default described above; no
wire formats, demuxers, encoders or muxers change.

## Consumer-contract ledger

Facts about the consuming server that this branch's behavior is calibrated
against, each cited to the server's public source (clone of the default
branch at commit `7c463f5`, 2026-09-05; line numbers re-checked 2026-09-07):

- The server validates the `ffmpeg -version` banner with the anchored regex
  `^ffmpeg version n?((?:[0-9]+\.?)+)`, requires at least version 4.4 and
  sets no maximum version; this branch's banner
  `ffmpeg version 8.1.2-mvc` parses as `8.1.2` and passes. —
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
- **Build identity.** The `VERSION` file yields the banner
  `ffmpeg version 8.1.2-mvc`, which satisfies the server's version regex
  above while keeping the fork's identity visible in every log.
- **Capabilities.** No option names, decoders, encoders, filters, hwaccels
  or CLI flags are added or removed relative to FFmpeg 8.1.2, so capability
  probes behave identically.
- **Artifact naming.** Ship the result under this project's own identity
  (e.g. `ffmpeg-mvc-8.1.2-mvc-linux64.tar.xz`). Never reuse the
  `jellyfin-ffmpeg*` binary or package names: those identify a different
  build with its own patches and update channel, and overriding a package
  with foreign content breaks the server's upgrade path. If installing next
  to a package-managed FFmpeg, place this build in its own directory (for
  example `/opt/ffmpeg-mvc/`) and point `JELLYFIN_FFMPEG` / `--ffmpeg` at
  it. Keep `ffprobe` in the same directory as `ffmpeg`.
- **Expected log line.** Decoding an MVC stream logs one informational
  notice per stream stating that the base view is decoded by default and
  naming the `view_ids` option; that is the designed behavior, not an
  error.

## License and patents

The FFmpeg codebase is licensed under the LGPLv2.1 (optionally GPLv3); this
branch changes no licensing posture. An open-source software license is not
a patent grant: decoding H.264/MVC may require patent licensing depending on
your jurisdiction and use case.
