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
# pic_order_cnt_lsb (2 * the access unit index), which caps a view list at 7
# pictures: the generator refuses to write more than the headers it reuses can
# express.
#
# usage: mvc-mkfix.pl [--dna=FILE] [--out=FILE] [--base=N] [--dep=N]
#                     [--base-frames=N] [--dep-frames=N] [--copy] [--zero]
#   --base/--dep     signed Intra16x16 DC coefficient of view 0 / view 1
#   --base-frames    pictures carried by the base view (default 2)
#   --dep-frames     pictures carried by the dependent view (default 2)
#   --copy           re-emit the sibling stream verbatim (escaping self-check)
#   --zero           author zero-residual payloads at the sibling's picture
#                    counts and require the result to be the sibling exactly
#
# The --zero check is the reason this script is kept next to the fixtures it
# writes: authored payloads that reproduce a shipped sample byte for byte are
# payloads the shipped decoder agrees with.
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

for my $a (@ARGV) {
    $dnafile     = $1 if $a =~ /^--dna=(.+)$/;
    $out         = $1 if $a =~ /^--out=(.+)$/;
    $base_lvl    = $1 if $a =~ /^--base=(-?\d+)$/;
    $dep_lvl     = $1 if $a =~ /^--dep=(-?\d+)$/;
    $base_frames = $1 if $a =~ /^--base-frames=(\d+)$/;
    $dep_frames  = $1 if $a =~ /^--dep-frames=(\d+)$/;
    $copy_only   = 1  if $a eq '--copy';
    $zero_check  = 1  if $a eq '--zero';
}

die "a view list of $base_frames/$dep_frames pictures does not fit the 4-bit
     pic_order_cnt_lsb of the reused headers (7 pictures maximum)\n"
    if $base_frames > 7 || $dep_frames > 7;

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
sub slice_payload {
    my (%o) = @_;                     # pps, frame_num, poc, idr, level
    wue(0);                           # first_mb_in_slice
    wue(2);                           # slice_type = I  (2, 7 or 12; the
                                      # golomb_to_pict_type table indexes by
                                      # value % 5, so 3 would be a P slice)
    wue($o{pps});                     # pic_parameter_set_id
    wu(4, $o{frame_num});             # frame_num (log2_max_frame_num_minus4 = 0)
    if ($o{idr}) {
        wue(0);                       # idr_pic_id
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
                 . slice_payload(pps => 0, frame_num => $au, poc => 2 * $au,
                                 idr => $idr, level => $base_lvl));
}

sub dep_picture {
    my ($au) = @_;
    return $copy_only
        ? $mvcslice[$au]{raw}
        : escape($mvhdr . slice_payload(pps => 1, frame_num => $au,
                                        poc => 2 * $au, idr => 0,
                                        level => $dep_lvl));
}

# ---- assembly --------------------------------------------------------------
# The stream is: the parameter sets and the first access unit's delimiter and
# IDR picture, the subset SPS with the multiview extension, the dependent view's
# picture parameter set, the dependent picture of access unit 0, and then one
# access unit per index - delimiter, base picture, dependent picture - with the
# two view lists ending independently.
my $sc   = "\x00\x00\x01";
my $body = '';

# prologue: everything up to the first dependent picture
for my $k (0 .. 5) {
    my $r = $role[$k];
    $body .= $sc . ($r eq 'idr0' ? base_picture(0, 1) : $nal[$k]{raw});
}
$body .= $sc . dep_picture(0) if $dep_frames >= 1;

# the remaining access units, up to the longer of the two view lists
for my $au (1 .. ($base_frames >= $dep_frames ? $base_frames - 1 : $dep_frames - 1)) {
    $body .= $sc . $aud->{raw};
    $body .= $sc . base_picture($au, 0) if $au < $base_frames;
    $body .= $sc . dep_picture($au)     if $au < $dep_frames;
}

open(my $of, '>:raw', $out) or die "open $out: $!";
print $of $body;
close $of;
printf "wrote %s: %d bytes (%d base + %d dependent pictures, view0 DC %d, view1 DC %d)\n",
       $out, length($body), $base_frames, $dep_frames, $base_lvl, $dep_lvl;

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
