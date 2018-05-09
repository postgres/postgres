#! /usr/bin/perl
#
# Copyright (c) 2007-2018, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_GB18030.pl
#
# Generate UTF-8 <--> GB18030 code conversion tables from
# "gb-18030-2000.xml", obtained from
# http://source.icu-project.org/repos/icu/data/trunk/charset/data/xml/
#
# The lines we care about in the source file look like
#    <a u="009A" b="81 30 83 36"/>
# where the "u" field is the Unicode code point in hex,
# and the "b" field is the hex byte sequence for GB18030

use strict;
use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_EUC_CN.pl';

# Read the input

my $in_file = "gb-18030-2000.xml";

open(my $in, '<', $in_file) || die("cannot open $in_file");

my @mapping;

while (<$in>)
{
	next if (!m/<a u="([0-9A-F]+)" b="([0-9A-F ]+)"/);
	my ($u, $c) = ($1, $2);
	$c =~ s/ //g;
	my $ucs  = hex($u);
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
		ucs       => $ucs,
		code      => $code,
		direction => BOTH,
		f         => $in_file,
		l         => $.
	  };
}
close($in);

print_conversion_tables($this_script, "EUC_CN", \@mapping);
