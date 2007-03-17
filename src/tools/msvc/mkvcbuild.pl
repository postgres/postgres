#
# Script that parses Unix style build environment and generates build files
# for building with Visual Studio.
#
# $PostgreSQL: pgsql/src/tools/msvc/mkvcbuild.pl,v 1.18 2007/03/17 14:01:01 mha Exp $
#
use strict;
use warnings;

use Mkvcbuild;

chdir('..\..\..') if (-d '..\msvc' && -d '..\..\..\src');
die 'Must run from root or msvc directory' unless (-d 'src\tools\msvc' && -d 'src');

die 'Could not find config.pl' unless (-f 'src/tools/msvc/config.pl');

our $config;
require 'src/tools/msvc/config.pl';

Mkvcbuild::mkvcbuild($config);
