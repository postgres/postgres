#! /usr/bin/perl
#
# Copyright (c) 2007-2016, PostgreSQL Global Development Group
#
# src/backend/utils/mb/Unicode/UCS_to_EUC_JIS_2004.pl
#
# Generate UTF-8 <--> EUC_JIS_2004 code conversion tables from
# "euc-jis-2004-std.txt" (http://x0213.org)

require "convutils.pm";

# first generate UTF-8 --> EUC_JIS_2004 table

$in_file = "euc-jis-2004-std.txt";

open(FILE, $in_file) || die("cannot open $in_file");

my @all;

while ($line = <FILE>)
{
	if ($line =~ /^0x(.*)[ \t]*U\+(.*)\+(.*)[ \t]*#(.*)$/)
	{
		$c              = $1;
		$u1             = $2;
		$u2             = $3;
		$rest           = "U+" . $u1 . "+" . $u2 . $4;
		$code           = hex($c);
		$ucs1           = hex($u1);
		$ucs2           = hex($u2);

		push @all, { direction => 'both',
					 ucs => $ucs1,
					 ucs_second => $ucs2,
					 code => $code,
					 comment => $rest };
		next;
	}
	elsif ($line =~ /^0x(.*)[ \t]*U\+(.*)[ \t]*#(.*)$/)
	{
		$c    = $1;
		$u    = $2;
		$rest = "U+" . $u . $3;
	}
	else
	{
		next;
	}

	$ucs  = hex($u);
	$code = hex($c);

	next if ($code < 0x80 && $ucs < 0x80);

	push @all, { direction => 'both', ucs => $ucs, code => $code, comment => $rest };
}
close(FILE);

print_tables("EUC_JIS_2004", \@all, 1);
