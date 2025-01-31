#!/usr/bin/perl
#
# Generate sorted list of non-overlapping intervals of non-spacing
# characters, using Unicode data files as input.  Pass UnicodeData.txt
# as argument.  The output is on stdout.
#
# Copyright (c) 2019-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

my $range_start = undef;
my $codepoint;
my $prev_codepoint;
my $count = 0;

print
  "/* generated by src/common/unicode/generate-unicode_nonspacing_table.pl, do not edit */\n\n";

print "static const struct mbinterval nonspacing[] = {\n";

foreach my $line (<ARGV>)
{
	chomp $line;
	my @fields = split ';', $line;
	$codepoint = hex $fields[0];

	# Me and Mn refer to combining characters
	# Cf refers to format characters
	if ($fields[2] eq 'Me' || $fields[2] eq 'Mn' || $fields[2] eq 'Cf')
	{
		# non-spacing character, save for start of range
		if (!defined($range_start))
		{
			$range_start = $codepoint;
		}
	}
	else
	{
		# not a non-spacing character, print out previous range if any
		if (defined($range_start))
		{
			printf "\t{0x%04X, 0x%04X},\n", $range_start, $prev_codepoint;
			$range_start = undef;
		}
	}
}
continue
{
	$prev_codepoint = $codepoint;
}

print "};\n";
