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
	my $inc_tpl = "";
	my %replace;
	$replace{'name'} = $cmd_name;
	$replace{'type'} = $cmd_type;
	$cmd_args =~ s/^store_t \*ctx// or die("Arguments '$cmd_args' don't start with 'store_t *ctx'\n");
	if ($cmd_name =~ /^get_/) {
		$template = "GETTER";
		$replace{'fmt'} = type_to_format($cmd_type);
	} else {
		my $pass_args;
		if ($cmd_type eq "void " && $cmd_args =~ s/, void \(\*cb\)\( (.*)void \*aux \), void \*aux$//) {
			my $cmd_cb_args = $1;
			$replace{'decl_cb_args'} = $cmd_cb_args;
			$replace{'pass_cb_args'} = make_args($cmd_cb_args);
			if (length($cmd_cb_args)) {
				my $r_cmd_cb_args = $cmd_cb_args;
				$r_cmd_cb_args =~ s/^int sts, // or die("Callback arguments of $cmd_name don't start with sts.\n");
				$r_cmd_cb_args =~ s/^(.*), $/, $1/;
				$replace{'print_pass_cb_args'} = make_args($r_cmd_cb_args);
				$replace{'print_fmt_cb_args'} = make_format($r_cmd_cb_args);
				$inc_tpl = 'CALLBACK_STS';
			} else {
				$inc_tpl = 'CALLBACK_VOID';
			}

			$pass_args = make_args($cmd_args);
			$pass_args =~ s/([^, ]+)/cmd->$1/g;
			my $r_cmd_args = $cmd_args =~ s/, (.*)$/$1, /r;
			$replace{'decl_state'} = $r_cmd_args =~ s/, /\;\n/gr;
			my $r_pass_args = make_args($r_cmd_args);
			$replace{'assign_state'} = $r_pass_args =~ s/([^,]+), /cmd->$1 = $1\;\n/gr;

			$replace{'checked'} = '0';
			$template = "CALLBACK";
		} else {
			$pass_args = make_args($cmd_args);

			if ($cmd_type eq "void ") {
				$template = "REGULAR_VOID";
			} else {
				$template = "REGULAR";
				$replace{'print_fmt_ret'} = type_to_format($cmd_type);
				$replace{'print_pass_ret'} = "rv";
			}
		}
		$replace{'decl_args'} = $cmd_args;
		$replace{'print_pass_args'} = $replace{'pass_args'} = $pass_args;
		$replace{'print_fmt_args'} = make_format($cmd_args);
	}
	my ($fake_cond, $fake_invoke, $fake_cb_args, $post_invoke) = (undef, "", "", "");
	for (keys %defines) {
		next if (!/^${cmd_name}_(.*)$/);
		my ($key, $val) = ($1, delete $defines{$_});
		if ($key eq 'counted') {
			$replace{count_step} = "countStep();\n";
		} elsif ($key eq 'fakeable') {
			$fake_cond = "ctx->is_fake";
			$replace{print_pass_dry} = ', '.$fake_cond.' ? " [FAKE]" : ""';
		} elsif ($key eq 'driable') {
			$fake_cond = "DFlags & DRYRUN";
			$replace{print_pass_dry} = ', ('.$fake_cond.') ? " [DRY]" : ""';
		} elsif ($key eq 'fake_invoke') {
			$fake_invoke = $val;
		} elsif ($key eq 'fake_cb_args') {
			$fake_cb_args = $val;
		} elsif ($key eq 'post_real_invoke') {
			$post_invoke = $val;
		} else {
			$replace{$key} = $val;
		}
	}
	if (defined($fake_cond)) {
		$replace{print_fmt_dry} = '%s';
		if ($inc_tpl eq 'CALLBACK_STS') {
			$fake_invoke .= "proxy_${cmd_name}_cb( DRV_OK${fake_cb_args}, cmd );\n";
		} elsif (length($fake_cb_args)) {
			die("Unexpected fake callback arguments to $cmd_name\n");
		}
		my $num_fake = $fake_invoke =~ s/^(?=.)/\t/gsm;
		my $num_real = $post_invoke =~ s/^(?=.)/\t/gsm;
		my $pre_invoke = "if (".$fake_cond.")";
		if ($num_fake > 1 || $num_real) {
			$pre_invoke .= " {";
			$fake_invoke .= "} else {\n";
			$post_invoke .= "}\n";
		} else {
			$fake_invoke .= "else\n";
		}
		$replace{pre_invoke} = $pre_invoke."\n".$fake_invoke;
		$replace{indent_invoke} = "\t";
		$replace{post_invoke} = $post_invoke;
	}
	my %used;
	my $text = $templates{$template};
	if ($inc_tpl) {
		if ($inc_tpl eq 'CALLBACK_STS') {
			if ($replace{print_fmt_cb_args}) {
				$inc_tpl .= '_FMT';
			} else {
				if ($replace{print_cb_args}) {
					$inc_tpl .= '_PRN';
				}
				# These may be defined but empty; that's not an error.
				delete $replace{print_fmt_cb_args};
				delete $replace{print_pass_cb_args};
			}
		}
		$text =~ s/^\t\@print_cb_args_tpl\@\n/$templates{$inc_tpl}/sm;
	}
	$text =~ s/^(\h*)\@(\w+)\@\n/$used{$2} = 1; indent($replace{$2} \/\/ "", $1)/smeg;
	$text =~ s/\@(\w+)\@/$used{$1} = 1; $replace{$1} \/\/ ""/eg;
	print $outh $text."\n";
	my @not_used = grep { !defined($used{$_}) } keys %replace;
	die("Fatal: unconsumed replacements in $cmd_name: ".join(" ", @not_used)."\n") if (@not_used);
}
die("Fatal: unconsumed DEFINEs: ".join(" ", keys %defines)."\n") if (%defines);

print $outh "struct driver proxy_driver = {\n".join("", map { "\t$_,\n" } @cmd_table)."};\n";
close $outh;
