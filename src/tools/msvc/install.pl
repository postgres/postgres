#
# Script that provides 'make install' functionality for msvc builds
#
# src/tools/msvc/install.pl
#
use strict;
use warnings;

use Install qw(Install);

# buildenv.pl is for specifying the build environment settings
# it should contain lines like:
# $ENV{PATH} = "c:/path/to/bison/bin;$ENV{PATH}";

if (-e "src/tools/msvc/buildenv.pl")
{
	require "src/tools/msvc/buildenv.pl";
}
elsif (-e "./buildenv.pl")
{
	require "./buildenv.pl";
}

my $target = shift || Usage();
Install($target);

sub Usage
{
	print "Usage: install.pl <targetdir>\n";
	exit(1);
}
