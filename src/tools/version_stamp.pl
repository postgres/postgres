#! /usr/bin/perl

#################################################################
# version_stamp.pl -- update version stamps throughout the source tree
#
# Copyright (c) 2008-2021, PostgreSQL Global Development Group
#
# src/tools/version_stamp.pl
#################################################################

#
# This script updates the version stamp in configure.ac, and also in assorted
# other files wherein it's not convenient to obtain the version number from
# configure's output.  Note that you still have to run autoconf afterward
# to regenerate configure from the updated configure.ac.
#
# Usage: cd to top of source tree and issue
#	src/tools/version_stamp.pl MINORVERSION
# where MINORVERSION can be a minor release number (0, 1, etc), or
# "devel", "alphaN", "betaN", "rcN".
#

use strict;
use warnings;

# Major version is hard-wired into the script.  We update it when we branch
# a new development version.
my $majorversion = 14;

# Validate argument and compute derived variables
my $minor = shift;
defined($minor) || die "$0: missing required argument: minor-version\n";

my ($dotneeded);

if ($minor =~ m/^\d+$/)
{
	$dotneeded = 1;
}
elsif ($minor eq "devel")
{
	$dotneeded = 0;
}
elsif ($minor =~ m/^alpha\d+$/)
{
	$dotneeded = 0;
}
elsif ($minor =~ m/^beta\d+$/)
{
	$dotneeded = 0;
}
elsif ($minor =~ m/^rc\d+$/)
{
	$dotneeded = 0;
}
else
{
	die "$0: minor-version must be N, devel, alphaN, betaN, or rcN\n";
}

my $fullversion;

# Create various required forms of the version number
if ($dotneeded)
{
	$fullversion = $majorversion . "." . $minor;
}
else
{
	$fullversion = $majorversion . $minor;
}

# Get the autoconf version number for eventual nag message
# (this also ensures we're in the right directory)

my $aconfver = "";
open(my $fh, '<', "configure.ac") || die "could not read configure.ac: $!\n";
while (<$fh>)
{
	if (m/^m4_if\(m4_defn\(\[m4_PACKAGE_VERSION\]\), \[(.*)\], \[\], \[m4_fatal/
	  )
	{
		$aconfver = $1;
		last;
	}
}
close($fh);
$aconfver ne ""
  || die "could not find autoconf version number in configure.ac\n";

# Update configure.ac and other files that contain version numbers

my $fixedfiles = "";

sed_file("configure.ac",
	"-e 's/AC_INIT(\\[PostgreSQL\\], \\[[0-9a-z.]*\\]/AC_INIT([PostgreSQL], [$fullversion]/'"
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
	return;
}
