#! /usr/bin/perl
#
# Copyright (c) 2007-2021, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_GB18030.pl
#
# Generate UTF-8 <--> UHC code conversion tables from
# "windows-949-2000.xml", obtained from
# http://source.icu-project.org/repos/icu/data/trunk/charset/data/xml/
#
# The lines we care about in the source file look like
#    <a u="009A" b="81 30 83 36"/>
# where the "u" field is the Unicode code point in hex,
# and the "b" field is the hex byte sequence for UHC

use strict;
use warnings;

use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_UHC.pl';

# Read the input

my $in_file = "windows-949-2000.xml";

open(my $in, '<', $in_file) || die("cannot open $in_file");

my @mapping;

while (<$in>)
{
	next if (!m/<a u="([0-9A-F]+)" b="([0-9A-F ]+)"/);
	my ($u, $c) = ($1, $2);
	$c =~ s/ //g;
	my $ucs  = hex($u);
	my $code = hex($c);

	next if ($code == 0x0080 || $code == 0x00FF);

	if ($code >= 0x80 && $ucs >= 0x0080)
	{
		push @mapping,
		  {
			ucs       => $ucs,
			code      => $code,
			direction => BOTH,
			f         => $in_file,
			l         => $.
		  };
	}
}
close($in);

# One extra character that's not in the source file.
push @mapping,
  {
	direction => BOTH,
	code      => 0xa2e8,
	ucs       => 0x327e,
	comment   => 'CIRCLED HANGUL IEUNG U',
	f         => $this_script,
	l         => __LINE__
  };

print_conversion_tables($this_script, "UHC", \@mapping);
