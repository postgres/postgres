#! /usr/bin/perl -w

#################################################################
# version_stamp.pl -- update version stamps throughout the source tree
#
# Copyright (c) 2008-2016, PostgreSQL Global Development Group
#
# src/tools/version_stamp.pl
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
# "devel", "alphaN", "betaN", "rcN".
#

# Major version is hard-wired into the script.  We update it when we branch
# a new development version.
$majorversion = 10;

# Validate argument and compute derived variables
$minor = shift;
defined($minor) || die "$0: missing required argument: minor-version\n";

if ($minor =~ m/^\d+$/)
{
	$dotneeded    = 1;
	$numericminor = $minor;
}
elsif ($minor eq "devel")
{
	$dotneeded    = 0;
	$numericminor = 0;
}
elsif ($minor =~ m/^alpha\d+$/)
{
	$dotneeded    = 0;
	$numericminor = 0;
}
elsif ($minor =~ m/^beta\d+$/)
{
	$dotneeded    = 0;
	$numericminor = 0;
}
elsif ($minor =~ m/^rc\d+$/)
{
	$dotneeded    = 0;
	$numericminor = 0;
}
else
{
	die "$0: minor-version must be N, devel, alphaN, betaN, or rcN\n";
}

# Create various required forms of the version number
if ($dotneeded)
{
	$fullversion = $majorversion . "." . $minor;
}
else
{
	$fullversion = $majorversion . $minor;
}
$numericversion = $majorversion . "." . $numericminor;
$padnumericversion = sprintf("%d%04d", $majorversion, $numericminor);

# Get the autoconf version number for eventual nag message
# (this also ensures we're in the right directory)

$aconfver = "";
open(FILE, "configure.in") || die "could not read configure.in: $!\n";
while (<FILE>)
{
	if (
m/^m4_if\(m4_defn\(\[m4_PACKAGE_VERSION\]\), \[(.*)\], \[\], \[m4_fatal/)
	{
		$aconfver = $1;
		last;
	}
}
close(FILE);
$aconfver ne ""
  || die "could not find autoconf version number in configure.in\n";

# Update configure.in and other files that contain version numbers

$fixedfiles = "";

sed_file("configure.in",
"-e 's/AC_INIT(\\[PostgreSQL\\], \\[[0-9a-z.]*\\]/AC_INIT([PostgreSQL], [$fullversion]/'"
);

sed_file("doc/bug.template",
"-e 's/PostgreSQL version (example: PostgreSQL .*) *:  PostgreSQL .*/PostgreSQL version (example: PostgreSQL $fullversion):  PostgreSQL $fullversion/'"
);

sed_file("src/include/pg_config.h.win32",
"-e 's/#define PACKAGE_STRING \"PostgreSQL .*\"/#define PACKAGE_STRING \"PostgreSQL $fullversion\"/' "
	  . "-e 's/#define PACKAGE_VERSION \".*\"/#define PACKAGE_VERSION \"$fullversion\"/' "
	  . "-e 's/#define PG_VERSION \".*\"/#define PG_VERSION \"$fullversion\"/' "
	  . "-e 's/#define PG_VERSION_NUM .*/#define PG_VERSION_NUM $padnumericversion/'"
);

sed_file("src/interfaces/libpq/libpq.rc.in",
"-e 's/FILEVERSION [0-9]*,[0-9]*,[0-9]*,0/FILEVERSION $majorversion,0,$numericminor,0/' "
	  . "-e 's/PRODUCTVERSION [0-9]*,[0-9]*,[0-9]*,0/PRODUCTVERSION $majorversion,0,$numericminor,0/' "
	  . "-e 's/VALUE \"FileVersion\", \"[0-9.]*/VALUE \"FileVersion\", \"$numericversion/' "
	  . "-e 's/VALUE \"ProductVersion\", \"[0-9.]*/VALUE \"ProductVersion\", \"$numericversion/'"
);

sed_file("src/port/win32ver.rc",
"-e 's/FILEVERSION    [0-9]*,[0-9]*,[0-9]*,0/FILEVERSION    $majorversion,0,$numericminor,0/' "
	  . "-e 's/PRODUCTVERSION [0-9]*,[0-9]*,[0-9]*,0/PRODUCTVERSION $majorversion,0,$numericminor,0/'"
);

print "Stamped these files with version number $fullversion:\n$fixedfiles";
print "Don't forget to run autoconf $aconfver before committing.\n";

exit 0;

sub sed_file
{
	my ($filename, $sedargs) = @_;
	my ($tmpfilename) = $filename . ".tmp";

	system("sed $sedargs $filename >$tmpfilename") == 0
	  or die "sed failed: $?";
	system("mv $tmpfilename $filename") == 0
	  or die "mv failed: $?";

	$fixedfiles .= "\t$filename\n";
}
