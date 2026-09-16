#!/usr/bin/perl
# ---------------------------------------------------------------------------
# Subtitle-canvas fixture generator.
#
# Writes a Presentation Graphic Stream elementary stream and a minimal MPEG-TS
# wrapper for it.  The stream has exactly one caption: a solid rectangle placed
# so that the centre of its bounding box is the centre of a 720x576 canvas.
# The default rectangle is large enough to stay visible after the subtitle
# canvas has been scaled into a tiny test eye, but small enough to keep the
# fixture byte counts stable.  That is enough for the depth-placement tests:
# the caption's position is known without reading pixels, and the canvas is big
# enough for the transcoder's subtitle-to-video path to scale into any test
# eye width.
#
# The elementary stream cannot be fed to FFmpeg directly because this tree has
# no raw Presentation Graphic Stream demuxer; the MPEG-TS wrapper is the smallest
# container the demuxers in the tree already recognise for it (stream type 0x90,
# stream_id 0xBD).  The depth filter also needs to learn which depth sequence
# belongs to the subtitle track; the TS wrapper carries only the caption bytes,
# so the FATE route remuxes the subtitle track into Matroska with the bare
# `3d-plane` stream tag before the filter graph consumes it.  The matching MVC
# fixture needs packet timestamps too, but keeping the two tracks in separate
# containers preserves both properties: the subtitle stays a recognisable
# Presentation Graphic Stream track, and the video stays one access unit per
# packet.  The commands that make the committed FATE inputs are:
#
#   perl mvc-mkpgs.pl --sup=subdepth-plane0.sup --ts=subdepth-plane0.ts
#   ffmpeg -i subdepth-plane0.ts -map 0:s:0 -c copy \
#          -metadata:s:s:0 3d-plane=0 subdepth-plane0-s.mkv
#   ffmpeg -fflags +genpts -f h264 -i 2view-ofmd.h264 \
#          -map 0:v -c copy -fps_mode passthrough subdepth-plane0-v.nut
#
# The PTS of the caption is 0 at 90 kHz, matching the first access unit of the
# MVC fixture used by the depth tests.  No end marker is authored: a later
# display set is the way a caption is replaced, and these tests do not need one.
# ---------------------------------------------------------------------------
use strict;
use warnings;

my $sup        = '/tmp/subdepth-plane0.sup';
my $ts         = '/tmp/subdepth-plane0.ts';
my $canvas_w   = 720;
my $canvas_h   = 576;
my $cap_w      = 360;
my $cap_h      = 360;
my $cap_x      = 180;
my $cap_y      = 108;
my $pts        = 0;
my $rate_code  = 3;
my $pid_pat    = 0x0000;   # the Program Association Table has a fixed PID
my $pid_pmt    = 0x1001;
my $pid_sub    = 0x1100;
my $program    = 1;
my $stream_type = 0x90;   # Presentation Graphic Stream subtitle

for my $arg (@ARGV) {
    $sup        = $1 if $arg =~ /^--sup=(.+)$/;
    $ts         = $1 if $arg =~ /^--ts=(.+)$/;
    $canvas_w   = $1 if $arg =~ /^--canvas-w=(\d+)$/;
    $canvas_h   = $1 if $arg =~ /^--canvas-h=(\d+)$/;
    $cap_w      = $1 if $arg =~ /^--cap-w=(\d+)$/;
    $cap_h      = $1 if $arg =~ /^--cap-h=(\d+)$/;
    $cap_x      = $1 if $arg =~ /^--cap-x=(\d+)$/;
    $cap_y      = $1 if $arg =~ /^--cap-y=(\d+)$/;
    $pts        = $1 if $arg =~ /^--pts=(\d+)$/;
    $pid_pat    = hex($1) if $arg =~ /^--pid-pat=(0x[0-9a-fA-F]+)$/;
    $pid_pmt    = hex($1) if $arg =~ /^--pid-pmt=(0x[0-9a-fA-F]+)$/;
    $pid_sub    = hex($1) if $arg =~ /^--pid-sub=(0x[0-9a-fA-F]+)$/;
}

die "the caption must fit the canvas\n"
    if $cap_w <= 0 || $cap_h <= 0 || $cap_x < 0 || $cap_y < 0 ||
       $cap_x + $cap_w > $canvas_w || $cap_y + $cap_h > $canvas_h;
die "the caption is too wide for the RLE escape\n" if $cap_w > 0x3fff;
die "the PIDs are 13-bit fields\n"
    if grep { $_ < 0 || $_ > 0x1fff } ($pid_pat, $pid_pmt, $pid_sub);

sub be16 { return pack 'n', $_[0] }
sub be24 {
    my ($v) = @_;
    die "value $v is not a 24-bit unsigned number\n" if $v < 0 || $v > 0xffffff;
    return chr(($v >> 16) & 0xff) . chr(($v >> 8) & 0xff) . chr($v & 0xff);
}
sub be32 { return pack 'N', $_[0] }
sub seg  { my ($type, $payload) = @_; return chr($type) . be16(length $payload) . $payload }

sub build_display_set {
    return seg(0x16, presentation_segment()) .
           seg(0x14, palette_segment())      .
           seg(0x17, window_segment())       .
           seg(0x15, object_segment())       .
           seg(0x80, "\x00\x00\x00\x00");
}

sub presentation_segment {
    my $payload  = be16($canvas_w) . be16($canvas_h);
    $payload .= chr($rate_code);          # frame-rate code, not used by FFmpeg
    $payload .= be16(0);                  # composition object id
    $payload .= chr(0x40);                # state 1: acquisition point
    $payload .= chr(0x00);                # flags: no palette update
    $payload .= chr(0x00);                # palette id
    $payload .= chr(0x01);                # object count
    $payload .= be16(1);                  # object id
    $payload .= chr(0x00);                # window id
    $payload .= chr(0x00);                # composition flag: not forced, no crop
    $payload .= be16($cap_x);
    $payload .= be16($cap_y);
    return $payload;
}

sub palette_segment {
    my $payload = chr(0x00) . chr(0x00);  # palette id 0, version 0
    $payload .= chr(0x00) . chr(0x00) . chr(0x80) . chr(0x80) . chr(0x00);
    $payload .= chr(0x01) . chr(0xee) . chr(0x80) . chr(0x80) . chr(0xff);
    return $payload;
}

sub window_segment {
    my $payload = chr(0x00) . chr(0x00) . chr(0x00);
    $payload .= be16(0) . be16(0) . be16($canvas_w) . be16($canvas_h);
    return $payload;
}

sub object_segment {
    my $rle = rle_bitmap();
    my $stored = 4 + length($rle);        # stored size includes width/height
    my $payload = be16(1) . chr(0x00) . chr(0x80);
    $payload .= be24($stored);
    $payload .= be16($cap_w) . be16($cap_h);
    $payload .= $rle;
    return $payload;
}

# One solid rectangle.  Each scan line is encoded as a single RLE run of colour
# index 1, followed by the two-byte end-of-line code.  Keeping the rectangle
# compact lets the caption stay visible at a small eye without turning the
# fixture into a megabyte stream.
sub rle_run {
    my ($run, $color) = @_;
    return chr(0x00) . chr(0x80 | $run) . chr($color) if $run < 0x40;
    return chr(0x00) . chr(0xc0 | (($run >> 8) & 0x3f)) .
           chr($run & 0xff) . chr($color);
}

sub rle_bitmap {
    my $line = rle_run($cap_w, 1) . "\x00\x00";
    return $line x $cap_h;
}

sub build_pes {
    my ($pts_value, $payload) = @_;
    my $pes = "\x00\x00\x01\xbd" . be16(8 + length $payload);
    $pes .= chr(0x80) . chr(0x80) . chr(5);
    $pes .= pts_bytes($pts_value);
    return $pes . $payload;
}

sub pts_bytes {
    my ($value) = @_;
    die "PTS $value is not 33-bit unsigned\n" if $value < 0 || $value > 0x1fffffff;
    my @b;
    $b[0] = 0x21 | ((($value >> 30) & 0x07) << 1);
    $b[1] = ($value >> 22) & 0xff;
    $b[2] = ((($value >> 14) & 0xff) << 1) | 1;
    $b[3] = ($value >> 7) & 0xff;
    $b[4] = (($value << 1) & 0xff) | 1;
    return join '', map { chr($_) } @b;
}

# MPEG-TS section CRC: MSB-first polynomial 0x04c11db7, initial value
# 0xffffffff, and no final complement.  This is not the reflected CRC used by
# zip/zlib.
sub crc32 {
    my ($s) = @_;
    my $crc = 0xffffffff;
    for my $byte (unpack 'C*', $s) {
        $crc ^= ($byte << 24);
        $crc &= 0xffffffff;
        for (0 .. 7) {
            if ($crc & 0x80000000) {
                $crc = (($crc << 1) ^ 0x04c11db7) & 0xffffffff;
            } else {
                $crc = ($crc << 1) & 0xffffffff;
            }
        }
    }
    return $crc;
}

sub pat_section {
    my $body = be16($program) . chr(0xc1) . chr(0x00) . chr(0x00);
    $body .= be16($program) . be16(0xe000 | ($pid_pmt & 0x1fff));
    my $table = chr(0x00) . chr(0xb0) . chr(length($body) + 4) . $body;
    return $table . be32(crc32($table));
}

sub pmt_section {
    my $body  = be16($program) . chr(0xc1) . chr(0x00) . chr(0x00);
    $body .= be16(0xe000 | 0x1fff);          # reserved bits, PCR_PID not present
    $body .= be16(0xf000 | 6);               # reserved bits + program_info length
    # Registration descriptor naming the profile this private stream carries.
    $body .= chr(0x05) . chr(0x04) . chr(0x48) . chr(0x44) . chr(0x4d) . chr(0x56);
    $body .= chr($stream_type);
    $body .= be16(0xf000 | ($pid_sub & 0x1fff));
    $body .= be16(0xf000);
    my $table = chr(0x02) . chr(0xb0) . chr(length($body) + 4) . $body;
    return $table . be32(crc32($table));
}

sub ts_packet {
    my ($payload, $pid, $cc, $first) = @_;
    $first //= 1;
    die "PID $pid is not 13-bit unsigned\n" if $pid < 0 || $pid > 0x1fff;
    my $after = 188 - 4 - length($payload);
    die "TS payload is too long\n" if $after < 0;

    my $b1 = (($pid >> 8) & 0x1f) | ($first ? 0x40 : 0x00);
    my $b3 = ($after > 0 ? 0x30 : 0x10) | ($cc & 0x0f);
    my $packet = chr(0x47) . chr($b1) . chr($pid & 0xff) . chr($b3);

    if ($after > 0) {
        my $field = $after - 1;
        $packet .= chr($field);
        $packet .= "\x00" if $field > 0;
        $packet .= "\xff" x ($field - 1) if $field > 1;
    }
    $packet .= $payload;
    die "TS packet is " . length($packet) . " bytes, not 188\n" if length($packet) != 188;
    return $packet;
}

sub build_ts {
    my ($pes_payload) = @_;
    my $out = '';
    $out .= ts_packet("\x00" . pat_section(), $pid_pat, 0, 1);
    $out .= ts_packet("\x00" . pmt_section(), $pid_pmt, 0, 1);

    # The caption may need more than one transport packet; only the first PES
    # chunk carries the packetized elementary stream header.
    my $cc = 0;
    while (length $pes_payload) {
        my $chunk = length($pes_payload) > 184 ? substr($pes_payload, 0, 184) : $pes_payload;
        $pes_payload = length($pes_payload) > 184 ? substr($pes_payload, 184) : '';
        $out .= ts_packet($chunk, $pid_sub, $cc, $cc == 0);
        $cc = ($cc + 1) & 0x0f;
    }
    return $out;
}

sub write_file {
    my ($path, $data) = @_;
    open(my $fh, '>:raw', $path) or die "open $path: $!";
    print {$fh} $data;
    close $fh or die "close $path: $!";
}

my $sup_data = build_display_set();
my $pes      = build_pes($pts, $sup_data);
die "the subtitle packet is too long for the PES length field\n"
    if length($pes) > 0xffff;

write_file($sup, $sup_data);
write_file($ts, build_ts($pes));
