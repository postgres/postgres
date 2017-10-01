use strict;
use warnings;
use TestLib;
use Test::More tests => 10;

program_help_ok('pg_isready');
program_version_ok('pg_isready');
program_options_handling_ok('pg_isready');

command_fails(['pg_isready'], 'fails with no server running');

my $tempdir = tempdir;
start_test_server $tempdir;

# use a long timeout for the benefit of very slow buildfarm machines
command_ok([qw(pg_isready --timeout=60)], 'succeeds with server running');
