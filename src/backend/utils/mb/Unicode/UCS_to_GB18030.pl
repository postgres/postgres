#! /usr/bin/perl
#
# Copyright (c) 2007-2015, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_GB18030.pl
#
# Generate UTF-8 <--> GB18030 code conversion tables from
# "gb-18030-2000.xml"
#
# The lines we care about in the source file look like
#    <a u="009A" b="81 30 83 36"/>
# where the "u" field is the Unicode code point in hex,
# and the "b" field is the hex byte sequence for GB18030

require "ucs2utf.pl";


# Read the input

$in_file = "gb-18030-2000.xml";

open(FILE, $in_file) || die("cannot open $in_file");

while (<FILE>)
{
	next if (!m/<a u="([0-9A-F]+)" b="([0-9A-F ]+)"/);
	$u = $1;
	$c = $2;
	$c =~ s/ //g;
	$ucs  = hex($u);
	$code = hex($c);
	if ($code >= 0x80 && $ucs >= 0x0080)
	{
		$utf = &ucs2utf($ucs);
		if ($arrayu{$utf} ne "")
		{
			printf STDERR "Warning: duplicate UTF8: %04x\n", $ucs;
			next;
		}
		if ($arrayc{$code} ne "")
		{
			printf STDERR "Warning: duplicate GB18030: %08x\n", $code;
			next;
		}
		$arrayu{$utf}  = $code;
		$arrayc{$code} = $utf;
		$count++;
	}
}
close(FILE);


#
# first, generate UTF8 --> GB18030 table
#

$file = "utf8_to_gb18030.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static const pg_utf_to_local ULmapGB18030[ $count ] = {\n";

$cc = $count;
for $index (sort { $a <=> $b } keys(%arrayu))
{
	$code = $arrayu{$index};
	$cc--;
	if ($cc == 0)
	{
		printf FILE "  {0x%04x, 0x%04x}\n", $index, $code;
	}
	else
	{
		printf FILE "  {0x%04x, 0x%04x},\n", $index, $code;
	}
}

print FILE "};\n";
close(FILE);


#
# then generate GB18030 --> UTF8 table
#

$file = "gb18030_to_utf8.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static const pg_local_to_utf LUmapGB18030[ $count ] = {\n";

$cc = $count;
for $index (sort { $a <=> $b } keys(%arrayc))
{
	$utf = $arrayc{$index};
	$cc--;
	if ($cc == 0)
	{
		printf FILE "  {0x%04x, 0x%04x}\n", $index, $utf;
	}
	else
	{
		printf FILE "  {0x%04x, 0x%04x},\n", $index, $utf;
	}
}

print FILE "};\n";
close(FILE);
