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
			my $pfx;
			for my $e (@vals) {
				if (!defined($pfx)) {
					$pfx = ($e =~ /^([A-Z]+_)/) ? $1 : "";
				} elsif (length($pfx)) {
					$pfx = "" if ((($e =~ /^([A-Z]+_)/) ? $1 : "") ne $pfx);
				}
			}
			my $bit = 1;
			my $bitn = 0;
			for my $e (@vals) {
				my $bits = ($e =~ s/\((\d+)\)$//) ? $1 : 1;
				if ($bits != 1) {
					print "#define $e(b) ($bit << (b))\n";
				} else {
					print "#define $e $bit\n";
				}
				$bit <<= $bits;
				$bitn += $bits;
			}
			if (length($pfx)) {
				print "#define ${pfx}_NUM_BITS $bitn\n";
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
