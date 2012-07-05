#! /usr/bin/perl -w

# generate_history.pl -- flatten release notes for use as HISTORY file
#
# Usage: generate_history.pl srcdir release.sgml >output.sgml
#
# The main point of this script is to strip out <link> references, which
# generally point into the rest of the documentation and so can't be used
# in a standalone build of the release notes.  To make sure this is done
# everywhere, we have to fold in the sub-files of the release notes.
#
# doc/src/sgml/generate_history.pl

use strict;

my $srcdir = shift;
die "$0: missing required argument: srcdir\n" if !defined($srcdir);
my $infile = shift;
die "$0: missing required argument: inputfile\n" if !defined($infile);

# Emit DOCTYPE header so that the output is a self-contained SGML document
print "<!DOCTYPE appendix PUBLIC \"-//OASIS//DTD DocBook V4.2//EN\">\n";

process_file($infile);

exit 0;

sub process_file
{
	my $filename = shift;

	local *FILE;    # need a local filehandle so we can recurse

	my $f = $srcdir . '/' . $filename;
	open(FILE, $f) || die "could not read $f: $!\n";

	while (<FILE>)
	{

		# Recursively expand sub-files of the release notes
		if (m/^&(release-.*);$/)
		{
			process_file($1 . ".sgml");
			next;
		}

		# Remove <link ...> tags, which might span multiple lines
		while (m/<link/)
		{
			if (s/<link\s+linkend[^>]*>//)
			{
				next;
			}

			# incomplete tag, so slurp another line
			$_ .= <FILE>;
		}

		# Remove </link> too
		s|</link>||g;

		print;
	}
	close(FILE);
}
