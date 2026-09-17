#!/usr/bin/perl
# ---------------------------------------------------------------------------
# Two-view H.264/MVC Annex-B fixture generator.
#
# Assembles a minimal 16x16 two-view stream: a base-view SPS, one picture
# parameter set per view, an access unit delimiter per access unit, one intra
# picture per view per access unit, and - between the first base picture and
# the first dependent picture - the subset SPS carrying the Annex E multiview
# extension. Every NAL unit that is not a picture is copied verbatim from the
# shipped sibling stream (default: 2view-p128.h264, beside this script), so the
# headers stay bit-identical to what already ships and only the slice payloads
# are authored here.
#
# Each picture is exactly one macroblock, coded Intra_16x16 with a single luma
# DC coefficient, so a picture renders flat and which view a run delivered is
# visible in the pixels: with the coefficients the FATE entries use (+100 in
# the base view, -100 in the dependent view, quantised differently by the two
# picture parameter sets) the views render as luma 158 and 47 with chroma 128.
#
# The picture count of each view is a parameter, and that is what makes the
# stream interesting to the composed output: a view list that ends one access
# unit before the other leaves the pairing with a half it can never pair, so
# the end-of-stream drain ships that half out deterministically.
#
# Macroblock layer, as the in-tree CAVLC decoder reads it (h264_cavlc.c):
#   mb_type                ue(v) = 3  -> Intra_16x16, DC prediction, coded
#                                        block pattern 0 (only the Intra16x16
#                                        DC block is present)
#   intra_chroma_pred_mode ue(v) = 0
#   mb_qp_delta            se(v) = 0  (read whenever an Intra16x16 DC block
#                                        is present)
#   Intra16x16 DC residual block, context 0 (no available neighbour):
#     total_coeff 0                                   -> "1"
#     total_coeff 1, trailing_ones 1 + sign bit       -> "01" + sign
#     total_coeff 1, trailing_ones 0  + level         -> "000101" + level
#     total_zeros (total_coeff 1, value 0)            -> "1"
#   The one coefficient sits at scan position 0, so the macroblock stays flat:
#   intra prediction falls back to 1<<(bitDepth-1) = 128 (no available top or
#   left samples) and the DC residual shifts every luma sample equally.
#
# The CAVLC codewords are checked against the in-tree decoder tables before
# they are used, so a table change is noticed here instead of silently
# producing a fixture that decodes as something else.
#
# Both views address their picture with the same 4-bit frame_num and the same
# pic_order_cnt_lsb. Without a refresh period that is 2 * the access unit index,
# which caps a view list at 8 pictures; with --idr-every the same fields restart
# at each IDR, so the period itself has to fit the reused headers.
#
# usage: mvc-mkfix.pl [--dna=FILE] [--out=FILE] [--base=N] [--dep=N]
#                     [--base2=N] [--dep2=N] [--base-frames=N] [--dep-frames=N]
#                     [--view1-start=N] [--idr-every=N] [--copy] [--zero]
#                     [--ofmd] [--ofmd-frames=N] [--ofmd-seq=N] [--ofmd-pts=N] [--ofmd-rate=N]
#                     [--ofmd-base=N] [--ofmd-lag-pattern=a,b,...]
#   --base/--dep     signed Intra16x16 DC coefficient of view 0 / view 1
#   --base2/--dep2   alternate DC coefficient for the odd pictures of the same
#                    view; without it every picture of that view is flat, and
#                    with it a pairing shift is visible inside a single eye
#   --base-frames    pictures carried by the base view (default 2)
#   --dep-frames     pictures carried by the dependent view (default 2)
#   --view1-start    access-unit offset at which the dependent view's FIRST
#                    picture is carried, leaving the base view one (or more)
#                    access units ahead in its output sequence
#   --idr-every      mark the base view's first picture of every Nth access unit
#                    as an IDR and restart frame_num/pic_order_cnt_lsb there;
#                    0 keeps the single IDR at the stream front
#   --copy           re-emit the sibling stream verbatim (escaping self-check)
#   --zero           author zero-residual payloads at the sibling's picture
#                    counts and require the result to be the sibling exactly
#   --ofmd           write the dependent view's subtitle-depth (offset
#                    metadata) SEI of the first access unit, ahead of the
#                    dependent picture it describes (see below)
#   --ofmd-frames    pictures the block describes (default: the dependent
#                    picture count, i.e. the whole fixture)
#   --ofmd-pts       90 kHz timestamp of the described group (default 0)
#   --ofmd-rate      picture-rate code of the block (default 3 = 25 fps, the
#                    rate the elementary-stream demuxer timestamps this stream
#                    at, so every picture is covered)
#   --ofmd-base      stamp the block this many picture durations of its own
#                    declared rate later than --ofmd-pts says, without touching
#                    anything else in the stream: same pictures, same order,
#                    same access units. That is what a demuxer does to a disc:
#                    it copies the pictures and their metadata verbatim while
#                    re-stamping the pictures from its own base, so the block's
#                    timestamp is left on the disc's timeline and the pictures
#                    are not. A decoder that reads depth by the pictures' time
#                    therefore has to move the block over by that base offset,
#                    and a fixture that differs from another one only in this
#                    number is the test that it does.
#   --ofmd-lag-pattern
#                    author MULTIPLE blocks - one per pattern entry, at least
#                    three - tiling the dependent pictures in order: block g
#                    describes pictures [g*F .. (g+1)*F) of the dependent view
#                    (F = --ofmd-frames), stamped like the single block is
#                    (--ofmd-pts plus --ofmd-base picture durations of the
#                    declared rate) further shifted by g*F of the block's own
#                    pictures, with the table ramp continuing across blocks so
#                    one picture's row says which block answered it.  Each
#                    block's SEI is injected a entries pictures into its own
#                    group (ahead of dependent picture g*F + a_g) rather than
#                    on the group's start: that is how authored discs split the
#                    two shapes - some titles ride the message two pictures
#                    after the start of the group it describes.  A pattern of
#                    all zeros keeps every SEI on its group's start, which is
#                    the same stream's on-time shape; the two fixtures' depth
#                    rows differing only where a late message's first pictures
#                    have already been decoded is the max-run phase test.
#                    Implies --ofmd.
#
# The --zero check is the reason this script is kept next to the fixtures it
# writes: authored payloads that reproduce a shipped sample byte for byte are
# payloads the shipped decoder agrees with.
#
# The subtitle-depth block (--ofmd) is the BD3D offset-metadata message, per
# the public implementations of it (LAVFilters msdk_mvc, MPC-BE MSDKDecoder
# ParseOffsetMetadata, Kodi BlurayOffsetMetadata). It travels as a
# user_data_unregistered message with a fixed UUID and the tag 'OFMD', wrapped
# in a scalable-nesting message, in the dependent view only, once per group of
# pictures:
#
#   0x25 <size> 0x40   nesting: all views of the access unit, one-byte header
#   0x05 <size>        user_data_unregistered
#   UUID(16) 'OFMD'
#   10-byte header: picture-rate code, the 36 bit 90 kHz timestamp of the group
#                   with its marker bits, the sequence count, the picture count
#                   of the group, and two bytes that are opaque to every
#                   implementation reading the message
#   then sequence-count * picture-count bytes, sequence-major and in display
#   order, one per picture: bit 7 the direction flag (set = behind the screen)
#   and bits 0..6 the magnitude in pixels. A flat entry therefore goes over as
#   0x80, the form a mastering tool authors, or as 0x00, the form a zero
#   magnitude toward the viewer writes; both mean no displacement.
#
# The four sequences written here say four different things about the same
# pictures, and their first entries are the four forms the wire format has:
#   0  +5, +6, +7, ...    a ramp toward the viewer          (wire 05 06 07 ..)
#   1  -1,  0, +1, ...    behind, then flat, then toward    (wire 81 00 01 ..)
#   2   0,  0,  0, ...    authored flat                     (wire 80 80 80 ..)
#   3   0,  1,  2, ...    a ramp from flat                  (wire 00 01 02 ..)
# Sequences 4 and above repeat sequence 3. A group shorter than the fixture
# leaves the trailing pictures of the fixture uncovered, which is a state the
# decoder has to get right too.
# ---------------------------------------------------------------------------
use strict;
use warnings;

use File::Basename qw(dirname);

my $dnafile = dirname(__FILE__) . '/2view-p128.h264';
my $out     = '/tmp/out.h264';
my ($base_lvl, $dep_lvl) = (0, 0);
my ($base_frames, $dep_frames) = (2, 2);
my $copy_only  = 0;
my $zero_check = 0;
my ($ofmd, $ofmd_frames, $ofmd_seq, $ofmd_pts, $ofmd_rate, $ofmd_base) =
    (0, -1, 4, 0, 3, 0);
my @ofmd_lags;
# Composed-pairing fixtures: an alternate luma level makes each view's picture
# differ from its own neighbour frame-to-frame (so a pairing shift is visible in
# the pixels, not just between the views), --view1-start starts the dependent
# view some access units after the base view (the one-AU base-lead a session
# start leaves behind, which the FIFO pairing propagated across the whole run),
# and --idr-every refreshes the base view on a period so a container wrapper
# from this stream has seekable keyframes rather than only one at the very front.
my ($base2_lvl, $dep2_lvl) = (undef, undef);
my $view1_start = 0;
my $idr_every   = 0;

for my $a (@ARGV) {
    $dnafile     = $1 if $a =~ /^--dna=(.+)$/;
    $out         = $1 if $a =~ /^--out=(.+)$/;
    $base_lvl    = $1 if $a =~ /^--base=(-?\d+)$/;
    $dep_lvl     = $1 if $a =~ /^--dep=(-?\d+)$/;
    $base2_lvl   = $1 if $a =~ /^--base2=(-?\d+)$/;
    $dep2_lvl    = $1 if $a =~ /^--dep2=(-?\d+)$/;
    $base_frames = $1 if $a =~ /^--base-frames=(\d+)$/;
    $dep_frames  = $1 if $a =~ /^--dep-frames=(\d+)$/;
    $view1_start = $1 if $a =~ /^--view1-start=(\d+)$/;
    $idr_every   = $1 if $a =~ /^--idr-every=(\d+)$/;
    $ofmd_frames = $1 if $a =~ /^--ofmd-frames=(\d+)$/;
    $ofmd_seq    = $1 if $a =~ /^--ofmd-seq=(\d+)$/;
    $ofmd_pts    = $1 if $a =~ /^--ofmd-pts=(\d+)$/;
    $ofmd_rate   = $1 if $a =~ /^--ofmd-rate=(\d+)$/;
    $ofmd_base   = $1 if $a =~ /^--ofmd-base=(\d+)$/;
    @ofmd_lags   = split /,/, $1 if $a =~ /^--ofmd-lag-pattern=([0-9]+(?:,[0-9]+)*)$/;
    $ofmd        = 1  if $a eq '--ofmd';
    $ofmd        = 1  if @ofmd_lags;
    $copy_only   = 1  if $a eq '--copy';
    $zero_check  = 1  if $a eq '--zero';
}

die "the base view needs at least one picture\n" if $base_frames < 1;
die "the dependent view needs at least one picture\n" if $dep_frames < 1;

my $max_au = $base_frames - 1;
$max_au = $view1_start + $dep_frames - 1 if $view1_start + $dep_frames - 1 > $max_au;

if ($idr_every) {
    die "--idr-every=0 already disables the period (no value needed)\n" if $idr_every < 1;
    die "--idr-every=$idr_every needs a picture order count that fits the 4-bit
         pic_order_cnt_lsb of the reused headers (at most 8 pictures per period)\n"
        if $idr_every > 8;
}
else {
    die "an access unit index $max_au does not fit the 4-bit pic_order_cnt_lsb
         of the reused headers (at most 8 pictures per view)\n"
        if $max_au > 7;
}

die "--view1-start needs an elementary stream with no subtitle-depth block\n"
    if $view1_start && $ofmd;
die "--idr-every has nothing to do with --copy/--zero\n"
    if ($copy_only || $zero_check) && $idr_every;
die "--view1-start has nothing to do with --copy/--zero\n"
    if ($copy_only || $zero_check) && $view1_start;
die "--base2/--dep2 have nothing to do with --copy/--zero\n"
    if ($copy_only || $zero_check) && (defined($base2_lvl) || defined($dep2_lvl));

die "--ofmd-lag-pattern needs an explicit --ofmd-frames (the block size it tiles)\n"
    if $ofmd && @ofmd_lags && $ofmd_frames < 0;
$ofmd_frames = $dep_frames if $ofmd_frames < 0;
die "--ofmd-frames must describe at most the $dep_frames dependent pictures\n"
    if $ofmd && $ofmd_frames > $dep_frames;
die "the sequence count is a 6-bit field with 32 the largest one authored\n"
    if $ofmd && ($ofmd_seq < 1 || $ofmd_seq > 32);
die "--ofmd has nothing to do with --copy/--zero\n" if $ofmd && ($copy_only || $zero_check);
die "--ofmd-base has nothing to shift without --ofmd\n" if !$ofmd && $ofmd_base;
die "--ofmd-lag-pattern wants a comma-separated list of non-negative integers\n"
    if grep(/^--ofmd-lag-pattern=/, @ARGV) && !@ofmd_lags;

# frame_rate_code to {numerator, denominator} of the picture rate, the same
# table the decoder reads the field with (libavcodec/h264_sei.c
# ofmd_frame_rates).  --ofmd-base states its shift in pictures of this rate, so
# the two have to agree about what one picture is worth.
my %ofmd_rates = (
     1 => [24000, 1001],   2 => [24, 1],   3 => [25, 1],   4 => [30000, 1001],
     5 => [30, 1],         6 => [50, 1],   7 => [60000, 1001],
     8 => [60, 1],         9 => [100, 1], 10 => [120, 1], 11 => [200, 1],
    12 => [240, 1],       13 => [300, 1],
);

# one picture of the declared rate, in 90 kHz units, rounded the way the
# decoder rounds a group length (av_rescale_rnd to nearest)
my $ofmd_step = 0;
if ($ofmd) {
    my $r = $ofmd_rates{$ofmd_rate}
        or die "--ofmd-rate=$ofmd_rate declares no picture rate\n";
    $ofmd_step = int(90000 * $r->[1] / $r->[0] + 0.5);
}

# The timestamp of the described group as it goes on the wire: --ofmd-pts with
# --ofmd-base added to it.  The field is 3 + 15 + 15 bits, so refuse to write a
# timestamp that would not survive the packing.
my $ofmd_wire_pts = $ofmd_pts + $ofmd_base * $ofmd_step;
die sprintf("--ofmd-base=%d --ofmd-pts=%d is %d, past the 33 bits of the field\n",
            $ofmd_base, $ofmd_pts, $ofmd_wire_pts)
    if $ofmd && $ofmd_wire_pts > 2**33 - 1;

# Multi-block mode (--ofmd-lag-pattern): block g describes dependent pictures
# [g*F .. (g+1)*F), is stamped one block-duration after the block before it,
# and its SEI rides ahead of dependent picture g*F + pattern[g].  Everything
# the mode needs to exist by the time it is written is checked here; the SEI
# bytes themselves are rendered in the assembly section (they need the message
# writer defined below).
my @ofmd_group_pts;             # block index -> wire pts of that block
if ($ofmd && @ofmd_lags) {
    my $groups = 0 + @ofmd_lags;
    die "--ofmd-lag-pattern describes $groups blocks; the mode exists to make that count MULTIPLE (3 or more)\n"
        if $groups < 3;
    die sprintf("--ofmd-lag-pattern=%d blocks x --ofmd-frames=%d does not fit --dep-frames=%d\n",
                $groups, $ofmd_frames, $dep_frames)
        if $groups * $ofmd_frames > $dep_frames;
    for my $g (0 .. $groups - 1) {
        my $slot = $g * $ofmd_frames + $ofmd_lags[$g];
        die sprintf("block %d of --ofmd-lag-pattern rides ahead of dependent picture %d, past the %d the stream carries\n",
                    $g, $slot, $dep_frames)
            if $slot >= $dep_frames;
        my $gpts = $ofmd_wire_pts + $g * $ofmd_frames * $ofmd_step;
        die sprintf("block %d of --ofmd-lag-pattern stamps %d, past the 33 bits of the field\n",
                    $g, $gpts)
            if $gpts > 2**33 - 1;
        $ofmd_group_pts[$g] = $gpts;
    }
}


open(my $fh, '<:raw', $dnafile) or die "open $dnafile: $!";
local $/ = undef;
my $dna = <$fh>;
close $fh;

my @nal  = split_nals($dna);
my @role = map {
    my $t = $_->{type};
    $t == 7  ? 'sps'
  : $t == 8  ? ($_->{raw} =~ /^\x68\xce/ ? 'pps0' : 'pps1')
  : $t == 9  ? 'aud'
  : $t == 15 ? 'subsetsps'
  : $t == 5  ? 'idr0'
  : $t == 1  ? 'intra0'
  : $t == 19 ? 'mvcslice'
  : 'other'
} @nal;
die "unexpected NAL sequence (@role)\n"
    unless "@role" eq 'sps pps0 aud idr0 subsetsps pps1 mvcslice aud intra0 mvcslice';

# The sibling stream is the two access units it is. Everything the generator
# writes is either one of these NAL units verbatim or a payload authored below
# for the same slot.
my ($idr0)     = grep { $_->{type} == 5  } @nal;   # base view, access unit 0
my ($intra0)   = grep { $_->{type} == 1  } @nal;   # base view, access unit 1
my @mvcslice   = grep { $_->{type} == 19 } @nal;   # view 1, one per access unit
my ($aud)      = grep { $_->{type} == 9  } @nal;   # access unit delimiter
my ($mvcslice) = @mvcslice;
# Dependency-annotated slice header of view 1 (priority / view id / anchor
# signalling), reused verbatim for every dependent picture.
my $mvhdr = substr($mvcslice->{raw}, 0, 4);

# --copy has nothing to author, so it can only be the sibling stream itself
die "--copy only re-emits the sibling stream (2 pictures per view)\n"
    if $copy_only && ($base_frames != 2 || $dep_frames != 2);

# ---- bit writer ------------------------------------------------------------
my @bits;
sub wbits { my ($s) = @_; push @bits, split //, $s }
sub wu    { my ($n, $v) = @_; for (my $i = $n - 1; $i >= 0; $i--) { push @bits, ($v >> $i) & 1 } }
sub wue   { my ($v) = @_; my $m = 0; my $x = $v + 1; while ($x > 1) { $x >>= 1; $m++ }
            wbits('0' x $m); wu($m + 1, $v + 1) }
sub wse   { my ($v) = @_; wue($v > 0 ? 2 * $v - 1 : -2 * $v) }
sub wtail { push @bits, 1; push @bits, 0 while @bits % 8 }
sub take  { my $s = join '', @bits; @bits = ();
            die "payload not byte aligned\n" if length($s) % 8;
            my $b = ''; $b .= chr(oct("0b$_")) for $s =~ /(.{8})/g;
            return $b }

# ---- CAVLC codewords (checked against libavcodec/h264_cavlc.c) -------------
my %CT = (zero => '1', one_trail => '01', one_plain => '000101', tz1_0 => '1');
sub check_tables {
    open(my $c, '<', 'libavcodec/h264_cavlc.c') or die "open h264_cavlc.c: $!";
    local $/ = undef;
    my $src = <$c>;
    close $c;
    my ($rows) = $src =~ /static const uint8_t coeff_token_len\[4\]\[4\*17\]=\{\s*\{(.*?)\},/s
        or die "coeff_token_len table not found";
    my @len = $rows =~ /\d+/g;
    my ($brows) = $src =~ /static const uint8_t coeff_token_bits\[4\]\[4\*17\]=\{\s*\{(.*?)\},/s
        or die "coeff_token_bits table not found";
    my @val = $brows =~ /\d+/g;
    # symbol index = total_coeff * 4 + trailing_ones
    for my $sym ([0, 0, 'zero'], [1, 1, 'one_trail'], [1, 0, 'one_plain']) {
        my ($tc, $to, $key) = @$sym;
        my $i = $tc * 4 + $to;
        my $want = sprintf '%0*b', $len[$i], $val[$i];
        die "codeword drift for coeff_token($tc,$to): table says $want, script has $CT{$key}\n"
            unless $want eq $CT{$key};
    }
    my ($zl) = $src =~ /static const uint8_t total_zeros_len\[16\]\[16\]=\s*\{\s*\{(.*?)\},/s or die;
    my ($zb) = $src =~ /static const uint8_t total_zeros_bits\[16\]\[16\]=\s*\{\s*\{(.*?)\},/s or die;
    my @zl = $zl =~ /\d+/g; my @zb = $zb =~ /\d+/g;
    my $want = sprintf '%0*b', $zl[0], $zb[0];
    die "codeword drift for total_zeros(1,0): table says $want, script has $CT{tz1_0}\n"
        unless $want eq $CT{tz1_0};
    print "codeword self-check: ok (coeff_token ctx 0, total_zeros row 1)\n";
}
check_tables() unless $copy_only;

# The first coefficient of a block with no other coefficients and
# trailing_ones == 0 is coded as a unary prefix plus, for the long prefixes, a
# suffix.  The decoder adds 2 to the decoded level_code in this case, so:
#     level_code  2..15   -> prefix (level_code - 2) zeros + one bit
#     level_code 16..31   -> 14 zeros + one bit + 4-bit suffix (level_code-16)
#     level_code 32..4127 -> 15 zeros + one bit + 12-bit suffix (level_code-32)
# and the sign follows the parity: even level_code -> positive, odd -> negative.
sub write_level_code {
    my ($lc) = @_;
    if ($lc >= 2 && $lc <= 15) {
        wbits('0' x ($lc - 2)); push @bits, 1;
    } elsif ($lc >= 16 && $lc <= 31) {
        wbits('0' x 14); push @bits, 1; wu(4, $lc - 16);
    } elsif ($lc >= 32 && $lc <= 4127) {
        wbits('0' x 15); push @bits, 1; wu(12, $lc - 32);
    } else {
        die "cannot code level_code $lc\n";
    }
}

sub write_dc_block {
    my ($level) = @_;
    if ($level == 0) { wbits($CT{zero}); return }
    my $mag = abs($level);
    if ($mag == 1) {
        wbits($CT{one_trail});
        push @bits, $level < 0 ? 1 : 0;   # trailing one: bit 1 -> -1, bit 0 -> +1
    } else {
        wbits($CT{one_plain});
        write_level_code($level < 0 ? 2 * $mag - 1 : 2 * $mag - 2);
    }
    wbits($CT{tz1_0});
}

# ---- slice payload ---------------------------------------------------------
sub pick_level {
    my ($base, $alt, $idx) = @_;
    return defined($alt) && ($idx % 2) ? $alt : $base;
}

# A periodic IDR restarts the same 4-bit header fields on every GOP boundary.
# The base picture owns the IDR flag; the dependent picture of that access unit
# rides the same frame_num/pic_order_cnt_lsb and adopts the base view's refresh
# through the multiview POC latch.
sub au_frame_num { my ($au) = @_; return $idr_every ? $au % $idr_every : $au }
sub au_poc       { my ($au) = @_; return 2 * au_frame_num($au) }
sub au_is_idr    { my ($au) = @_; return $au == 0 || ($idr_every && ($au % $idr_every) == 0) }
sub au_idr_pic_id {
    my ($au) = @_;
    return $idr_every ? int($au / $idr_every) % 65536 : 0;
}

sub slice_payload {
    my (%o) = @_;                     # pps, frame_num, poc, idr, level, idr_pic_id
    wue(0);                           # first_mb_in_slice
    wue(2);                           # slice_type = I  (2, 7 or 12; the
                                      # golomb_to_pict_type table indexes by
                                      # value % 5, so 3 would be a P slice)
    wue($o{pps});                     # pic_parameter_set_id
    wu(4, $o{frame_num});             # frame_num (log2_max_frame_num_minus4 = 0)
    if ($o{idr}) {
        wue(defined($o{idr_pic_id}) ? $o{idr_pic_id} : 0); # idr_pic_id
        wu(4, $o{poc});               # pic_order_cnt_lsb
        push @bits, 0, 0;             # no_output_of_prior_pics_flag,
                                      # long_term_reference_flag
    } else {
        wu(4, $o{poc});               # pic_order_cnt_lsb
        push @bits, 0;                # adaptive_ref_pic_marking_mode_flag
    }
    wse(0);                           # slice_qp_delta -> QP = 26
    # The picture parameter sets request deblocking parameters in the slice
    # header: idc 0 (filter enabled) with zero offsets, as in the sibling
    # stream.
    wue(0);                           # slice_disable_deblocking_filter_idc
    wse(0);                           # slice_alpha_c0_offset_div2
    wse(0);                           # slice_beta_offset_div2
    wue(3);                           # mb_type: Intra_16x16, DC pred, CBP 0
    wue(0);                           # intra_chroma_pred_mode
    wse(0);                           # mb_qp_delta
    write_dc_block($o{level});        # Intra16x16 DC residual block
    wtail();                          # rbsp_trailing_bits
    return take();
}

sub base_picture {
    my ($au, $idr) = @_;
    return $copy_only
        ? ($idr ? $idr0->{raw} : $intra0->{raw})
        : escape(($idr ? "\x65" : "\x41")
                 . slice_payload(pps => 0, frame_num => au_frame_num($au),
                                 poc => au_poc($au), idr => $idr,
                                 idr_pic_id => au_idr_pic_id($au),
                                 level => pick_level($base_lvl, $base2_lvl, $au)));
}

sub dep_picture {
    my ($idx, $au) = @_;
    return $copy_only
        ? $mvcslice[$idx]{raw}
        : escape($mvhdr . slice_payload(pps => 1, frame_num => au_frame_num($au),
                                        poc => au_poc($au), idr => 0,
                                        level => pick_level($dep_lvl, $dep2_lvl, $idx)));
}

# ---- subtitle-depth (offset metadata) SEI ----------------------------------
# The BD3D offset-metadata message of the dependent view (see the header
# comment for the layout and where the format comes from).
my $ofmd_uuid = "\x17\xee\x8c\x60\xf8\x4d\x11\xd9" .
                "\x8c\xd6\x08\x00\x20\x0c\x9a\x66";

# signed pixels to wire byte: bit 7 is the direction flag (set = behind the
# screen) and bits 0..6 the magnitude, so 0x80 is the authored flat entry.
sub ofmd_entry {
    my ($v) = @_;
    return $v < 0 ? 0x80 | -$v : $v;
}

# what one entry of the block is on the wire, by offset sequence and by picture
# of the described group (see the four forms documented in the header comment).
# Both encodings of a flat entry are written: sequences 1 and 3 give the plain
# zero byte that a zero magnitude toward the viewer produces, sequence 2 the
# 0x80 a mastering tool authors, and a reader takes either as no displacement.
sub ofmd_wire {
    my ($seq, $frame) = @_;
    return $frame + 5             if $seq == 0;     #  05 06 07 ..
    return ofmd_entry($frame - 1) if $seq == 1;     #  81 00 01 ..
    return 0x80                   if $seq == 2;     #  80 80 80 ..
    return ofmd_entry($frame);                      #  00 01 02 ..
}

# a SEI payload size field: as many 0xFF bytes as the size holds 255s in it,
# then the remainder - the form both this project's streams and the readers of
# this message use
sub sei_size {
    my ($n) = @_;
    return "\xff" x int($n / 255) . chr($n % 255);
}

sub ofmd_sei_nal {
    my ($seq, $frames, $pts, $code, $pos0) = @_;
    my ($header, $table, $user_data, $nested, $payload) = ('', '', '', '', '');
    $pos0 = 0 unless $pos0;

    wbits('1000');                      # marker_bit + three reserved bits
    wu(4, $code);                       # frame_rate_code
    wbits('00000');                     # reserved
    wu(3, ($pts >> 30) & 0x7);          # pts[32:30]
    wbits('1');                         # marker_bit
    wu(15, ($pts >> 15) & 0x7fff);      # pts[29:15]
    wbits('1');                         # marker_bit
    wu(15,  $pts        & 0x7fff);      # pts[14:0]
    wbits('10');                        # the two marker bits
    wu(6, $seq);                        # sequence_count
    wu(8, $frames);                     # frame_count
    wu(8, 0x80);                        # the two opaque bytes, as authored
    wu(8, 0x80);
    die "subtitle depth header is not byte aligned\n" if @bits % 8;
    $header = take();

    for my $s (0 .. $seq - 1) {
        # in multi-block mode the ramp runs across the blocks: entry values say
        # which picture of the tiled run this block answers, not where inside
        # the block it sits
        $table .= chr(ofmd_wire($s, $pos0 + $_)) for 0 .. $frames - 1;
    }

    $user_data = $ofmd_uuid . 'OFMD' . $header . $table;
    $nested    = chr(5) . sei_size(length $user_data) . $user_data;
    # one-byte scalable-nesting header: no operation point, all views of the
    # access unit; the message it wraps is the user data above
    $payload   = "\x40" . $nested;

    return "\x06" . chr(37) . sei_size(length $payload) . $payload . "\x80";
}

# ---- assembly --------------------------------------------------------------

# The stream is: the parameter sets and the first access unit's delimiter and
# IDR picture, the subset SPS with the multiview extension, the dependent view's
# picture parameter set, the dependent picture of access unit 0, and then one
# access unit per index - delimiter, base picture, dependent picture - with the
# two view lists ending independently.
my $sc   = "\x00\x00\x01";
my $body = '';

# Multi-block mode: render each block's SEI once and place it at the delimiter
# of the dependent picture its pattern entry names (the bytes ride ahead of
# that picture, inside its access unit and after the base view's slice).
my @ofmd_sei_at;                # dependent picture index -> NAL bytes with delimiters
if ($ofmd && @ofmd_lags) {
    for my $g (0 .. $#ofmd_lags) {
        $ofmd_sei_at[$g * $ofmd_frames + $ofmd_lags[$g]] .=
            $sc . ofmd_sei_nal($ofmd_seq, $ofmd_frames, $ofmd_group_pts[$g],
                               $ofmd_rate, $g * $ofmd_frames);
    }
}

# prologue: everything up to the first dependent picture
for my $k (0 .. 5) {
    my $r = $role[$k];
    $body .= $sc . ($r eq 'idr0' ? base_picture(0, 1) : $nal[$k]{raw});
}
# the subtitle-depth SEI of the group, ahead of the first picture it describes
$body .= $sc . ofmd_sei_nal($ofmd_seq, $ofmd_frames, $ofmd_wire_pts, $ofmd_rate)
    if $ofmd && $dep_frames >= 1 && !@ofmd_lags;
# in multi-block mode only a block of lag 0 stands ahead of picture 0; the
# later-laid blocks were placed at their own slots above
$body .= $ofmd_sei_at[0] if $ofmd && @ofmd_lags && defined $ofmd_sei_at[0];
$body .= $sc . dep_picture(0, 0) if $dep_frames >= 1 && $view1_start == 0;

# the remaining access units, up to the later of the two view lists
for my $au (1 .. $max_au) {
    my $dep_idx = $au - $view1_start;

    $body .= $sc . $aud->{raw};
    $body .= $sc . base_picture($au, au_is_idr($au)) if $au < $base_frames;
    $body .= $ofmd_sei_at[$au] if $ofmd && @ofmd_lags && defined $ofmd_sei_at[$au];
    $body .= $sc . dep_picture($dep_idx, $au)
        if $dep_idx >= 0 && $dep_idx < $dep_frames;
}

open(my $of, '>:raw', $out) or die "open $out: $!";
print $of $body;
close $of;
printf "wrote %s: %d bytes (%d base + %d dependent pictures, view0 DC %d%s, view1 DC %d%s, view1 starts at AU %d%s%s)\n",
       $out, length($body), $base_frames, $dep_frames, $base_lvl,
       defined($base2_lvl) ? "/$base2_lvl" : "",
       $dep_lvl,
       defined($dep2_lvl) ? "/$dep2_lvl" : "",
       $view1_start,
       $idr_every ? ", base IDR every $idr_every" : "",
       $ofmd ? (@ofmd_lags
                ? sprintf(', subtitle depth: %d blocks x %d sequences x %d pictures from pts %d, rate code %d, SEI lags %s',
                          scalar(@ofmd_lags), $ofmd_seq, $ofmd_frames,
                          $ofmd_wire_pts, $ofmd_rate, join(',', @ofmd_lags))
                : sprintf(', subtitle depth: %d sequences x %d pictures at pts %d, rate code %d',
                          $ofmd_seq, $ofmd_frames, $ofmd_pts, $ofmd_rate))
             : '';

if ($copy_only || $zero_check) {
    my $what = $copy_only ? 'COPY' : 'ZERO';
    my $want = $body eq $dna;
    printf "%s-CHECK: %s\n", $what,
        $want ? "the stream reproduces $dnafile exactly"
              : "MISMATCH vs $dnafile";
    exit($want ? 0 : 1);
}
exit 0;

# ---- helpers ---------------------------------------------------------------
sub escape {
    my ($b) = @_;
    my ($o, $zeros) = ('', 0);
    for my $i (0 .. length($b) - 1) {
        my $v = ord(substr($b, $i, 1));
        if ($zeros >= 2 && $v <= 3) { $o .= "\x03"; $zeros = 0 }
        $o .= chr($v);
        $zeros = $v == 0 ? $zeros + 1 : 0;
    }
    return $o;
}

sub split_nals {
    my ($data) = @_;
    my @starts;
    for (my $p = 0; $p + 2 < length($data); $p++) {
        push @starts, $p + 3 if substr($data, $p, 3) eq "\x00\x00\x01";
    }
    my @nals;
    for my $k (0 .. $#starts) {
        my $s = $starts[$k];
        my $e = ($k < $#starts) ? $starts[$k + 1] - 3 : length($data);
        $e-- while $e - 1 > $s && substr($data, $e - 1, 1) eq "\x00";
        my $raw = substr($data, $s, $e - $s);
        my $b0  = ord(substr($raw, 0, 1));
        push @nals, { raw => $raw, type => $b0 & 0x1f, ref => ($b0 >> 5) & 3 };
    }
    return @nals;
}
