# -*-perl-*- hey - emacs - this is a perl file

# src/tools/msvc/build.pl

use strict;

BEGIN
{

	chdir("../../..") if (-d "../msvc" && -d "../../../src");

}

use lib "src/tools/msvc";

use Cwd;

use Mkvcbuild;

# buildenv.pl is for specifying the build environment settings
# it should contain lines like:
# $ENV{PATH} = "c:/path/to/bison/bin;$ENV{PATH}";

if (-e "src/tools/msvc/buildenv.pl")
{
	do "src/tools/msvc/buildenv.pl";
}
elsif (-e "./buildenv.pl")
{
	do "./buildenv.pl";
}

# set up the project
our $config;
do "config_default.pl";
do "config.pl" if (-f "src/tools/msvc/config.pl");

my $vcver = Mkvcbuild::mkvcbuild($config);

# check what sort of build we are doing

my $bconf     = $ENV{CONFIG}   || "Release";
my $msbflags  = $ENV{MSBFLAGS} || "";
my $buildwhat = $ARGV[1]       || "";
if (uc($ARGV[0]) eq 'DEBUG')
{
	$bconf = "Debug";
}
elsif (uc($ARGV[0]) ne "RELEASE")
{
	$buildwhat = $ARGV[0] || "";
}

# ... and do it

if ($buildwhat and $vcver >= 10.00)
{
	system(
"msbuild $buildwhat.vcxproj /verbosity:normal $msbflags /p:Configuration=$bconf"
	);
}
elsif ($buildwhat)
{
	system("vcbuild $msbflags $buildwhat.vcproj $bconf");
}
else
{
	system(
"msbuild pgsql.sln /verbosity:normal $msbflags /p:Configuration=$bconf");
}

# report status

my $status = $? >> 8;

exit $status;
