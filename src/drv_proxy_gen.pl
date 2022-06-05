#!/usr/bin/perl
#
# SPDX-FileCopyrightText: 2017-2022 Oswald Buddenhagen <ossi@users.sf.net>
# SPDX-License-Identifier: GPL-2.0-or-later
#
# mbsync - mailbox synchronizer
#

use strict;
use warnings;

die("Usage: $0 driver.h drv_proxy.c drv_proxy.inc\n")
	if ($#ARGV != 2);

my ($in_header, $in_source, $out_source) = @ARGV;

my %templates;
my %defines;
my %excluded;
my %special;

open(my $ins, $in_source) or die("Cannot open $in_source: $!\n");
my $template;
my $define;
my $conts;
while (<$ins>) {
	if ($template) {
		if (/^\/\/\# END$/) {
			$templates{$template} = $conts;
			$template = undef;
		} else {
			$conts .= $_;
		}
	} elsif ($define) {
		if (/^\/\/\# END$/) {
			$defines{$define} = $conts;
			$define = undef;
		} else {
			($_ eq "\n") or s/^\t// or die("DEFINE content is not indented: $_");
			$conts .= $_;
		}
	} else {
		if (/^\/\/\# TEMPLATE (\w+)$/) {
			$template = $1;
			$conts = "";
		} elsif (/^\/\/\# DEFINE (\w+)$/) {
			$define = $1;
			$conts = "";
		} elsif (/^\/\/\# DEFINE (\w+) (.*)$/) {
			$defines{$1} = $2;
		} elsif (/^\/\/\# UNDEFINE (\w+)$/) {
			$defines{$1} = "";
		} elsif (/^\/\/\# EXCLUDE (\w+)$/) {
			$excluded{$1} = 1;
		} elsif (/^\/\/\# SPECIAL (\w+)$/) {
			$special{$1} = 1;
		}
	}
}
close($ins);

open(my $inh, $in_header) or die("Cannot open $in_header: $!\n");
my $sts = 0;
my $cont = "";
while (<$inh>) {
	if ($sts == 0) {
		if (/^struct driver \{$/) {
			$sts = 1;
		}
	} elsif ($sts == 1) {
		if (/^\};$/) {
			$sts = 0;
		} else {
			$cont .= $_;
		}
	}
}
close($inh);

$cont =~ s,(?://.*)?\n, ,g;
$cont =~ s,/\*.*?\*/, ,g;
$cont =~ s,\h+, ,g;
my @ptypes = map { s,^ ,,r } split(/;/, $cont);
pop @ptypes;  # last one is empty

my @cmd_table;

sub make_args($)
{
	$_ = shift;
	s/(?:^|(?<=, ))(?:const )?\w+ \*?//g;
	return $_;
}

sub type_to_format($)
{
	$_ = shift;
	s/xint /\%\#x/g;
	s/uint /\%u/g;
	s/int /\%d/g;
	s/const char \*/\%s/g;
	return $_;
}

sub make_format($)
{
	$_ = type_to_format(shift);
	s/, (\%\#?.)(\w+)/, $2=$1/g;
	return $_;
}

sub indent($$)
{
	my ($str, $indent) = @_;
	return $str =~ s,^(?=.),$indent,smgr;
}

open(my $outh, ">".$out_source) or die("Cannot create $out_source: $!\n");

for (@ptypes) {
	/^([\w* ]+)\(\*(\w+)\)\( (.*) \)$/ or die("Cannot parse prototype '$_'\n");
	my ($cmd_type, $cmd_name, $cmd_args) = ($1, $2, $3);
	if (defined($excluded{$cmd_name})) {
		push @cmd_table, "NULL";
		next;
	}
	push @cmd_table, "proxy_$cmd_name";
	next if (defined($special{$cmd_name}));
	my %replace;
	$replace{'name'} = $cmd_name;
	$replace{'type'} = $cmd_type;
	$cmd_args =~ s/^store_t \*ctx// or die("Arguments '$cmd_args' don't start with 'store_t *ctx'\n");
	if ($cmd_name =~ /^get_/) {
		$template = "GETTER";
		$replace{'fmt'} = type_to_format($cmd_type);
	} else {
		if ($cmd_type eq "void " && $cmd_args =~ s/, void \(\*cb\)\( (.*)void \*aux \), void \*aux$//) {
			my $cmd_cb_args = $1;
			if (length($cmd_cb_args)) {
				$replace{'decl_cb_args'} = $cmd_cb_args;
				my $r_cmd_cb_args = $cmd_cb_args;
				$r_cmd_cb_args =~ s/^int sts, // or die("Callback arguments of $cmd_name don't start with sts.\n");
				$replace{'decl_cb_state'} = $r_cmd_cb_args =~ s/, /\;\n/gr;
				my $pass_cb_args = make_args($cmd_cb_args);
				$replace{'save_cb_args'} = $pass_cb_args =~ s/([^,]+), /cmd->$1 = $1\;\n/gr;
				$pass_cb_args =~ s/([^, ]+)/cmd->$1/g;
				$replace{'pass_cb_args'} = $pass_cb_args;
				$replace{'print_pass_cb_args'} = $pass_cb_args =~ s/(.*), $/, $1/r;
				$replace{'print_fmt_cb_args'} = make_format($cmd_cb_args =~ s/(.*), $/, $1/r);
				$replace{'gen_cmd_t'} = "gen_sts_cmd_t";
				$replace{'GEN_CMD'} = "GEN_STS_CMD\n";
				$replace{'gen_cmd'} = "&cmd->gen.gen";
			} else {
				$replace{'gen_cmd_t'} = "gen_cmd_t";
				$replace{'GEN_CMD'} = "GEN_CMD\n";
				$replace{'gen_cmd'} = "&cmd->gen";
			}
			$replace{'checked'} = '0';
			$template = "CALLBACK";
		} elsif ($cmd_type eq "void ") {
			$template = "REGULAR_VOID";
		} else {
			$template = "REGULAR";
			$replace{'print_fmt_ret'} = type_to_format($cmd_type);
			$replace{'print_pass_ret'} = "rv";
		}
		$replace{'decl_args'} = $cmd_args;
		$replace{'print_pass_args'} = $replace{'pass_args'} = make_args($cmd_args);
		$replace{'print_fmt_args'} = make_format($cmd_args);
	}
	for (keys %defines) {
		$replace{$1} = delete $defines{$_} if (/^${cmd_name}_(.*)$/);
	}
	my %used;
	my $text = $templates{$template};
	$text =~ s/^(\h*)\@(\w+)\@\n/$used{$2} = 1; indent($replace{$2} \/\/ "", $1)/smeg;
	$text =~ s/\@(\w+)\@/$used{$1} = 1; $replace{$1} \/\/ ""/eg;
	print $outh $text."\n";
	my @not_used = grep { !defined($used{$_}) } keys %replace;
	die("Fatal: unconsumed replacements in $cmd_name: ".join(" ", @not_used)."\n") if (@not_used);
}
die("Fatal: unconsumed DEFINEs: ".join(" ", keys %defines)."\n") if (%defines);

print $outh "struct driver proxy_driver = {\n".join("", map { "\t$_,\n" } @cmd_table)."};\n";
close $outh;
