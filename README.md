# FFmpeg-mvc README

FFmpeg-mvc is a fork of FFmpeg, a collection of libraries and tools to
process multimedia content such as audio, video, subtitles and related
metadata. In addition to the upstream codebase, this fork adds
H.264/MVC (ITU-T H.264 / ISO/IEC 14496-10, Annex E) multiview decoding
for 2D+delta streams.

## H.264/MVC support

- Parses the Annex E `sequence_parameter_set_mvc_extension` carried in
  subset SPSes (NAL unit type 15): view list, anchor and non-anchor
  reference lists, operations, and mvc VUI. Supported profiles:
  Stereo High (118) and Multiview High (128).
- Decodes dependent-view slices (NAL unit types 19-23) with their 24-bit
  NAL unit header extension.
- The base view (view ID 0) is decoded by default, so a bare decode of a
  multiview stream yields its plain 2D video. Selecting all views of a
  two-view stream (`view_ids` set to a single `-1`, or `-map 0:v:view:all`)
  delivers it as one native side-by-side frame per access unit with a single
  `AV_FRAME_DATA_STEREO3D` side data entry; select any single eye with a view
  specifier on `-map` (e.g. `-map 0:v:view:1`).
- Hardware acceleration is not supported for MVC streams: requests for
  hardware acceleration fall back to software decoding with a warning.

See the "Multiview video (H.264/MVC)" section of the ffmpeg docs
(doc/ffmpeg.texi) and the h264 decoder entry (doc/decoders.texi).

## Branches and releases

- `master` — rolling development line; upstream's main branch is merged
  in periodically.
- `release/9.0`, `release/8.1` — stable lines tracking the matching
  upstream maintenance branches.
- `jellyfin-8.1` — product branch derived from `release/8.1`: a
  media-server drop-in build that behaves like plain FFmpeg when invoked
  without multiview options. Code taken from jellyfin/jellyfin-ffmpeg is
  attributed in the commits that carry it; operator notes are in
  `PRODUCT-jellyfin.md` on that branch.

### Tests

FATE tests for this support are `h264-mvc-*` and `cbs-h264-mvc-*`
(tests/fate/h264.mak, tests/fate/cbs.mak). Their four sample streams are
kept in-tree under `tests/fate/h264-mvc/`, including a distinct-content
two-view stream that pins per-view selection; run with the samples
staged:

    cp -r tests/fate/h264-mvc "$SAMPLES"/
    make fate-h264 fate-cbs SAMPLES="$SAMPLES"

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## AI-assisted development

AI was used in the development of this fork.
