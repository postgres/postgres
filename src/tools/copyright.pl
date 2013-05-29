#!/usr/bin/perl
#################################################################
# copyright.pl -- update copyright notices throughout the source tree, idempotently.
#
# Copyright (c) 2011-2013, PostgreSQL Global Development Group
#
# src/tools/copyright.pl
#################################################################

use strict;
use warnings;

use File::Find;
use File::Basename;
use Tie::File;

my $pgdg = 'PostgreSQL Global Development Group';
my $cc   = 'Copyright \(c\) ';

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
	return if ($_ =~ m/\.(ico|bin)$/);

	my @lines;
	tie @lines, "Tie::File", $File::Find::name;

	foreach my $line (@lines)
	{

		# We only care about lines with a copyright notice.
		next unless $line =~ m/$cc.*$pgdg/;

		# Skip line if already matches the current year; if not
		# we get $year-$year, e.g. 2012-2012
		next if $line =~ m/$cc$year, $pgdg/;

		# We process all lines because some files have copyright
		# strings embedded in them, e.g. src/bin/psql/help.c
		$line =~ s/($cc\d{4})(, $pgdg)/$1-$year$2/;
		$line =~ s/($cc\d{4})-\d{4}(, $pgdg)/$1-$year$2/;
	}
	untie @lines;
}

print
"Manually update doc/src/sgml/legal.sgml and src/interfaces/libpq/libpq.rc.in too.\n";
print
"Also update ./COPYRIGHT and doc/src/sgml/legal.sgml in all back branches.\n";

