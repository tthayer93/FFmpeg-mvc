# Contributing to FFmpeg-mvc

This fork carries one mission on top of FFmpeg: H.264/MVC multiview
(3D) decoding. Where a change should be sent depends on what it
touches.

- **MVC and multiview items** — the H.264/MVC decoder and parser paths
  (`libavcodec/h264*`), view selection in fftools, composed
  side-by-side output, the `h264-mvc-*` and `cbs-h264-mvc-*` FATE
  tests, and this fork's documentation of them — open a pull request
  against `master` in this repository. Issues against this repository
  are fine for bugs and questions about that behaviour too.

- **Everything else** — upstream codecs, filters, demuxers, tools,
  build system, and general FFmpeg behaviour — goes **upstream to
  FFmpeg**, not here: see
  <https://ffmpeg.org/developer.html#Contributing> (the ffmpeg-devel
  mailing list, or FFmpeg's own Forgejo at code.ffmpeg.org). This fork
  re-imports upstream work regularly, so an accepted upstream change
  flows back into these branches at the next merge, while a fork-only
  copy of general work would just drift from it.

Pull requests here that touch non-MVC code are usually closed with a
pointer to the upstream process, so that review of shared code stays in
one place.
