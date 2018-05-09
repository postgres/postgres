#! /usr/bin/perl
#
# Copyright (c) 2001-2018, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_SJIS.pl
#
# Generate UTF-8 <=> SJIS code conversion radix tree Generate UTF-8
# <=> SJIS code conversion radix tree Unfortunately it is prohibited
# by the organization to distribute the map files. So if you try to
# use this script, you have to obtain CP932.TXT from the organization's
# ftp site.

use strict;
use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_SJIS.pl';

my $mapping = read_source("CP932.TXT");

# Drop these SJIS codes from the source for UTF8=>SJIS conversion
my @reject_sjis = (
	0xed40 .. 0xeefc, 0x8754 .. 0x875d, 0x878a, 0x8782,
	0x8784,           0xfa5b,           0xfa54, 0x8790 .. 0x8792,
	0x8795 .. 0x8797, 0x879a .. 0x879c);

foreach my $i (@$mapping)
{
	my $code = $i->{code};
	my $ucs  = $i->{ucs};

	if (grep { $code == $_ } @reject_sjis)
	{
		$i->{direction} = TO_UNICODE;
	}
}

# Add these UTF8->SJIS pairs to the table.
push @$mapping,
  ( {
		direction => FROM_UNICODE,
		ucs       => 0x00a2,
		code      => 0x8191,
		comment   => '# CENT SIGN',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => FROM_UNICODE,
		ucs       => 0x00a3,
		code      => 0x8192,
		comment   => '# POUND SIGN',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => FROM_UNICODE,
		ucs       => 0x00a5,
		code      => 0x5c,
		comment   => '# YEN SIGN',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => FROM_UNICODE,
		ucs       => 0x00ac,
		code      => 0x81ca,
		comment   => '# NOT SIGN',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => FROM_UNICODE,
		ucs       => 0x2016,
		code      => 0x8161,
		comment   => '# DOUBLE VERTICAL LINE',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => FROM_UNICODE,
		ucs       => 0x203e,
		code      => 0x7e,
		comment   => '# OVERLINE',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => FROM_UNICODE,
		ucs       => 0x2212,
		code      => 0x817c,
		comment   => '# MINUS SIGN',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => FROM_UNICODE,
		ucs       => 0x301c,
		code      => 0x8160,
		comment   => '# WAVE DASH',
		f         => $this_script,
		l         => __LINE__
	});

print_conversion_tables($this_script, "SJIS", $mapping);
