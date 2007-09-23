
# -*-perl-*- hey - emacs - this is a perl file

# $PostgreSQL: pgsql/src/tools/msvc/build.pl,v 1.1 2007/09/23 21:52:56 adunstan Exp $

BEGIN
{
	
	chdir("../../..") if  (-d "../msvc" && -d "../../../src");
	
}

use lib "src/tools/msvc";

use Cwd;

use Mkvcbuild;

# buildenv.pl is for specifying the build environment settings
# it should contain lines like:
# $ENV{PATH} = "c:/path/to/bison/bin;$ENV{PATH}";

if ( -e "src/tools/msvc/buildenv.pl")
{
    require "src/tools/msvc/buildenv.pl";
}
elsif (-e "./buildenv.pl" )
{
    require "./buildenv.pl";
}


# set up the project
our $config;
require "config.pl";

Mkvcbuild::mkvcbuild($config);

# check what sort of build we are doing

my $bconf = $ENV{CONFIG} || "Release";
my $buildwhat = $ARGV[1] || "";
if ($ARGV[0] eq 'DEBUG')
{
    $bconf = "Debug";
}
elsif ($ARGV[0] ne "RELEASE")
{
    $buildwhat = $ARGV[0] || "";
}

# ... and do it

if ($buildwhat)
{
    system("vcbuild $buildwhat.vcproj $bconf");
}
else
{
    system("msbuild pgsql.sln /verbosity:detailed /p:Configuration=$bconf");
}

# report status

$status = $? >> 8;

exit $status;
