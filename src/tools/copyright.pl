#!/usr/bin/perl
#################################################################
# copyright.pl -- update copyright notices throughout the source tree, idempotently.
#
# Copyright (c) 2011-2016, PostgreSQL Global Development Group
#
# src/tools/copyright.pl
#
# FYI, Tie adds a trailing newline on the last line if missing.
#################################################################

use strict;
use warnings;

use File::Find;
use File::Basename;
use Tie::File;

my $pgdg = 'PostgreSQL Global Development Group';
my $cc   = 'Copyright \(c\)';
my $ccliteral = 'Copyright (c)';

# year-1900 is what localtime(time) puts in element 5
my $year = 1900 + ${ [ localtime(time) ] }[5];

print "Using current year:  $year\n";

find({ wanted => \&wanted, no_chdir => 1 }, '.');

sub wanted
{

	# prevent corruption of git indexes by ignoring any .git/
	if (basename($_) eq '.git')
	{
		$File::Find::prune = 1;
		return;
	}

	return if !-f $File::Find::name || -l $File::Find::name;

	# skip file names with binary extensions
	# How are these updated?  bjm 2012-01-02
	return if ($_ =~ m/\.(ico|bin|po)$/);

	my @lines;
	tie @lines, "Tie::File", $File::Find::name;

	foreach my $line (@lines)
	{

		# We only care about lines with a copyright notice.
		next unless $line =~ m/$cc.*$pgdg/i;

		# Skip line if already matches the current year; if not
		# we get $year-$year, e.g. 2012-2012
		next if $line =~ m/$cc $year, $pgdg/i;

		# We process all lines because some files have copyright
		# strings embedded in them, e.g. src/bin/psql/help.c
		$line =~ s/$cc (\d{4})-\d{4}, $pgdg/$ccliteral $1-$year, $pgdg/i;
		$line =~ s/$cc (\d{4}), $pgdg/$ccliteral $1-$year, $pgdg/i;
	}
	untie @lines;
}

print "Manually update:\n";
print "  ./src/interfaces/libpq/libpq.rc.in in head\n";
print "  ./doc/src/sgml/legal.sgml in head and back branches\n";
print "  ./COPYRIGHT in back branches\n";
