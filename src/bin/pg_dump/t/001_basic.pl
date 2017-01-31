use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 42;

my $tempdir       = TestLib::tempdir;
my $tempdir_short = TestLib::tempdir_short;

#########################################
# Basic checks

program_help_ok('pg_dump');
program_version_ok('pg_dump');
program_options_handling_ok('pg_dump');

program_help_ok('pg_restore');
program_version_ok('pg_restore');
program_options_handling_ok('pg_restore');

program_help_ok('pg_dumpall');
program_version_ok('pg_dumpall');
program_options_handling_ok('pg_dumpall');

#########################################
# Test various invalid options and disallowed combinations
# Doesn't require a PG instance to be set up, so do this first.

command_exit_is([ 'pg_dump', 'qqq', 'abc' ],
	1, 'pg_dump: too many command-line arguments (first is "asd")');

command_exit_is([ 'pg_restore', 'qqq', 'abc' ],
	1, 'pg_restore too many command-line arguments (first is "asd")');

command_exit_is([ 'pg_dumpall', 'qqq', 'abc' ],
	1, 'pg_dumpall: too many command-line arguments (first is "qqq")');

command_exit_is(
	[ 'pg_dump', '-s', '-a' ],
	1,
'pg_dump: options -s/--schema-only and -a/--data-only cannot be used together'
);

command_exit_is(
	[ 'pg_restore', '-s', '-a' ],
	1,
'pg_restore: options -s/--schema-only and -a/--data-only cannot be used together'
);

command_exit_is([ 'pg_restore', '-d', 'xxx', '-f', 'xxx' ],
	1,
	'pg_restore: options -d/--dbname and -f/--file cannot be used together');

command_exit_is(
	[ 'pg_dump', '-c', '-a' ],
	1,
	'pg_dump: options -c/--clean and -a/--data-only cannot be used together');

command_exit_is(
	[ 'pg_restore', '-c', '-a' ],
	1,
'pg_restore: options -c/--clean and -a/--data-only cannot be used together');

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

command_exit_is(
	[ 'pg_restore', '--single-transaction', '-j3' ],
	1,
	'pg_restore: cannot specify both --single-transaction and multiple jobs');

command_exit_is([ 'pg_restore', '--if-exists' ],
	1, 'pg_restore: option --if-exists requires option -c/--clean');

# pg_dumpall command-line argument checks
command_exit_is(
	[ 'pg_dumpall', '-g', '-r' ],
	1,
'pg_restore: options -g/--globals-only and -r/--roles-only cannot be used together'
);

command_exit_is(
	[ 'pg_dumpall', '-g', '-t' ],
	1,
'pg_restore: options -g/--globals-only and -t/--tablespaces-only cannot be used together'
);

command_exit_is(
	[ 'pg_dumpall', '-r', '-t' ],
	1,
'pg_restore: options -r/--roles-only and -t/--tablespaces-only cannot be used together'
);

command_exit_is([ 'pg_dumpall', '--if-exists' ],
	1, 'pg_dumpall: option --if-exists requires option -c/--clean');
