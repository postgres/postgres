#! /usr/bin/perl
#
# Copyright (c) 2001-2024, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_KR.pl
#
# Generate UTF-8 <--> EUC_KR code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain OLD5601.TXT from
# the organization's ftp site.
#
# OLD5601.TXT format:
#		 KSC5601 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

use strict;
use warnings FATAL => 'all';

use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_EUC_KR.pl';

# Load the source file.

my $mapping = &read_source("KSX1001.TXT");

foreach my $i (@$mapping)
{
	$i->{code} = $i->{code} | 0x8080;
}

# Some extra characters that are not in KSX1001.TXT
push @$mapping,
  ( {
		direction => BOTH,
		ucs => 0x20AC,
		code => 0xa2e6,
		comment => '# EURO SIGN',
		f => $this_script,
		l => __LINE__
	},
	{
		direction => BOTH,
		ucs => 0x00AE,
		code => 0xa2e7,
		comment => '# REGISTERED SIGN',
		f => $this_script,
		l => __LINE__
	},
	{
		direction => BOTH,
		ucs => 0x327E,
		code => 0xa2e8,
		comment => '# CIRCLED HANGUL IEUNG U',
		f => $this_script,
		l => __LINE__
	});

print_conversion_tables($this_script, "EUC_KR", $mapping);
