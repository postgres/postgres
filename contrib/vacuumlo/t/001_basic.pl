use strict;
use warnings;

use TestLib;
use Test::More tests => 8;

program_help_ok('vacuumlo');
program_version_ok('vacuumlo');
program_options_handling_ok('vacuumlo');
