#! /usr/bin/perl
#
# Copyright (c) 2001-2015, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_JP.pl
#
# Generate UTF-8 <--> EUC_JP code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain JIS0201.TXT, JIS0208.TXT, JIS0212.TXT from
# the organization's ftp site.
#
# JIS0201.TXT format:
#		 JIS0201 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)
#
# JIS0208.TXT format:
#		 JIS0208 shift-JIS code in hex
#		 JIS0208 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)
#
# JIS0212.TXT format:
#		 JIS0212 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

require "ucs2utf.pl";

# first generate UTF-8 --> EUC_JP table

#
# JIS0201
#
$in_file = "JIS0201.TXT";

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

		# add single shift 2
		$array{$utf} = ($code | 0x8e00);
	}
}
close(FILE);

#
# JIS0208
#
$in_file = "JIS0208.TXT";

open(FILE, $in_file) || die("cannot open $in_file");

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($s, $c, $u, $rest) = split;
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

		$array{$utf} = ($code | 0x8080);
	}
}
close(FILE);

#
# JIS0212
#
$in_file = "JIS0212.TXT";

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

		$array{$utf} = ($code | 0x8f8080);
	}
}
close(FILE);

#
# first, generate UTF8 --> EUC_JP table
#

$file = "utf8_to_euc_jp.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static const pg_utf_to_local ULmapEUC_JP[ $count ] = {\n";

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

#
# JIS0201
#
$in_file = "JIS0201.TXT";

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
		if ($array{$code} ne "")
		{
			printf STDERR "Warning: duplicate code: %04x\n", $ucs;
			next;
		}
		$count++;

		# add single shift 2
		$code |= 0x8e00;
		$array{$code} = $utf;
	}
}
close(FILE);

#
# JIS0208
#
$in_file = "JIS0208.TXT";

open(FILE, $in_file) || die("cannot open $in_file");

while (<FILE>)
{
	chop;
	if (/^#/)
	{
		next;
	}
	($s, $c, $u, $rest) = split;
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

		$code |= 0x8080;
		$array{$code} = $utf;
	}
}
close(FILE);

#
# JIS0212
#
$in_file = "JIS0212.TXT";

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

		$code |= 0x8f8080;
		$array{$code} = $utf;
	}
}
close(FILE);

$file = "euc_jp_to_utf8.map";
open(FILE, "> $file") || die("cannot open $file");
print FILE "static const pg_local_to_utf LUmapEUC_JP[ $count ] = {\n";
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
