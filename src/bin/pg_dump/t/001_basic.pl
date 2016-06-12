use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 15;

my $tempdir       = TestLib::tempdir;
my $tempdir_short = TestLib::tempdir_short;

#########################################
# Basic checks

program_help_ok('pg_dump');
program_version_ok('pg_dump');
program_options_handling_ok('pg_dump');

#########################################
# Test various invalid options and disallowed combinations
# Doesn't require a PG instance to be set up, so do this first.

command_exit_is([ 'pg_dump', 'qqq', 'abc' ],
	1, 'pg_dump: too many command-line arguments (first is "asd")');

command_exit_is(
	[ 'pg_dump', '-s', '-a' ],
	1,
'pg_dump: options -s/--schema-only and -a/--data-only cannot be used together'
);

command_exit_is(
	[ 'pg_dump', '-c', '-a' ],
	1,
	'pg_dump: options -c/--clean and -a/--data-only cannot be used together');

command_exit_is(
	[ 'pg_dump', '--inserts', '-o' ],
	1,
'pg_dump: options --inserts/--column-inserts and -o/--oids cannot be used together'
);

command_exit_is([ 'pg_dump', '--if-exists' ],
	1, 'pg_dump: option --if-exists requires option -c/--clean');

command_exit_is([ 'pg_dump', '-j' ],
	1, 'pg_dump: option requires an argument -- \'j\'');

command_exit_is([ 'pg_dump', '-j3' ],
	1, 'pg_dump: parallel backup only supported by the directory format');
