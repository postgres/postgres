#
# Script that provides 'make install' functionality for msvc builds
#
# $PostgreSQL: pgsql/src/tools/msvc/install.pl,v 1.7 2007/03/17 14:01:01 mha Exp $
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
