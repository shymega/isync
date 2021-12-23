#!/usr/bin/perl
#
# SPDX-FileCopyrightText: 2022 Oswald Buddenhagen <ossi@users.sf.net>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# mbsync - mailbox synchronizer
#

use strict;
use warnings;

my $in_enum = 0;
my $conts;
while (<>) {
	s,\s*(?://.*)?$,,;
	if ($in_enum) {
		if (/^\)$/) {
			$conts =~ s/\s//g;
			$conts =~ s/,$//;
			my @vals = split(/,/, $conts);
			my ($pfx, $pfx1);
			for my $e (@vals) {
				if (!defined($pfx)) {
					$pfx1 = $pfx = ($e =~ /^([A-Z]+_)/) ? $1 : "";
				} elsif (length($pfx)) {
					$pfx = "" if ((($e =~ /^([A-Z]+_)/) ? $1 : "") ne $pfx);
				}
			}
			my $bit = 1;
			my $bitn = 0;
			my (@names, @nameos);
			my $nameo = 0;
			for my $e (@vals) {
				my $bits = ($e =~ s/\((\d+)\)$//) ? $1 : 1;
				my $n = substr($e, length($pfx));
				if ($bits != 1) {
					die("Unsupported field size $bits\n") if ($bits != 2);
					print "#define $e(b) ($bit << (b))\n";
					push @names, "F-".$n, "N-".$n;
					my $nl = length($n) + 3;
					push @nameos, $nameo, $nameo + $nl;
					$nameo += $nl * 2;
				} else {
					print "#define $e $bit\n";
					push @names, $n;
					push @nameos, $nameo;
					$nameo += length($n) + 1;
				}
				$bit <<= $bits;
				$bitn += $bits;
			}
			if (length($pfx)) {
				print "#define ${pfx}_NUM_BITS $bitn\n";
			}
			if (length($pfx1)) {
				print "#define ${pfx1}_STRINGS \"".join("\\0", @names)."\"\n";
				print "#define ${pfx1}_OFFSETS ".join(", ", @nameos)."\n";
			}
			print "\n";
			$in_enum = 0;
		} else {
			$conts .= $_;
		}
	} else {
		if (/^BIT_ENUM\($/) {
			$conts = "";
			$in_enum = 1;
		}
	}
}
