#
# Script that parses Unix style build environment and generates build files
# for building with Visual Studio.
#
# src/tools/msvc/mkvcbuild.pl
#
use strict;
use warnings;

use Mkvcbuild;

chdir('..\..\..') if (-d '..\msvc' && -d '..\..\..\src');
die 'Must run from root or msvc directory'
  unless (-d 'src\tools\msvc' && -d 'src');

die 'Could not find config_default.pl'
  unless (-f 'src/tools/msvc/config_default.pl');
print "Warning: no config.pl found, using default.\n"
  unless (-f 'src/tools/msvc/config.pl');

our $config;
do 'src/tools/msvc/config_default.pl';
do 'src/tools/msvc/config.pl' if (-f 'src/tools/msvc/config.pl');

Mkvcbuild::mkvcbuild($config);
