#! /usr/bin/perl
#
# Copyright (c) 2007-2013, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_GB18030.pl
#
# Generate UTF-8 <--> GB18030 code conversion tables from
# "ISO10646-GB18030.TXT"
#
# file format:
#		GB18030 hex code
#		UCS-2 hex code

require "ucs2utf.pl";


# first generate UTF-8 --> GB18030 table

$in_file = "ISO10646-GB18030.TXT";

open(FILE, $in_file) || die("cannot open $in_file");

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($u, $c, $rest) = split;
	$ucs  = hex($u);
	$code = hex($c);
	if ($code >= 0x80 && $ucs >= 0x0080)
	{
		$utf = &ucs2utf($ucs);
		if ($array{$utf} ne "")
		{
			printf STDERR "Warning: duplicate UTF8: %04x\n", $ucs;
			next;
		}
		$count++;

		$array{$utf} = $code;
	}
}
close(FILE);


#
# first, generate UTF8 --> GB18030 table
#

$file = "utf8_to_gb18030.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static pg_utf_to_local ULmapGB18030[ $count ] = {\n";

for $index (sort { $a <=> $b } keys(%array))
{
	$code = $array{$index};
	$count--;
	if ($count == 0)
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
reset 'array';

open(FILE, $in_file) || die("cannot open $in_file");

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($u, $c, $rest) = split;
	$ucs  = hex($u);
	$code = hex($c);
	if ($code >= 0x80 && $ucs >= 0x0080)
	{
		$utf = &ucs2utf($ucs);
		if ($array{$code} ne "")
		{
			printf STDERR "Warning: duplicate code: %04x\n", $ucs;
			next;
		}
		$count++;

		$array{$code} = $utf;
	}
}
close(FILE);

$file = "gb18030_to_utf8.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static pg_local_to_utf LUmapGB18030[ $count ] = {\n";
for $index (sort { $a <=> $b } keys(%array))
{
	$utf = $array{$index};
	$count--;
	if ($count == 0)
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
