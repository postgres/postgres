#! /usr/bin/perl
#
# Copyright (c) 2007-2025, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_GB18030.pl
#
# Generate UTF-8 <--> GB18030 code conversion tables from
# "gb18030-2022.ucm", obtained from
# https://github.com/unicode-org/icu/blob/main/icu4c/source/data/mappings/
#
# The lines we care about in the source file look like
#   <UXXXX> \xYY[\xYY...] |n
# where XXXX is the Unicode code point in hex,
# and the \xYY... is the hex byte sequence for GB18030,
# and n is a flag indicating the type of mapping.

use strict;
use warnings FATAL => 'all';

use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_GB18030.pl';

# Read the input

my $in_file = "gb18030-2022.ucm";

open(my $in, '<', $in_file) || die("cannot open $in_file");

my @mapping;

while (<$in>)
{
	# Mappings may have been removed by commenting out
	next if /^#/;

	next if !/^<U([0-9A-Fa-f]+)>\s+
			((?:\\x[0-9A-Fa-f]{2})+)\s+
			\|(\d+)/x;
	my ($u, $c, $flag) = ($1, $2, $3);
	$c =~ s/\\x//g;

	# We only want round-trip mappings
	next if ($flag ne '0');

	my $ucs = hex($u);
	my $code = hex($c);
	if ($code >= 0x80 && $ucs >= 0x0080)
	{
		push @mapping,
		  {
			ucs => $ucs,
			code => $code,
			direction => BOTH,
			f => $in_file,
			l => $.
		  };
	}
}
close($in);

print_conversion_tables($this_script, "GB18030", \@mapping);
