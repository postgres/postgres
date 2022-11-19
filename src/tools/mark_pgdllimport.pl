#!/usr/bin/perl

#----------------------------------------------------------------------
#
# mark_pgdllimport.pl
#	Perl script that tries to add PGDLLIMPORT markings to PostgreSQL
#	header files.
#
# This relies on a few idiosyncracies of the PostgreSQL coding style,
# such as the fact that we always use "extern" in function
# declarations, and that we don't use // comments. It's not very
# smart and may not catch all cases.
#
# It's probably a good idea to run pgindent on any files that this
# script modifies before committing.  This script uses as arguments
# a list of the header files to scan for the markings.
#
# Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/tools/mark_pgdllimport.pl
#
#----------------------------------------------------------------------

use strict;
use warnings;

for my $include_file (@ARGV)
{
	open(my $rfh, '<', $include_file) || die "$include_file: $!";
	my $buffer                = '';
	my $num_pgdllimport_added = 0;

	while (my $raw_line = <$rfh>)
	{
		my $needs_pgdllimport = 1;

		# By convention we declare global variables explicitly extern. We're
		# looking for those not already marked with PGDLLIMPORT.
		$needs_pgdllimport = 0
		  if $raw_line !~ /^extern\s+/
		  || $raw_line =~ /PGDLLIMPORT/;

		# Make a copy of the line and perform a simple-minded comment strip.
		# Also strip trailing whitespace.
		my $stripped_line = $raw_line;
		$stripped_line =~ s/\/\*.*\*\///g;
		$stripped_line =~ s/\s+$//;

		# Variable declarations should end in a semicolon. If we see an
		# opening parenthesis, it's probably a function declaration.
		$needs_pgdllimport = 0
		  if $stripped_line !~ /;$/
		  || $stripped_line =~ /\(/;

		# Add PGDLLIMPORT marker, if required.
		if ($needs_pgdllimport)
		{
			$raw_line =~ s/^extern/extern PGDLLIMPORT/;
			++$num_pgdllimport_added;
		}

		# Add line to buffer.
		$buffer .= $raw_line;
	}

	close($rfh);

	# If we added any PGDLLIMPORT markers, rewrite the file.
	if ($num_pgdllimport_added > 0)
	{
		printf "%s: adding %d PGDLLIMPORT markers\n",
		  $include_file, $num_pgdllimport_added;
		open(my $wfh, '>', $include_file) || die "$include_file: $!";
		print $wfh $buffer;
		close($wfh);
	}
}
