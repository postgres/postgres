#! /usr/bin/perl
#
# Copyright (c) 2001-2015, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_TW.pl
#
# Generate UTF-8 <--> EUC_TW code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain CNS11643.TXT from
# the organization's ftp site.
#
# CNS11643.TXT format:
#		 CNS11643 code in hex (3 bytes)
#		 (I guess the first byte means the plane No.)
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

require "ucs2utf.pl";

# first generate UTF-8 --> EUC_TW table

$in_file = "CNS11643.TXT";

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
	if ($code >= 0x80 && $ucs >= 0x0080)
	{
		$utf = &ucs2utf($ucs);
		if ($array{$utf} ne "")
		{
			printf STDERR "Warning: duplicate UTF8: %04x\n", $ucs;
			next;
		}
		$count++;

		$plane = ($code & 0x1f0000) >> 16;
		if ($plane > 16)
		{
			printf STDERR "Warning: invalid plane No.$plane. ignored\n";
			next;
		}

		if ($plane == 1)
		{
			$array{$utf} = (($code & 0xffff) | 0x8080);
		}
		else
		{
			$array{$utf} =
			  (0x8ea00000 + ($plane << 16)) | (($code & 0xffff) | 0x8080);
		}
	}
}
close(FILE);

#
# first, generate UTF8 --> EUC_TW table
#

$file = "utf8_to_euc_tw.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static const pg_utf_to_local ULmapEUC_TW[ $count ] = {\n";

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
# then generate EUC_JP --> UTF8 table
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
	($c, $u, $rest) = split;
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

		$plane = ($code & 0x1f0000) >> 16;
		if ($plane > 16)
		{
			printf STDERR "Warning: invalid plane No.$plane. ignored\n";
			next;
		}

		if ($plane == 1)
		{
			$c = (($code & 0xffff) | 0x8080);
			$array{$c} = $utf;
			$count++;
		}
		$c = (0x8ea00000 + ($plane << 16)) | (($code & 0xffff) | 0x8080);
		$array{$c} = $utf;
	}
}
close(FILE);

$file = "euc_tw_to_utf8.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static const pg_local_to_utf LUmapEUC_TW[ $count ] = {\n";
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
