#! /usr/bin/perl
#
# Copyright (c) 2001-2014, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_BIG5.pl
#
# Generate UTF-8 <--> BIG5 conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain the map files from the organization's ftp site.
# ftp://www.unicode.org/Public/MAPPINGS/
#
# Our "big5" comes from BIG5.TXT, with the addition of the characters
# in the range 0xf9d6-0xf9dc from CP950.TXT.
#
# BIG5.TXT format:
#		 BIG5 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)
#
# CP950.TXT format:
#		 CP950 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)


require "ucs2utf.pl";


#
# first, generate UTF8 --> BIG5 table
#
$in_file = "BIG5.TXT";

open(FILE, $in_file) || die("cannot open $in_file");

reset 'array';

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($c, $u, $rest) = split;
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

$in_file = "CP950.TXT";

open(FILE, $in_file) || die("cannot open $in_file");

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($c, $u, $rest) = split;
	$ucs  = hex($u);
	$code = hex($c);

	# Pick only the ETEN extended characters in the range 0xf9d6 - 0xf9dc
	# from CP950.TXT
	if (   $code >= 0x80
		&& $ucs >= 0x0080
		&& $code >= 0xf9d6
		&& $code <= 0xf9dc)
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

$file = lc("utf8_to_big5.map");
open(FILE, "> $file") || die("cannot open $file");
print FILE "static pg_utf_to_local ULmapBIG5[ $count ] = {\n";

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
# then generate BIG5 --> UTF8 table
#
$in_file = "BIG5.TXT";

open(FILE, $in_file) || die("cannot open $in_file");

reset 'array';

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($c, $u, $rest) = split;
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
		$array{$code} = $utf;
	}
}
close(FILE);

$in_file = "CP950.TXT";

open(FILE, $in_file) || die("cannot open $in_file");

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($c, $u, $rest) = split;
	$ucs  = hex($u);
	$code = hex($c);

	# Pick only the ETEN extended characters in the range 0xf9d6 - 0xf9dc
	# from CP950.TXT
	if (   $code >= 0x80
		&& $ucs >= 0x0080
		&& $code >= 0xf9d6
		&& $code <= 0xf9dc)
	{
		$utf = &ucs2utf($ucs);
		if ($array{$utf} ne "")
		{
			printf STDERR "Warning: duplicate UTF8: %04x\n", $ucs;
			next;
		}
		$count++;
		$array{$code} = $utf;
	}
}
close(FILE);

$file = lc("big5_to_utf8.map");
open(FILE, "> $file") || die("cannot open $file");
print FILE "static pg_local_to_utf LUmapBIG5[ $count ] = {\n";
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
