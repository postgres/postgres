#! /usr/bin/perl
#
# Copyright (c) 2007-2022, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_SHIFT_JIS_2004.pl
#
# Generate UTF-8 <--> SHIFT_JIS_2004 code conversion tables from
# "sjis-0213-2004-std.txt" (http://x0213.org)

use strict;
use warnings;

use convutils;

# first generate UTF-8 --> SHIFT_JIS_2004 table

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_SHIFT_JIS_2004.pl';

my $in_file = "sjis-0213-2004-std.txt";

open(my $in, '<', $in_file) || die("cannot open $in_file");

my @mapping;

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

		push @mapping,
		  {
			code       => $code,
			ucs        => $ucs1,
			ucs_second => $ucs2,
			comment    => $rest,
			direction  => BOTH,
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
		my $direction;

		if ($code < 0x80 && $ucs < 0x80)
		{
			next;
		}
		elsif ($code < 0x80)
		{
			$direction = FROM_UNICODE;
		}
		elsif ($ucs < 0x80)
		{
			$direction = TO_UNICODE;
		}
		else
		{
			$direction = BOTH;
		}

		push @mapping,
		  {
			code      => $code,
			ucs       => $ucs,
			comment   => $rest,
			direction => $direction,
			f         => $in_file,
			l         => $.
		  };
	}
}
close($in);

print_conversion_tables($this_script, "SHIFT_JIS_2004", \@mapping);
