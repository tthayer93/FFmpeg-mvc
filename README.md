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
- All views are decoded by default. A two-view stream with all views
  selected is delivered as one native side-by-side frame per access unit
  with a single `AV_FRAME_DATA_STEREO3D` side data entry; select a single
  eye with a view specifier on `-map` (e.g. `-map 0:v:view:0`).

See the "Multiview video (H.264/MVC)" section of the ffmpeg docs
(doc/ffmpeg.texi) and the h264 decoder entry (doc/decoders.texi).

### Tests

FATE tests for this support are `h264-mvc-*` and `cbs-h264-mvc-*`
(tests/fate/h264.mak, tests/fate/cbs.mak). Their three sample streams are
kept in-tree under `tests/fate/h264-mvc/`; run with the samples staged:

    cp -r tests/fate/h264-mvc "$SAMPLES"/
    make fate-h264 fate-cbs SAMPLES="$SAMPLES"

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.
