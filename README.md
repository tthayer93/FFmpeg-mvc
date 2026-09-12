# FFmpeg-mvc README

FFmpeg-mvc is a fork of FFmpeg, a collection of libraries and tools to
process multimedia content such as audio, video, subtitles and related
metadata. In addition to the upstream codebase, this fork adds
H.264/MVC (ITU-T H.264 / ISO/IEC 14496-10, Annex E) multiview decoding
for 2D+delta streams.

## Experimental status

The multiview support added by this fork is experimental. It carries no
stability promises: option behaviour and output details may change
between releases, and damaged streams are handled on a best-effort
concealment basis. The tagged releases on the `release/8.1`,
`release/9.0`, and `jellyfin-8.1` branches are the recommended pins;
`master` is a rolling development line. As always with FFmpeg, there is
no warranty of any kind.

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

## Building

Building needs the usual FFmpeg prerequisites: a C compiler (gcc or
clang), `nasm` or `yasm` on x86, `make`, and `pkg-config`; see
`INSTALL.md` for the full list. The examples below encode with
`libx264`, which is GPL-licensed, so configure with GPL enabled and the
x264 development files installed:

    ./configure --enable-gpl --enable-libx264
    make -j"$(nproc)"

The freshly built `ffmpeg` and `ffprobe` land in the build root and can
be run from there. To build a specific release, check out its tag first
(see "Branches and releases"):

    git checkout n8.1.2-mvc3

`./configure --help` lists every option, including optional external
libraries.

## Installing

    sudo make install

installs into `/usr/local` by default. Pass `--prefix` to `configure` to
choose another location, or `DESTDIR=` to `make install` when packaging.
If you configured `--enable-shared` and installed to a prefix the
dynamic loader does not know yet, run `sudo ldconfig` (or add the
library directory to the loader configuration) before first use.

## Usage

A multiview stream decodes to its base view by default, so an ordinary
transcode yields the plain 2D video:

    ffmpeg -i in.mkv -c:v libx264 -crf 20 out.mp4

To reach the other views, this fork adds a `view_ids` option that
selects views when the stream is opened, so it must be placed **before**
`-i`. The examples below convert a two-view `.mkv` remux of a 3D movie;
swap in your own encoder settings.

One view only (the two eyes of the title as separate files):

    ffmpeg -view_ids 0 -i in.mkv -c:v libx264 -crf 20 view0.mp4
    ffmpeg -view_ids 1 -i in.mkv -c:v libx264 -crf 20 view1.mp4

View 0 is the base view; which physical eye each view feeds depends on
the release, so render a short clip and check before committing to a
long job.

All views of a two-view stream, composed into one native side-by-side
frame per access unit:

    ffmpeg -view_ids -1 -i in.mkv -c:v libx264 -crf 20 sbs.mp4

Those frames are tagged as side-by-side, which most players honour. To
bake a display format into the pixels instead, follow the decoder with
the upstream `stereo3d` filter:

    # half-width side-by-side
    ffmpeg -view_ids -1 -i in.mkv \
        -vf "stereo3d=in=sbsl:out=sbs2l" -c:v libx264 -crf 20 sbs_half.mp4

    # half-height top-and-bottom
    ffmpeg -view_ids -1 -i in.mkv \
        -vf "stereo3d=in=sbsl:out=tb2l" -c:v libx264 -crf 20 tab.mp4

    # red/cyan anaglyph (other styles: agmg, aghs)
    ffmpeg -view_ids -1 -i in.mkv \
        -vf "stereo3d=in=sbsl:out=arcd" -c:v libx264 -crf 20 anaglyph.mp4

If the first view of the title is its **right** eye, say so with
`in=sbsr` instead of `in=sbsl`. Composed (`-view_ids -1`) decoding runs
both views through one single-threaded pipeline to keep them correctly
paired, so it is slower than single-view decoding; damage near a broken
access unit is concealed symmetrically in both views, with a warning on
the console. `-map 0:v:view:N` (and `-map 0:v:view:all`) work as
alternative selectors.

For media servers (for example Jellyfin or Emby), build the
`jellyfin-8.1` branch: transcodes run without view options behave
exactly like plain FFmpeg (base view, plain 2D), and the multiview
paths above are available wherever the server allows custom
encoder or filter arguments. Operator notes: `PRODUCT-jellyfin.md`.

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

Current pins — the release banner of a build matches its tag:

| Branch         | Pin              |
|----------------|------------------|
| `release/8.1`  | `n8.1.2-mvc3`    |
| `release/9.0`  | `n9.0.1-mvc3`    |
| `jellyfin-8.1` | `n8.1.2-mvc3-jf4` |

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
