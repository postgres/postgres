#! /usr/bin/perl
#
# Copyright (c) 2007-2025, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_CN.pl
#
# Generate UTF-8 <--> EUC_CN code conversion tables from
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

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_EUC_CN.pl';

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

	# The GB-18030 character set, which we use as the source, contains
	# a lot of extra characters on top of the GB2312 character set that
	# EUC_CN encodes. Filter out those extra characters.
	next if (($code & 0xFF) < 0xA1);
	next
	  if (
		!(     $code >= 0xA100 && $code <= 0xA9FF
			|| $code >= 0xB000 && $code <= 0xF7FF));

	next if ($code >= 0xA2A1 && $code <= 0xA2B0);
	next if ($code >= 0xA2E3 && $code <= 0xA2E4);
	next if ($code >= 0xA2EF && $code <= 0xA2F0);
	next if ($code >= 0xA2FD && $code <= 0xA2FE);
	next if ($code >= 0xA4F4 && $code <= 0xA4FE);
	next if ($code >= 0xA5F7 && $code <= 0xA5FE);
	next if ($code >= 0xA6B9 && $code <= 0xA6C0);
	next if ($code >= 0xA6D9 && $code <= 0xA6FE);
	next if ($code >= 0xA7C2 && $code <= 0xA7D0);
	next if ($code >= 0xA7F2 && $code <= 0xA7FE);
	next if ($code >= 0xA8BB && $code <= 0xA8C4);
	next if ($code >= 0xA8EA && $code <= 0xA8FE);
	next if ($code >= 0xA9A1 && $code <= 0xA9A3);
	next if ($code >= 0xA9F0 && $code <= 0xA9FE);
	next if ($code >= 0xD7FA && $code <= 0xD7FE);

	# A couple of characters are mapped differently from GB-2312 or GB-18030
	if ($code == 0xA1A4)
	{
		$ucs = 0x30FB;
	}
	if ($code == 0xA1AA)
	{
		$ucs = 0x2015;
	}

	push @mapping,
	  {
		ucs => $ucs,
		code => $code,
		direction => BOTH,
		f => $in_file,
		l => $.
	  };
}
close($in);

print_conversion_tables($this_script, "EUC_CN", \@mapping);
