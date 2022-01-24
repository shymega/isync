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

sub show($$$);
sub test($$$$);

################################################################################

# Format of the test defs: [ far, near, state ]
# far/near: [ maxuid, { seq, uid, flags }... ]
# state: [ MaxPulledUid, MaxExpiredFarUid, MaxPushedUid, { muid, suid, flags }... ]

use enum qw(:=1 A..Z);
sub mn($) { chr(64 + shift) }

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
#show("01", "01", "01");
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
#show("01", "02", "02");
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
#show("01", "03", "03");
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
#show("01", "04", "04");
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
#show("01", "05", "05");
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
#show("01", "06", "06");
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
#show("01", "07", "07");
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
#show("01", "08", "08");
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
#show("10", "11", "11");
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

#show("22", "22", "11");
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
#show("30", "31", "31");
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
#show("30", "32", "32");
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
#show("50", "51", "51");
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
# Return: $max_uid, { uid => [ seq, flags ] }
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
				($uid, $flg) = ($1, $2);
			} else {
				print STDERR "unrecognided file name '$f' in '$bn'.\n";
				exit 1;
			}
			open(FILE, "<", $bn."/".$d."/".$f) or die "Cannot read message '$f' in '$bn'.\n";
			my $sz = 0;
			while (<FILE>) {
				/^Subject: (\[placeholder\] )?(\d+)$/ && ($ph = defined($1), $num = $2);
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
	return $mu, \%ms;
}

# $filename
# Return: [ $max_pulled, $max_expired_far, $max_pushed, (far_uid, near_uid, flags), ... ]
sub readstate($)
{
	my ($fn) = @_;

	my $ls = readfile($fn, CHOMP);
	if (!$ls) {
		print STDERR "Cannot read sync state $fn: $!\n";
		return;
	}
	my ($far_val, $near_val) = (0, 0);
	my ($max_pull, $max_push, $max_exp) = (0, 0, 0);
	my %hdr = (
		'FarUidValidity' => \$far_val,
		'NearUidValidity' => \$near_val,
		'MaxPulledUid' => \$max_pull,
		'MaxPushedUid' => \$max_push,
		'MaxExpiredFarUid' => \$max_exp
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
			$$want = $2;
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
	my @T = ($max_pull, $max_exp, $max_push);
	for (@$ls) {
		if (!/^(\d+) (\d+) (.*)$/) {
			print STDERR "Malformed sync state entry: $_\n";
			return;
		}
		push @T, $1, $2, $3;
	}
	return \@T;
}

# $boxname
# Output:
# [ maxuid,
#   serial, uid, "flags", ... ],
sub showbox($)
{
	my ($bn) = @_;

	my ($mu, $ms) = readbox($bn);
	my @bc = ($mu);
	for my $uid (sort { $a <=> $b } keys %$ms) {
		push @bc, $$ms{$uid}[0], $uid, $$ms{$uid}[1];
	}
	printbox(\@bc);
}

# $filename
# Output:
# [ maxuid[F], maxxfuid, maxuid[N],
#   uid[F], uid[N], "flags", ... ],
sub showstate($)
{
	my ($fn) = @_;

	my $rss = readstate($fn);
	printstate($rss) if ($rss);
}

# $filename
sub showchan($)
{
	my ($fn) = @_;

	showbox("far");
	showbox("near");
	showstate($fn);
}

# $source_state_name, $target_state_name, $configs_name
sub show($$$)
{
	my ($sx, $tx, $sfxn) = @_;
	my ($sp, $sfx);
	eval "\$sp = \\\@x$sx";
	eval "\$sfx = \\\@O$sfxn";
	mkchan($sp);
	print "my \@x$sx = (\n";
	showchan("near/.mbsyncstate");
	print ");\n";
	writecfg($sfx);
	runsync(0, "", "");
	killcfg();
	print "my \@X$tx = (\n";
	showchan("near/.mbsyncstate");
	print ");\n";
	print "test(\"\", \\\@x$sx, \\\@X$tx, \\\@O$sfxn);\n\n";
	rmtree "near";
	rmtree "far";
}

# $box_name, \@box_state
sub mkbox($$)
{
	my ($bn, $bs) = @_;

	rmtree($bn);
	(mkdir($bn) and mkdir($bn."/tmp") and mkdir($bn."/new") and mkdir($bn."/cur")) or
		die "Cannot create mailbox $bn.\n";
	open(FILE, ">", $bn."/.uidvalidity") or die "Cannot create UID validity for mailbox $bn.\n";
	print FILE "1\n$$bs[0]\n";
	close FILE;
	for (my $i = 1; $i < @$bs; $i += 3) {
		my ($num, $uid, $flg) = ($$bs[$i], $$bs[$i + 1], $$bs[$i + 2]);
		my $big = $flg =~ s/\*//;
		my $ph = $flg =~ s/\?//;
		open(FILE, ">", $bn."/".($flg =~ /S/ ? "cur" : "new")."/0.1_".$num.".local,U=".$uid.":2,".$flg) or
			die "Cannot create message ".mn($num)." in mailbox $bn.\n";
		print FILE "From: foo\nTo: bar\nDate: Thu, 1 Jan 1970 00:00:00 +0000\nSubject: ".($ph?"[placeholder] ":"").$num."\n\n".(("A"x50)."\n")x($big*30);
		close FILE;
	}
}

# \@state
sub mkstate($)
{
	my ($t) = @_;

	open(FILE, ">", "near/.mbsyncstate") or
		die "Cannot create sync state.\n";
	print FILE "FarUidValidity 1\nMaxPulledUid ".$$t[0]."\n".
	           "NearUidValidity 1\nMaxExpiredFarUid ".$$t[1]."\nMaxPushedUid ".$$t[2]."\n\n";
	for (my $i = 3; $i < @$t; $i += 3) {
		print FILE $$t[$i]." ".$$t[$i + 1]." ".$$t[$i + 2]."\n";
	}
	close FILE;
}

# \@chan_state
sub mkchan($)
{
	my ($cs) = @_;

	my ($f, $n, $t) = @$cs;
	mkbox("far", $f);
	mkbox("near", $n);
	mkstate($t);
}

# $box_name, \@box_state
sub ckbox($$)
{
	my ($bn, $bs) = @_;

	my ($mu, $ms) = readbox($bn);
	if ($mu != $$bs[0]) {
		print STDERR "MAXUID mismatch for '$bn' (got $mu, wanted $$bs[0]).\n";
		return 1;
	}
	for (my $i = 1; $i < @$bs; $i += 3) {
		my ($num, $uid, $flg) = ($$bs[$i], $$bs[$i + 1], $$bs[$i + 2]);
		my $m = delete $$ms{$uid};
		if (!defined $m) {
			print STDERR "No message $bn:$uid.\n";
			return 1;
		}
		if ($$m[0] ne $num) {
			print STDERR "Subject mismatch for $bn:$uid.\n";
			return 1;
		}
		if ($$m[1] ne $flg) {
			print STDERR "Flag mismatch for $bn:$uid.\n";
			return 1;
		}
	}
	if (%$ms) {
		print STDERR "Excess messages in '$bn': ".join(", ", sort({ $a <=> $b } keys(%$ms))).".\n";
		return 1;
	}
	return 0;
}

# $state_file, \@sync_state
sub ckstate($$)
{
	my ($fn, $t) = @_;

	my $ss = readstate($fn);
	return 1 if (!$ss);
	my @hn = ('MaxPulledUid', 'MaxExpiredFarUid', 'MaxPushedUid');
	for my $h (0 .. 2) {
		my ($got, $want) = ($$ss[$h], $$t[$h]);
		if ($got ne $want) {
			print STDERR "Sync state header entry $hn[$h] mismatch: got $got, wanted $want\n";
			return 1;
		}
	}
	my $i = 3;
	while ($i < @$ss) {
		my $l = $$ss[$i]." ".$$ss[$i + 1]." ".$$ss[$i + 2];
		if ($i == @$t) {
			print STDERR "Excess sync state entry: '$l'.\n";
			return 1;
		}
		my $xl = $$t[$i]." ".$$t[$i + 1]." ".$$t[$i + 2];
		if ($l ne $xl) {
			print STDERR "Sync state entry mismatch: '$l' instead of '$xl'.\n";
			return 1;
		}
		$i += 3;
	}
	if ($i < @$t) {
		print STDERR "Missing sync state entry: '".$$t[$i]." ".$$t[$i + 1]." ".$$t[$i + 2]."'.\n";
		return 1;
	}
	return 0;
}

# $state_file, \@chan_state
sub ckchan($$)
{
	my ($fn, $cs) = @_;
	my $rslt = ckstate($fn, $$cs[2]);
	$rslt |= ckbox("far", $$cs[0]);
	$rslt |= ckbox("near", $$cs[1]);
	return $rslt;
}

# \@box_state
sub printbox($)
{
	my ($bs) = @_;

	print " [ $$bs[0],\n   ";
	my $frst = 1;
	for (my $i = 1; $i < @$bs; $i += 3) {
		if ($frst) {
			$frst = 0;
		} else {
			print ", ";
		}
		print mn($$bs[$i]).", ".$$bs[$i + 1].", \"".$$bs[$i + 2]."\"";
	}
	print " ],\n";
}

# \@sync_state
sub printstate($)
{
	my ($t) = @_;

	print " [ ".$$t[0].", ".$$t[1].", ".$$t[2].",\n   ";
	my $frst = 1;
	for (my $i = 3; $i < @$t; $i += 3) {
		if ($frst) {
			$frst = 0;
		} else {
			print ", ";
		}
		print(($$t[$i] // "??").", ".($$t[$i + 1] // "??").", \"".($$t[$i + 2] // "??")."\"");
	}
	print " ],\n";
}

# \@chan_state
sub printchan($)
{
	my ($cs) = @_;

	printbox($$cs[0]);
	printbox($$cs[1]);
	printstate($$cs[2]);
}

# $run_async, \@source_state, \@target_state, \@channel_configs
sub test_impl($$$$)
{
	my ($async, $sx, $tx, $sfx) = @_;

	mkchan($sx);

	my ($xc, $ret) = runsync($async, "-Tj", "1-initial.log");
	if ($xc || ckchan("near/.mbsyncstate.new", $tx)) {
		print "Input:\n";
		printchan($sx);
		print "Options:\n";
		print " [ ".join(", ", map('"'.qm($_).'"', @$sfx))." ]\n";
		if (!$xc) {
			print "Expected result:\n";
			printchan($tx);
			print "Actual result:\n";
			showchan("near/.mbsyncstate.new");
		}
		print "Debug output:\n";
		print @$ret;
		exit 1;
	}

	my $nj = readfile("near/.mbsyncstate.journal");
	my ($jxc, $jret) = runsync($async, "-0 --no-expunge", "2-replay.log");
	if ($jxc || ckstate("near/.mbsyncstate", $$tx[2])) {
		print "Journal replay failed.\n";
		print "Options:\n";
		print " [ ".join(", ", map('"'.qm($_).'"', @$sfx))." ], [ \"-0\", \"--no-expunge\" ]\n";
		print "Old State:\n";
		printstate($$sx[2]);
		print "Journal:\n".join("", @$nj)."\n";
		if (!$jxc) {
			print "Expected New State:\n";
			printstate($$tx[2]);
			print "New State:\n";
			showstate("near/.mbsyncstate");
		}
		print "Debug output:\n";
		print @$jret;
		exit 1;
	}

	my ($ixc, $iret) = runsync($async, "", "3-verify.log");
	if ($ixc || ckchan("near/.mbsyncstate", $tx)) {
		print "Idempotence verification run failed.\n";
		print "Input == Expected result:\n";
		printchan($tx);
		print "Options:\n";
		print " [ ".join(", ", map('"'.qm($_).'"', @$sfx))." ]\n";
		if (!$ixc) {
			print "Actual result:\n";
			showchan("near/.mbsyncstate");
		}
		print "Debug output:\n";
		print @$iret;
		exit 1;
	}

	rmtree "near";
	rmtree "far";

	my $njl = (@$nj - 1) * 2;
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
		if ($nxc || ckchan("near/.mbsyncstate.new", $tx)) {
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
				showchan("near/.mbsyncstate.new");
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
	my ($ttl, $sx, $tx, $sfx) = @_;

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

	test_impl(0, $sx, $tx, $sfx);
	test_impl(1, $sx, $tx, $sfx);

	killcfg();
}
