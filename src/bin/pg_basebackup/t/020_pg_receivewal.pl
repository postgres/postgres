use strict;
use warnings;
use TestLib;
use Test::More tests => 8;

program_help_ok('pg_receivewal');
program_version_ok('pg_receivewal');
program_options_handling_ok('pg_receivewal');
