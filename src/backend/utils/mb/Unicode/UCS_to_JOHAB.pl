#! /usr/bin/perl
#
# Copyright (c) 2001-2022, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_JOHAB.pl
#
# Generate UTF-8 <--> JOHAB conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain the map files from the organization's download site.
# https://www.unicode.org/Public/MAPPINGS/
# We assume the file include three tab-separated columns:
#		 JOHAB code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

use strict;
use warnings;

use convutils;

my $this_script = 'src/backend/utils/mb/Unicode/UCS_to_JOHAB.pl';

# Load the source file.

my $mapping = &read_source("JOHAB.TXT");

# Some extra characters that are not in JOHAB.TXT
push @$mapping,
  ( {
		direction => BOTH,
		ucs       => 0x20AC,
		code      => 0xd9e6,
		comment   => '# EURO SIGN',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => BOTH,
		ucs       => 0x00AE,
		code      => 0xd9e7,
		comment   => '# REGISTERED SIGN',
		f         => $this_script,
		l         => __LINE__
	},
	{
		direction => BOTH,
		ucs       => 0x327E,
		code      => 0xd9e8,
		comment   => '# CIRCLED HANGUL IEUNG U',
		f         => $this_script,
		l         => __LINE__
	});

print_conversion_tables($this_script, "JOHAB", $mapping);
