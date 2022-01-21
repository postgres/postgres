# -*-perl-*- hey - emacs - this is a perl file

# Copyright (c) 2021-2022, PostgreSQL Global Development Group

#
# Script that provides 'make' functionality for msvc builds.
#
# src/tools/msvc/build.pl
#
use strict;
use warnings;

use FindBin;
use lib $FindBin::RealBin;

use Cwd;

use Mkvcbuild;

sub usage
{
	die(    "Usage: build.pl [ [ <configuration> ] <component> ]\n"
		  . "Options are case-insensitive.\n"
		  . "  configuration: Release | Debug.  This sets the configuration\n"
		  . "    to build.  Default is Release.\n"
		  . "  component: name of component to build.  An empty value means\n"
		  . "    to build all components.\n");
}

chdir('../../..') if (-d '../msvc' && -d '../../../src');
die 'Must run from root or msvc directory'
  unless (-d 'src/tools/msvc' && -d 'src');

usage() unless scalar(@ARGV) <= 2;

# buildenv.pl is for specifying the build environment settings
# it should contain lines like:
# $ENV{PATH} = "c:/path/to/bison/bin;$ENV{PATH}";

if (-e "src/tools/msvc/buildenv.pl")
{
	do "./src/tools/msvc/buildenv.pl";
}
elsif (-e "./buildenv.pl")
{
	do "./buildenv.pl";
}

# set up the project
our $config;
do "./src/tools/msvc/config_default.pl";
do "./src/tools/msvc/config.pl" if (-f "src/tools/msvc/config.pl");

my $vcver = Mkvcbuild::mkvcbuild($config);

# check what sort of build we are doing
my $bconf     = $ENV{CONFIG}   || "Release";
my $msbflags  = $ENV{MSBFLAGS} || "";
my $buildwhat = $ARGV[1]       || "";

if (defined($ARGV[0]))
{
	if (uc($ARGV[0]) eq 'DEBUG')
	{
		$bconf = "Debug";
	}
	elsif (uc($ARGV[0]) ne "RELEASE")
	{
		$buildwhat = $ARGV[0] || "";
	}
}

# ... and do it

if ($buildwhat)
{
	system(
		"msbuild $buildwhat.vcxproj /verbosity:normal $msbflags /p:Configuration=$bconf"
	);
}
else
{
	system(
		"msbuild pgsql.sln /verbosity:normal $msbflags /p:Configuration=$bconf"
	);
}

# report status

my $status = $? >> 8;

exit $status;
