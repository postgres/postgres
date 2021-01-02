#! /usr/bin/perl
#
# Copyright (c) 2007-2021, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_JIS_2004.pl
#
# Generate UTF-8 <--> EUC_JIS_2004 code conversion tables from
# "euc-jis-2004-std.txt" (http://x0213.org)

use strict;
use warnings;

use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_EUC_JIS_2004.pl';

# first generate UTF-8 --> EUC_JIS_2004 table

my $in_file = "euc-jis-2004-std.txt";

open(my $in, '<', $in_file) || die("cannot open $in_file");

my @all;

while (my $line = <$in>)
{
	if ($line =~ /^0x(\w+)\s*U\+(\w+)\+(\w+)\s*#\s*(\S.*)?\s*$/)
	{

		# combined characters
		my ($c, $u1, $u2) = ($1, $2, $3);
		# The "\t \t" below is just to avoid insubstantial diffs.
		my $rest = "U+" . $u1 . "+" . $u2 . "\t \t" . $4;
		my $code = hex($c);
		my $ucs1 = hex($u1);
		my $ucs2 = hex($u2);

		push @all,
		  {
			direction  => BOTH,
			ucs        => $ucs1,
			ucs_second => $ucs2,
			code       => $code,
			comment    => $rest,
			f          => $in_file,
			l          => $.
		  };
	}
	elsif ($line =~ /^0x(\w+)\s*U\+(\w+)\s*#\s*(\S.*)?\s*$/)
	{

		# non-combined characters
		my ($c, $u, $rest) = ($1, $2, "U+" . $2 . $3);
		my $ucs  = hex($u);
		my $code = hex($c);

		next if ($code < 0x80 && $ucs < 0x80);

		push @all,
		  {
			direction => BOTH,
			ucs       => $ucs,
			code      => $code,
			comment   => $rest,
			f         => $in_file,
			l         => $.
		  };
	}
}
close($in);

print_conversion_tables($this_script, "EUC_JIS_2004", \@all);
