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
# $PostgreSQL: pgsql/doc/src/sgml/generate_history.pl,v 1.1 2009/05/02 20:17:19 tgl Exp $

use strict;

my($srcdir) = shift;
defined($srcdir) || die "$0: missing required argument: srcdir\n";
my($infile) = shift;
defined($infile) || die "$0: missing required argument: inputfile\n";

# Emit DOCTYPE header so that the output is a self-contained SGML document
print "<!DOCTYPE appendix PUBLIC \"-//OASIS//DTD DocBook V4.2//EN\">\n";

process_file($infile);

exit 0;

sub process_file {
    my($filename) = @_;

    local *FILE;		# need a local filehandle so we can recurse

    my($f) = $srcdir . '/' . $filename;
    open(FILE, $f) || die "could not read $f: $!\n";

    while (<FILE>) {
	# Recursively expand sub-files of the release notes
	if (m/^&(release-.*);$/) {
	    process_file($1 . ".sgml");
	    next;
	}

	# Remove <link ...> tags, which might span multiple lines
	while (m/<link/) {
	    if (s/<link\s+linkend[^>]*>//) {
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
