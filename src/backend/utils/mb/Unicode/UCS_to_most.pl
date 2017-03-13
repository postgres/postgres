#! /usr/bin/perl
#
# Copyright (c) 2001-2017, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_most.pl
#
# Generate UTF-8 <--> character code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain the map files from the organization's ftp site.
# ftp://www.unicode.org/Public/MAPPINGS/
# We assume the file include three tab-separated columns:
#		 source character set code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

use strict;
use convutils;

my $this_script = $0;

my %filename = (
	'WIN866'     => 'CP866.TXT',
	'WIN874'     => 'CP874.TXT',
	'WIN1250'    => 'CP1250.TXT',
	'WIN1251'    => 'CP1251.TXT',
	'WIN1252'    => 'CP1252.TXT',
	'WIN1253'    => 'CP1253.TXT',
	'WIN1254'    => 'CP1254.TXT',
	'WIN1255'    => 'CP1255.TXT',
	'WIN1256'    => 'CP1256.TXT',
	'WIN1257'    => 'CP1257.TXT',
	'WIN1258'    => 'CP1258.TXT',
	'ISO8859_2'  => '8859-2.TXT',
	'ISO8859_3'  => '8859-3.TXT',
	'ISO8859_4'  => '8859-4.TXT',
	'ISO8859_5'  => '8859-5.TXT',
	'ISO8859_6'  => '8859-6.TXT',
	'ISO8859_7'  => '8859-7.TXT',
	'ISO8859_8'  => '8859-8.TXT',
	'ISO8859_9'  => '8859-9.TXT',
	'ISO8859_10' => '8859-10.TXT',
	'ISO8859_13' => '8859-13.TXT',
	'ISO8859_14' => '8859-14.TXT',
	'ISO8859_15' => '8859-15.TXT',
	'ISO8859_16' => '8859-16.TXT',
	'KOI8R'      => 'KOI8-R.TXT',
	'KOI8U'      => 'KOI8-U.TXT',
	'GBK'        => 'CP936.TXT');

# make maps for all encodings if not specified
my @charsets = (scalar(@ARGV) > 0) ? @ARGV : keys(%filename);

foreach my $charset (@charsets)
{
	my $mapping = &read_source($filename{$charset});

	print_conversion_tables($this_script, $charset, $mapping);
}
