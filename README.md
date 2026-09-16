# FFmpeg-mvc README

FFmpeg-mvc is a fork of FFmpeg, a collection of libraries and tools to
process multimedia content such as audio, video, subtitles and related
metadata. In addition to the upstream codebase, this fork adds
H.264/MVC (ITU-T H.264 / ISO/IEC 14496-10, Annex E) multiview decoding
for 2D+delta streams.

## Experimental status

The H.264/MVC support added by this fork is experimental: option
behaviour and output details may change between releases, and damaged
streams are handled on a best-effort basis. Build from a tagged
release rather than `master` if you need reproducibility.

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
- Subtitle depth: when a release carries authored per-plane depth for its
  subtitles, the mvcsubdepth filter renders them at that depth (see Usage).
- Hardware acceleration is not supported for MVC streams: requests for
  hardware acceleration fall back to software decoding with a warning.

See the "Multiview video (H.264/MVC)" section of the ffmpeg docs
(doc/ffmpeg.texi) and the h264 decoder entry (doc/decoders.texi).

## Hardware Acceleration

Hardware acceleration is not supported. Hardware video decoders do
not implement the H.264/MVC extension: upstream hardware decode APIs
have no path for the Annex E dependent-view bitstreams, and this
fork's composed pairing is decoder-internal state that a hardware
path cannot carry. Requests for hardware acceleration log a warning
and decoding falls back to software.

## Building and installing

Building needs the usual FFmpeg build tools: a C compiler (gcc or
clang), `make`, `pkg-config` (to locate external libraries), and
`nasm` on x86. The default build decodes and encodes with all
built-in components, including the MVC decoder:

    ./configure
    make -j"$(nproc)"

To add x264 encoding (GPL) to the build:

    ./configure --enable-gpl --enable-libx264
    make -j"$(nproc)"

`ffmpeg` and `ffprobe` land in the build root and can run from
there; `sudo make install` installs into `/usr/local` (relocate with
`--prefix`, or `DESTDIR=` when packaging; with `--enable-shared`, run
`sudo ldconfig` before first use). For a specific release, check out
its tag first.

## Usage

A multiview stream decodes to its base view by default, so an
ordinary transcode yields the plain 2D video:

    ffmpeg -i in.mkv -c:v ffv1 out.mkv

To reach the other views, select them explicitly. The legacy
`view_ids` option goes **before** `-i`; the `-map` view specifiers go
after `-i` (the two forms cannot be mixed). The examples below
convert a two-view `.mkv` of a 3D movie and use FFV1, the built-in
lossless encoder, so they run on the default build; for smaller
files substitute your own encoder (for example `-c:v libx264 -crf
20` with the GPL build above).

One view only:

    ffmpeg -view_ids 0 -i in.mkv -c:v ffv1 view0.mkv
    ffmpeg -view_ids 1 -i in.mkv -c:v ffv1 view1.mkv

The same with the newer selectors: `-map 0:v:view:0` (or `view:1`)
after `-i`. View 0 is the base view; which physical eye it carries
varies by release, so check a short clip before a long job.

All views, composed into one native side-by-side frame per access
unit:

    ffmpeg -view_ids -1 -i in.mkv -c:v ffv1 sbs.mkv

The frames are tagged side-by-side, which most players honour. To
bake a display format into the pixels, add the `stereo3d` filter:

    # half-width side-by-side
    ffmpeg -view_ids -1 -i in.mkv \
        -vf "stereo3d=in=sbsl:out=sbs2l" -c:v ffv1 sbs_half.mkv

    # half-height top-and-bottom
    ffmpeg -view_ids -1 -i in.mkv \
        -vf "stereo3d=in=sbsl:out=tb2l" -c:v ffv1 tab.mkv

    # red/cyan anaglyph (also: arch, arcc; green/magenta: agmg)
    ffmpeg -view_ids -1 -i in.mkv \
        -vf "stereo3d=in=sbsl:out=arcd" -c:v ffv1 anaglyph.mkv

If the first view is the title's **right** eye, use `in=sbsr`
instead of `in=sbsl`. Composed decoding (`-view_ids -1` or
`-map 0:v:view:all`) runs both views through one single-threaded
pipeline to keep them paired, so it is slower than single-view
decoding. A damaged dependent view is completed against the base
view with a console warning; when the halves cannot be paired,
standalone half frames are delivered rather than dropped.

### Subtitles with depth

Enable subtitles exactly as with plain FFmpeg: select the stream and
carry it into your own filter graph; nothing is displayed by itself.

    # burn one track into both eyes at its authored depth
    ffmpeg -view_ids -1 -i in.mkv \
        -filter_complex "[0:v]format=rgba[vc];[0:s:0]format=rgba[sub];[vc][sub]mvcsubdepth=eof_action=pass[out]" \
        -map "[out]" -c:v ffv1 subs.mkv

`mvcsubdepth` accepts:

- `depth=0|1`: 1 (the default) renders at the authored depth, 0 puts
  both copies flat at the screen plane.
- `shift=<pixels>`: a constant shift instead of the authored depth,
  positive toward the viewer; it overrides everything else.
- `plane=<0..31>`: name the depth sequence for this track; by default
  it comes from its metadata tag, `3d-plane-<lang>` or bare `3d-plane`.

The tag names a sequence in `0..31`; a track with no usable tag, or a
video with no authored depth, renders flat.

Keep `mvcsubdepth` right after the composed video source: stack
filters drop the video's markers and `overlay` drops the subtitle's.

For media servers (Jellyfin, Emby), build the `jellyfin-8.1`
branch: transcodes without view options behave exactly like plain
FFmpeg (operator notes: `PRODUCT-jellyfin.md` on that branch).

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

Fork release names are the upstream FFmpeg release name plus a fork
generation suffix (`-mvcN`, or `-jfN` on the product line).

### Tests

FATE tests for this support are `h264-mvc-*` and `cbs-h264-mvc-*`
(tests/fate/h264.mak, tests/fate/cbs.mak). Their four sample streams are
kept in-tree under `tests/fate/h264-mvc/`, including a distinct-content
two-view stream that pins per-view selection; run with the samples
staged:

    cp -r tests/fate/h264-mvc "$SAMPLES"/
    make fate-h264 fate-cbs SAMPLES="$SAMPLES"

## Contributing

MVC and multiview changes are welcome as pull requests against `master`
in this repository; everything else belongs upstream at FFmpeg. See
`CONTRIBUTING.md` for the routing details.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## AI-assisted development

AI was used in the development of this fork.
