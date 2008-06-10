#! /usr/bin/perl -w

#################################################################
# version_stamp.pl -- update version stamps throughout the source tree
#
# Copyright (c) 2008, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/tools/version_stamp.pl,v 1.1.10.1 2008/06/10 18:09:26 tgl Exp $
#################################################################

#
# This script updates the version stamp in configure.in, and also in assorted
# other files wherein it's not convenient to obtain the version number from
# configure's output.  Note that you still have to run autoconf afterward
# to regenerate configure from the updated configure.in.
#
# Usage: cd to top of source tree and issue
#	src/tools/version_stamp.pl MINORVERSION
# where MINORVERSION can be a minor release number (0, 1, etc), or
# "devel", "betaN", "rcN".
#

# Major version is hard-wired into the script.  We update it when we branch
# a new development version.
$major1 = 7;
$major2 = 4;

# Validate argument and compute derived variables
$minor = shift;
defined($minor) || die "$0: missing required argument: minor-version\n";

if ($minor =~ m/^\d+$/) {
    $dotneeded = 1;
    $numericminor = $minor;
} elsif ($minor eq "devel") {
    $dotneeded = 0;
    $numericminor = 0;
} elsif ($minor =~ m/^beta\d+$/) {
    $dotneeded = 0;
    $numericminor = 0;
} elsif ($minor =~ m/^rc\d+$/) {
    $dotneeded = 0;
    $numericminor = 0;
} else {
    die "$0: minor-version must be N, devel, betaN, or rcN\n";
}

# Create various required forms of the version number
$majorversion = $major1 . "." . $major2;
if ($dotneeded) {
    $fullversion = $majorversion . "." . $minor;
} else {
    $fullversion = $majorversion . $minor;
}

# Get the autoconf version number for eventual nag message
# (this also ensures we're in the right directory)

$aconfver = "";
open(FILE, "configure.in") || die "could not read configure.in: $!\n";
while (<FILE>) {
    if (m/^m4_if\(m4_defn\(\[m4_PACKAGE_VERSION\]\), \[(.*)\], \[\], \[m4_fatal/) {
        $aconfver = $1;
	last;
    }
}
close(FILE);
$aconfver ne "" || die "could not find autoconf version number in configure.in\n";

# Update configure.in and other files that contain version numbers

$fixedfiles = "";

sed_file("configure.in",
	 "-e 's/AC_INIT(\\[PostgreSQL\\], \\[[0-9a-z.]*\\]/AC_INIT([PostgreSQL], [$fullversion]/'");

sed_file("doc/bug.template",
	 "-e 's/PostgreSQL version (example: PostgreSQL .*) *:  PostgreSQL .*/PostgreSQL version (example: PostgreSQL $fullversion):  PostgreSQL $fullversion/'");

sed_file("src/include/pg_config.h.win32",
	 "-e 's/#define PG_VERSION \".*\"/#define PG_VERSION \"$fullversion\"/' " .
	 "-e 's/#define PG_VERSION_STR \".* (win32)\"/#define PG_VERSION_STR \"$fullversion (win32)\"/'");

sed_file("src/interfaces/libpq/libpq.rc",
	 "-e 's/FILEVERSION [0-9]*,[0-9]*,[0-9]*,0/FILEVERSION $major1,$major2,$numericminor,0/' " .
	 "-e 's/PRODUCTVERSION [0-9]*,[0-9]*,[0-9]*,0/PRODUCTVERSION $major1,$major2,$numericminor,0/' " .
	 "-e 's/VALUE \"FileVersion\", \"[0-9]*, [0-9]*, [0-9]*/VALUE \"FileVersion\", \"$major1, $major2, $numericminor/' " .
	 "-e 's/VALUE \"ProductVersion\", \"[0-9]*, [0-9]*, [0-9]*/VALUE \"ProductVersion\", \"$major1, $major2, $numericminor/'");

print "Stamped these files with version number $fullversion:\n$fixedfiles";
print "Don't forget to run autoconf $aconfver before committing.\n";

exit 0;

sub sed_file {
    my($filename, $sedargs) = @_;
    my($tmpfilename) = $filename . ".tmp";

    system("sed $sedargs $filename >$tmpfilename") == 0
      or die "sed failed: $?";
    system("mv $tmpfilename $filename") == 0
      or die "mv failed: $?";

    $fixedfiles .= "\t$filename\n";
}
