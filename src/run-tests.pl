#! /usr/bin/perl -w
#
# SPDX-FileCopyrightText: 2006-2022 Oswald Buddenhagen <ossi@users.sf.net>
# SPDX-License-Identifier: GPL-2.0-or-later
#

use warnings;
use strict;

use Carp;
$SIG{__WARN__} = \&Carp::cluck;
$SIG{__DIE__} = \&Carp::confess;

use Cwd;
use File::Path;
use File::Temp 'tempdir';

my $use_vg = $ENV{USE_VALGRIND};
my $use_st = $ENV{USE_STRACE};
my $mbsync = getcwd()."/mbsync";

my (@match, $start);
for my $arg (@ARGV) {
	if ($arg eq "+") {
		$start = 1;
	} else {
		push @match, $arg;
	}
}
die("Need exactly one test name when using start syntax.\n")
	if ($start && (@match != 1));

if (!-d "tmp") {
  unlink "tmp";
  my $tdir = tempdir();
  symlink $tdir, "tmp" or die "Cannot symlink temp directory: $!\n";
}
chdir "tmp" or die "Cannot enter temp direcory.\n";

sub test($$$$);

################################################################################

# Format of the test defs: [ far, near, state ]
# far/near: [ maxuid, { seq, uid, flags }... ]
# state: [ MaxPulledUid, MaxExpiredFarUid, MaxPushedUid, { muid, suid, flags }... ]

use enum qw(:=1 A..Z);
sub mn($) { my ($n) = @_; $n == 0 ? "0" : chr(64 + $n) }
sub mf($) { my ($f) = @_; length($f) ? $f : '-' }

# generic syncing tests
my @x01 = (
 [ 9,
   A, 1, "F", B, 2, "", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "F", G, 7, "FT", I, 9, "" ],
 [ 9,
   A, 1, "", B, 2, "F", C, 3, "F", D, 4, "", E, 5, "", G, 7, "", H, 8, "", J, 9, "" ],
 [ 8, 0, 0,
   1, 1, "", 2, 2, "", 3, 3, "", 4, 4, "", 5, 5, "", 6, 6, "", 7, 7, "", 8, 8, "" ],
);

my @O01 = ("", "", "");
my @X01 = (
 [ 10,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "FT", G, 7, "FT", I, 9, "", J, 10, "" ],
 [ 10,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", E, 5, "T", G, 7, "FT", H, 8, "T", J, 9, "", I, 10, "" ],
 [ 10, 0, 10,
   1, 1, "F", 2, 2, "F", 3, 3, "FS", 4, 4, "", 5, 5, "T", 6, 0, "", 7, 7, "FT", 0, 8, "", 10, 9, "", 9, 10, "" ],
);
test("full", \@x01, \@X01, \@O01);

my @O02 = ("", "", "Expunge Both\n");
my @X02 = (
 [ 10,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", I, 9, "", J, 10, "" ],
 [ 10,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", J, 9, "", I, 10, "" ],
 [ 10, 0, 10,
   1, 1, "F", 2, 2, "F", 3, 3, "FS", 4, 4, "", 10, 9, "", 9, 10, "" ],
);
test("full + expunge both", \@x01, \@X02, \@O02);

my @O03 = ("", "", "Expunge Near\n");
my @X03 = (
 [ 10,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "FT", G, 7, "FT", I, 9, "", J, 10, "" ],
 [ 10,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", J, 9, "", I, 10, "" ],
 [ 10, 0, 10,
   1, 1, "F", 2, 2, "F", 3, 3, "FS", 4, 4, "", 5, 0, "T", 6, 0, "", 7, 0, "T", 10, 9, "", 9, 10, "" ],
);
test("full + expunge near side", \@x01, \@X03, \@O03);

my @O04 = ("", "", "Sync Pull\n");
my @X04 = (
 [ 9,
   A, 1, "F", B, 2, "", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "F", G, 7, "FT", I, 9, "" ],
 [ 10,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", E, 5, "T", G, 7, "FT", H, 8, "T", J, 9, "", I, 10, "" ],
 [ 9, 0, 0,
   1, 1, "F", 2, 2, "", 3, 3, "FS", 4, 4, "", 5, 5, "T", 6, 6, "", 7, 7, "FT", 0, 8, "", 9, 10, "" ],
);
test("pull", \@x01, \@X04, \@O04);

my @O05 = ("", "", "Sync Flags\n");
my @X05 = (
 [ 9,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "F", G, 7, "FT", I, 9, "" ],
 [ 9,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", E, 5, "T", G, 7, "FT", H, 8, "", J, 9, "" ],
 [ 8, 0, 0,
   1, 1, "F", 2, 2, "F", 3, 3, "FS", 4, 4, "", 5, 5, "T", 6, 6, "", 7, 7, "FT", 8, 8, "" ],
);
test("flags", \@x01, \@X05, \@O05);

my @O06 = ("", "", "Sync Delete\n");
my @X06 = (
 [ 9,
   A, 1, "F", B, 2, "", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "FT", G, 7, "FT", I, 9, "" ],
 [ 9,
   A, 1, "", B, 2, "F", C, 3, "F", D, 4, "", E, 5, "", G, 7, "", H, 8, "T", J, 9, "" ],
 [ 8, 0, 0,
   1, 1, "", 2, 2, "", 3, 3, "", 4, 4, "", 5, 5, "", 6, 0, "", 7, 7, "", 0, 8, "" ],
);
test("deletions", \@x01, \@X06, \@O06);

my @O07 = ("", "", "Sync New\n");
my @X07 = (
 [ 10,
   A, 1, "F", B, 2, "", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "F", G, 7, "FT", I, 9, "", J, 10, "" ],
 [ 10,
   A, 1, "", B, 2, "F", C, 3, "F", D, 4, "", E, 5, "", G, 7, "", H, 8, "", J, 9, "", I, 10, "" ],
 [ 10, 0, 10,
   1, 1, "", 2, 2, "", 3, 3, "", 4, 4, "", 5, 5, "", 6, 6, "", 7, 7, "", 8, 8, "", 10, 9, "", 9, 10, "" ],
);
test("new", \@x01, \@X07, \@O07);

my @O08 = ("", "", "Sync PushFlags PullDelete\n");
my @X08 = (
 [ 9,
   A, 1, "F", B, 2, "F", C, 3, "FS", D, 4, "", E, 5, "T", F, 6, "F", G, 7, "FT", I, 9, "" ],
 [ 9,
   A, 1, "", B, 2, "F", C, 3, "F", D, 4, "", E, 5, "", G, 7, "", H, 8, "T", J, 9, "" ],
 [ 8, 0, 0,
   1, 1, "", 2, 2, "F", 3, 3, "F", 4, 4, "", 5, 5, "", 6, 6, "", 7, 7, "", 0, 8, "" ],
);
test("push flags + pull deletions", \@x01, \@X08, \@O08);

# size restriction tests

my @x10 = (
 [ 2,
   A, 1, "", B, 2, "*" ],
 [ 1,
   C, 1, "*" ],
 [ 0, 0, 0,
    ],
);

my @O11 = ("MaxSize 1k\n", "MaxSize 1k\n", "Expunge Near");
my @X11 = (
 [ 3,
   A, 1, "", B, 2, "*", C, 3, "?" ],
 [ 3,
   C, 1, "*", A, 2, "", B, 3, "?" ],
 [ 3, 0, 3,
   3, 1, "<", 1, 2, "", 2, 3, ">" ],
);
test("max size", \@x10, \@X11, \@O11);

my @x22 = (
 [ 3,
   A, 1, "", B, 2, "*", C, 3, "?" ],
 [ 3,
   C, 1, "F*", A, 2, "", B, 3, "F?" ],
 [ 3, 0, 3,
   3, 1, "<", 1, 2, "", 2, 3, ">" ],
);

my @X22 = (
 [ 4,
   A, 1, "", B, 2, "*", C, 3, "T?", C, 4, "F*" ],
 [ 4,
   C, 1, "F*", A, 2, "", B, 4, "*" ],
 [ 4, 0, 4,
   4, 1, "F", 3, 0, "T", 1, 2, "", 2, 4, "" ],
);
test("max size + flagging", \@x22, \@X22, \@O11);

my @x23 = (
 [ 2,
   A, 1, "", B, 2, "F*" ],
 [ 1,
   C, 1, "F*" ],
 [ 0, 0, 0,
    ],
);

my @X23 = (
 [ 3,
   A, 1, "", B, 2, "F*", C, 3, "F*" ],
 [ 3,
   C, 1, "F*", A, 2, "", B, 3, "F*" ],
 [ 3, 0, 3,
   3, 1, "F", 1, 2, "", 2, 3, "F" ]
);
test("max size + initial flagging", \@x23, \@X23, \@O11);

my @x24 = (
 [ 3,
   A, 1, "", B, 2, "*", C, 3, "F*" ],
 [ 1,
   A, 1, "" ],
 [ 3, 0, 1,
   1, 1, "", 2, 0, "^", 3, 0, "^" ],
);

my @X24 = (
 [ 3,
   A, 1, "", B, 2, "*", C, 3, "F*" ],
 [ 3,
   A, 1, "", B, 2, "?", C, 3, "F*" ],
 [ 3, 0, 3,
   1, 1, "", 2, 2, ">", 3, 3, "F" ],
);
test("max size (pre-1.4 legacy)", \@x24, \@X24, \@O11);

# expiration tests

my @x30 = (
 [ 6,
   A, 1, "F", B, 2, "", C, 3, "S", D, 4, "", E, 5, "S", F, 6, "" ],
 [ 0,
   ],
 [ 0, 0, 0,
    ],
);

my @O31 = ("", "", "MaxMessages 3\n");
my @X31 = (
 [ 6,
   A, 1, "F", B, 2, "", C, 3, "S", D, 4, "", E, 5, "S", F, 6, "" ],
 [ 5,
   A, 1, "F", B, 2, "", D, 3, "", E, 4, "S", F, 5, "" ],
 [ 6, 3, 5,
   1, 1, "F", 2, 2, "", 4, 3, "", 5, 4, "S", 6, 5, "" ],
);
test("max messages", \@x30, \@X31, \@O31);

my @O32 = ("", "", "MaxMessages 3\nExpireUnread yes\n");
my @X32 = (
 [ 6,
   A, 1, "F", B, 2, "", C, 3, "S", D, 4, "", E, 5, "S", F, 6, "" ],
 [ 4,
   A, 1, "F", D, 2, "", E, 3, "S", F, 4, "" ],
 [ 6, 3, 4,
   1, 1, "F", 4, 2, "", 5, 3, "S", 6, 4, "" ],
);
test("max messages vs. unread", \@x30, \@X32, \@O32);

my @x50 = (
 [ 6,
   A, 1, "FS", B, 2, "FS", C, 3, "S", D, 4, "", E, 5, "", F, 6, "" ],
 [ 6,
   A, 1, "S", B, 2, "ST", D, 4, "", E, 5, "", F, 6, "" ],
 [ 6, 3, 0,
   1, 1, "FS", 2, 2, "~S", 3, 3, "~S", 4, 4, "", 5, 5, "", 6, 6, "" ],
);

my @O51 = ("", "", "MaxMessages 3\nExpunge Both\n");
my @X51 = (
 [ 6,
   A, 1, "S", B, 2, "FS", C, 3, "S", D, 4, "", E, 5, "", F, 6, "" ],
 [ 6,
   B, 2, "FS", D, 4, "", E, 5, "", F, 6, "" ],
 [ 6, 3, 6,
   2, 2, "FS", 4, 4, "", 5, 5, "", 6, 6, "" ],
);
test("max messages + expunge", \@x50, \@X51, \@O51);


################################################################################

print "OK.\n";
exit 0;

sub parse_box($)
{
	my ($rbs) = @_;

	my $mu = $$rbs[0];
	my %ms;
	for (my $i = 1; $i < @$rbs; $i += 3) {
		$ms{$$rbs[$i + 1]} = [ $$rbs[$i], $$rbs[$i + 2] ];
	}
	return {
		max_uid => $mu,
		messages => \%ms  #  { uid => [ subject, flags ], ... }
	};
}

sub parse_state($)
{
	my ($rss) = @_;

	my @ents;
	for (my $i = 3; $i < @$rss; $i += 3) {
		push @ents, [ @{$rss}[$i .. $i+2] ];
	}
	return {
		max_pulled => $$rss[0],
		max_expired => $$rss[1],
		max_pushed => $$rss[2],
		entries => \@ents  # [ [ far_uid, near_uid, flags ], ... ]
	};
}

sub parse_chan($)
{
	my ($cs) = @_;

	return {
		far => parse_box($$cs[0]),
		near => parse_box($$cs[1]),
		state => parse_state($$cs[2])
	};
}

sub qm($)
{
	shift;
	s/\\/\\\\/g;
	s/\"/\\"/g;
	s/\"/\\"/g;
	s/\n/\\n/g;
	return $_;
}

# [ $far, $near, $channel ]
sub writecfg($)
{
	my ($sfx) = @_;

	open(FILE, ">", ".mbsyncrc") or
		die "Cannot open .mbsyncrc.\n";
	print FILE
"FSync no

MaildirStore far
Path ./
Inbox ./far
".$$sfx[0]."
MaildirStore near
Path ./
Inbox ./near
".$$sfx[1]."
Channel test
Far :far:
Near :near:
SyncState *
".$$sfx[2];
	close FILE;
}

sub killcfg()
{
	unlink $_ for (glob("*.log"));
	unlink ".mbsyncrc";
}

# $run_async, $mbsync_options, $log_file
# Return: $exit_code, \@mbsync_output
sub runsync($$$)
{
	my ($async, $flags, $file) = @_;

	my $cmd;
	if ($use_vg) {
		$cmd = "valgrind -q --error-exitcode=1 ";
	} elsif ($use_st) {
		$cmd = "strace ";
	} else {
		$flags .= " -D";
	}
	$flags .= " -Ta" if ($async);
	$cmd .= "$mbsync -Tz $flags -c .mbsyncrc test";
	open FILE, "$cmd 2>&1 |";
	my @out = <FILE>;
	close FILE or push(@out, $! ? "*** error closing mbsync: $!\n" : "*** mbsync exited with signal ".($?&127).", code ".($?>>8)."\n");
	if ($file) {
		open FILE, ">$file" or die("Cannot create $file: $!\n");
		print FILE @out;
		close FILE;
	}
	return $?, \@out;
}


use constant CHOMP => 1;

sub readfile($;$)
{
	my ($file, $chomp) = @_;

	open(FILE, $file) or return;
	my @nj = <FILE>;
	close FILE;
	chomp(@nj) if ($chomp);
	return \@nj;
}

# $path
sub readbox($)
{
	my $bn = shift;

	(-d $bn) or
		die "No mailbox '$bn'.\n";
	(-d $bn."/tmp" and -d $bn."/new" and -d $bn."/cur") or
		die "Invalid mailbox '$bn'.\n";
	my $uidval = readfile($bn."/.uidvalidity", CHOMP);
	die "Cannot read UID validity of mailbox '$bn': $!\n" if (!$uidval);
	my $mu = $$uidval[1];
	my %ms = ();
	for my $d ("cur", "new") {
		opendir(DIR, $bn."/".$d) or next;
		for my $f (grep(!/^\.\.?$/, readdir(DIR))) {
			my ($uid, $flg, $ph, $num);
			if ($f =~ /^\d+\.\d+_\d+\.[-[:alnum:]]+,U=(\d+):2,(.*)$/) {
				($uid, $flg) = (int($1), $2);
			} else {
				print STDERR "unrecognided file name '$f' in '$bn'.\n";
				exit 1;
			}
			open(FILE, "<", $bn."/".$d."/".$f) or die "Cannot read message '$f' in '$bn'.\n";
			my $sz = 0;
			while (<FILE>) {
				/^Subject: (\[placeholder\] )?(\d+)$/ && ($ph = defined($1), $num = int($2));
				$sz += length($_);
			}
			close FILE;
			if (!defined($num)) {
				print STDERR "message '$f' in '$bn' has no identifier.\n";
				exit 1;
			}
			@{ $ms{$uid} } = ($num, $flg.($sz>1000?"*":"").($ph?"?":""));
		}
	}
	return { max_uid => $mu, messages => \%ms };
}

# \%fallback_sync_state
sub readstate(;$)
{
	my ($fbss) = @_;

	my $fn = "near/.mbsyncstate";
	if ($fbss) {
		$fn .= ".new";
		return $fbss if (!-s $fn);
	}
	my $ls = readfile($fn, CHOMP);
	if (!$ls) {
		print STDERR "Cannot read sync state $fn: $!\n";
		return;
	}
	my @ents;
	my %ss = (
		max_pulled => 0,
		max_expired => 0,
		max_pushed => 0,
		entries => \@ents
	);
	my ($far_val, $near_val) = (0, 0);
	my %hdr = (
		'FarUidValidity' => \$far_val,
		'NearUidValidity' => \$near_val,
		'MaxPulledUid' => \$ss{max_pulled},
		'MaxPushedUid' => \$ss{max_pushed},
		'MaxExpiredFarUid' => \$ss{max_expired}
	);
	OUTER: while (1) {
		while (@$ls) {
			$_ = shift(@$ls);
			last OUTER if (!length($_));
			if (!/^([^ ]+) (\d+)$/) {
				print STDERR "Malformed sync state header entry: $_\n";
				return;
			}
			my $want = delete $hdr{$1};
			if (!defined($want)) {
				print STDERR "Unexpected sync state header entry: $1\n";
				return;
			}
			$$want = int($2);
		}
		print STDERR "Unterminated sync state header.\n";
		return;
	}
	delete $hdr{'MaxExpiredFarUid'};  # optional field
	my @ky = keys %hdr;
	if (@ky) {
		print STDERR "Keys missing from sync state header: @ky\n";
		return;
	}
	if ($far_val ne '1' or $near_val ne '1') {
		print STDERR "Unexpected UID validity $far_val $near_val (instead of 1 1)\n";
		return;
	}
	for (@$ls) {
		if (!/^(\d+) (\d+) (.*)$/) {
			print STDERR "Malformed sync state entry: $_\n";
			return;
		}
		push @ents, [ int($1), int($2), $3 ];
	}
	return \%ss;
}

# \%fallback_sync_state
sub readchan(;$)
{
	my ($fbss) = @_;

	return {
		far => readbox("far"),
		near => readbox("near"),
		state => readstate($fbss)
	};
}

# $box_name, \%box_state
sub mkbox($$)
{
	my ($bn, $bs) = @_;

	rmtree($bn);
	(mkdir($bn) and mkdir($bn."/tmp") and mkdir($bn."/new") and mkdir($bn."/cur")) or
		die "Cannot create mailbox $bn.\n";
	open(FILE, ">", $bn."/.uidvalidity") or die "Cannot create UID validity for mailbox $bn.\n";
	print FILE "1\n$$bs{max_uid}\n";
	close FILE;
	my $ms = $$bs{messages};
	for my $uid (keys %$ms) {
		my ($num, $flg) = @{$$ms{$uid}};
		my $big = $flg =~ s/\*//;
		my $ph = $flg =~ s/\?//;
		open(FILE, ">", $bn."/".($flg =~ /S/ ? "cur" : "new")."/0.1_".$num.".local,U=".$uid.":2,".$flg) or
			die "Cannot create message ".mn($num)." in mailbox $bn.\n";
		print FILE "From: foo\nTo: bar\nDate: Thu, 1 Jan 1970 00:00:00 +0000\nSubject: ".($ph?"[placeholder] ":"").$num."\n\n".(("A"x50)."\n")x($big*30);
		close FILE;
	}
}

# \%sync_state
sub mkstate($)
{
	my ($ss) = @_;

	open(FILE, ">", "near/.mbsyncstate") or
		die "Cannot create sync state.\n";
	print FILE "FarUidValidity 1\nMaxPulledUid ".$$ss{max_pulled}."\n".
	           "NearUidValidity 1\nMaxExpiredFarUid ".$$ss{max_expired}.
	           "\nMaxPushedUid ".$$ss{max_pushed}."\n\n";
	for my $ent (@{$$ss{entries}}) {
		print FILE $$ent[0]." ".$$ent[1]." ".$$ent[2]."\n";
	}
	close FILE;
}

# \%chan_state
sub mkchan($)
{
	my ($cs) = @_;

	mkbox("far", $$cs{far});
	mkbox("near", $$cs{near});
	mkstate($$cs{state});
}

# $box_name, \%actual_box_state, \%reference_box_state
sub cmpbox($$$)
{
	my ($bn, $bs, $ref_bs) = @_;

	my $ret = 0;
	my ($ref_mu, $ref_ms) = ($$ref_bs{max_uid}, $$ref_bs{messages});
	my ($mu, $ms) = ($$bs{max_uid}, $$bs{messages});
	if ($mu != $ref_mu) {
		print STDERR "MAXUID mismatch for '$bn': got $mu, wanted $ref_mu\n";
		$ret = 1;
	}
	for my $uid (sort { $a <=> $b } keys %$ref_ms) {
		my ($num, $flg) = @{$$ref_ms{$uid}};
		my $m = $$ms{$uid};
		if (!defined $m) {
			print STDERR "Missing message $bn:$uid:".mn($num)."\n";
			$ret = 1;
			next;
		}
		if ($$m[0] != $num) {
			print STDERR "Subject mismatch for $bn:$uid:".
					" got ".mn($$m[0]).", wanted ".mn($num)."\n";
			return 1;
		}
		if ($$m[1] ne $flg) {
			print STDERR "Flag mismatch for $bn:$uid:".mn($num).":".
					" got ".mf($$m[1]).", wanted ".mf($flg)."\n";
			$ret = 1;
		}
	}
	for my $uid (sort { $a <=> $b } keys %$ms) {
		if (!defined($$ref_ms{$uid})) {
			print STDERR "Excess message $bn:$uid:".mn($$ms{$uid}[0])."\n";
			$ret = 1;
		}
	}
	return $ret;
}

sub mapmsg($$)
{
	my ($uid, $bs) = @_;

	if ($uid) {
		if (my $msg = $$bs{messages}{$uid}) {
			return $$msg[0];
		}
	}
	return 0;
}

# \%actual_chan_state, \%reference_chan_state
sub cmpstate($$)
{
	my ($cs, $ref_cs) = @_;

	my ($ss, $fbs, $nbs) = ($$cs{state}, $$cs{far}, $$cs{near});
	return 1 if (!$ss);
	my ($ref_ss, $ref_fbs, $ref_nbs) = ($$ref_cs{state}, $$ref_cs{far}, $$ref_cs{near});
	return 0 if ($ss == $ref_ss);
	my $ret = 0;
	for my $h (['MaxPulledUid', 'max_pulled'],
	           ['MaxExpiredFarUid', 'max_expired'],
	           ['MaxPushedUid', 'max_pushed']) {
		my ($hn, $sn) = @$h;
		my ($got, $want) = ($$ss{$sn}, $$ref_ss{$sn});
		if ($got != $want) {
			print STDERR "Sync state header entry $hn mismatch: got $got, wanted $want\n";
			$ret = 1;
		}
	}
	my $ref_ents = $$ref_ss{entries};
	my $ents = $$ss{entries};
	for (my $i = 0; $i < @$ents || $i < @$ref_ents; $i++) {
		my ($ent, $fuid, $nuid, $num);
		if ($i < @$ents) {
			$ent = $$ents[$i];
			($fuid, $nuid) = ($$ent[0], $$ent[1]);
			my ($fnum, $nnum) = (mapmsg($fuid, $fbs), mapmsg($nuid, $nbs));
			if ($fnum && $nnum && $fnum != $nnum) {
				print STDERR "Invalid sync state entry $fuid:$nuid:".
						" mismatched subjects (".mn($fnum).":".mn($nnum).")\n";
				return 1;
			}
			$num = $fnum || $nnum;
		}
		if ($i == @$ref_ents) {
			print STDERR "Excess sync state entry $fuid:$nuid (".mn($num).")\n";
			return 1;
		}
		my $rent = $$ref_ents[$i];
		my ($rfuid, $rnuid) = ($$rent[0], $$rent[1]);
		my $rnum = mapmsg($rfuid, $ref_fbs) || mapmsg($rnuid, $ref_nbs);
		if ($i == @$ents) {
			print STDERR "Missing sync state entry $rfuid:$rnuid (".mn($rnum).")\n";
			return 1;
		}
		if ($fuid != $rfuid || $nuid != $rnuid || $num != $rnum) {
			print STDERR "Unexpected sync state entry:".
					" got $fuid:$nuid (".mn($num)."), wanted $rfuid:$rnuid (".mn($rnum).")\n";
			return 1;
		}
		if ($$ent[2] ne $$rent[2]) {
			print STDERR "Flag mismatch in sync state entry $fuid:$nuid (".mn($rnum)."):".
					" got ".mf($$ent[2]).", wanted ".mf($$rent[2])."\n";
			$ret = 1;
		}
	}
	return $ret;
}

# \%actual_chan_state, \%reference_chan_state
sub cmpchan($$)
{
	my ($cs, $ref_cs) = @_;

	my $rslt = 0;
	$rslt |= cmpbox("far", $$cs{far}, $$ref_cs{far});
	$rslt |= cmpbox("near", $$cs{near}, $$ref_cs{near});
	$rslt |= cmpstate($cs, $ref_cs);
	return $rslt;
}

# \%box_state
sub printbox($)
{
	my ($bs) = @_;

	my ($mu, $ms) = ($$bs{max_uid}, $$bs{messages});
	print " [ $mu,\n   ";
	my $frst = 1;
	for my $uid (sort { $a <=> $b } keys %$ms) {
		my ($num, $flg) = @{$$ms{$uid}};
		if ($frst) {
			$frst = 0;
		} else {
			print ", ";
		}
		print mn($num).", ".$uid.", \"".$flg."\"";
	}
	print " ],\n";
}

# \%sync_state
sub printstate($)
{
	my ($ss) = @_;

	return if (!$ss);
	print " [ ".$$ss{max_pulled}.", ".$$ss{max_expired}.", ".$$ss{max_pushed}.",\n   ";
	my $frst = 1;
	for my $ent (@{$$ss{entries}}) {
		if ($frst) {
			$frst = 0;
		} else {
			print ", ";
		}
		print(($$ent[0] // "??").", ".($$ent[1] // "??").", \"".($$ent[2] // "??")."\"");
	}
	print " ],\n";
}

# \%chan_state
sub printchan($)
{
	my ($cs) = @_;

	printbox($$cs{far});
	printbox($$cs{near});
	printstate($$cs{state});
}

# $run_async, \%source_state, \%target_state, \@channel_configs
sub test_impl($$$$)
{
	my ($async, $sx, $tx, $sfx) = @_;

	mkchan($sx);

	my ($xc, $ret) = runsync($async, "-Tj", "1-initial.log");
	my $rtx = readchan($$sx{state}) if (!$xc);
	if ($xc || cmpchan($rtx, $tx)) {
		print "Input:\n";
		printchan($sx);
		print "Options:\n";
		print " [ ".join(", ", map('"'.qm($_).'"', @$sfx))." ]\n";
		if (!$xc) {
			print "Expected result:\n";
			printchan($tx);
			print "Actual result:\n";
			printchan($rtx);
		}
		print "Debug output:\n";
		print @$ret;
		exit 1;
	}

	my ($nj, $njl) = (undef, 0);
	if ($$rtx{state} != $$sx{state}) {
		$nj = readfile("near/.mbsyncstate.journal");
		$njl = (@$nj - 1) * 2;

		my ($jxc, $jret) = runsync($async, "-0 --no-expunge", "2-replay.log");
		my $jrcs = readstate() if (!$jxc);
		if ($jxc || cmpstate({ far => $$tx{far}, near => $$tx{near}, state => $jrcs }, $tx)) {
			print "Journal replay failed.\n";
			print "Options:\n";
			print " [ ".join(", ", map('"'.qm($_).'"', @$sfx))." ], [ \"-0\", \"--no-expunge\" ]\n";
			print "Old State:\n";
			printstate($$sx{state});
			print "Journal:\n".join("", @$nj)."\n";
			if (!$jxc) {
				print "Expected New State:\n";
				printstate($$tx{state});
				print "New State:\n";
				printstate($jrcs);
			}
			print "Debug output:\n";
			print @$jret;
			exit 1;
		}
	}

	my ($ixc, $iret) = runsync($async, "", "3-verify.log");
	my $irtx = readchan() if (!$ixc);
	if ($ixc || cmpchan($irtx, $tx)) {
		print "Idempotence verification run failed.\n";
		print "Input == Expected result:\n";
		printchan($tx);
		print "Options:\n";
		print " [ ".join(", ", map('"'.qm($_).'"', @$sfx))." ]\n";
		if (!$ixc) {
			print "Actual result:\n";
			printchan($irtx);
		}
		print "Debug output:\n";
		print @$iret;
		exit 1;
	}

	rmtree "near";
	rmtree "far";

	for (my $l = 1; $l <= $njl; $l++) {
		mkchan($sx);

		my ($nxc, $nret) = runsync($async, "-Tj$l", "4-interrupt.log");
		if ($nxc != (100 + ($l & 1)) << 8) {
			print "Interrupting at step $l/$njl failed.\n";
			print "Debug output:\n";
			print @$nret;
			exit 1;
		}

		($nxc, $nret) = runsync($async, "-Tj", "5-resume.log");
		my $nrtx = readchan($$sx{state}) if (!$nxc);
		if ($nxc || cmpchan($nrtx, $tx)) {
			print "Resuming from step $l/$njl failed.\n";
			print "Input:\n";
			printchan($sx);
			print "Options:\n";
			print " [ ".join(", ", map('"'.qm($_).'"', @$sfx))." ]\n";
			my $nnj = readfile("near/.mbsyncstate.journal");
			my $ln = int($l / 2);
			print "Journal:\n".join("", @$nnj[0..$ln])."-------\n".join("", @$nnj[($ln + 1)..$#$nnj])."\n";
			print "Full journal:\n".join("", @$nj)."\n";
			if (!$nxc) {
				print "Expected result:\n";
				printchan($tx);
				print "Actual result:\n";
				printchan($nrtx);
			}
			print "Debug output:\n";
			print @$nret;
			exit 1;
		}

		rmtree "near";
		rmtree "far";
	}
}

# $title, \@source_state, \@target_state, \@channel_configs
sub test($$$$)
{
	my ($ttl, $isx, $itx, $sfx) = @_;

	if (@match) {
		if ($start) {
			return if (index($ttl, $match[0]) < 0);
			@match = ();
		} else {
			return if (!grep { index($ttl, $_) >= 0 } @match);
		}
	}

	print "Testing: ".$ttl." ...\n";
	writecfg($sfx);

	my $sx = parse_chan($isx);
	my $tx = parse_chan($itx);

	test_impl(0, $sx, $tx, $sfx);
	test_impl(1, $sx, $tx, $sfx);

	killcfg();
}
