use strict;
use warnings;

use Config;
use PostgresNode;
use TestLib;
use Test::More tests => 78;

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

command_fails_like(
	[ 'pg_dump', 'qqq', 'abc' ],
	qr/\Qpg_dump: error: too many command-line arguments (first is "abc")\E/,
	'pg_dump: too many command-line arguments');

command_fails_like(
	[ 'pg_restore', 'qqq', 'abc' ],
	qr/\Qpg_restore: error: too many command-line arguments (first is "abc")\E/,
	'pg_restore: too many command-line arguments');

command_fails_like(
	[ 'pg_dumpall', 'qqq', 'abc' ],
	qr/\Qpg_dumpall: error: too many command-line arguments (first is "qqq")\E/,
	'pg_dumpall: too many command-line arguments');

command_fails_like(
	[ 'pg_dump', '-s', '-a' ],
	qr/\Qpg_dump: error: options -s\/--schema-only and -a\/--data-only cannot be used together\E/,
	'pg_dump: options -s/--schema-only and -a/--data-only cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '-s', '--include-foreign-data=xxx' ],
	qr/\Qpg_dump: error: options -s\/--schema-only and --include-foreign-data cannot be used together\E/,
	'pg_dump: options -s/--schema-only and --include-foreign-data cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '-j2', '--include-foreign-data=xxx' ],
	qr/\Qpg_dump: error: option --include-foreign-data is not supported with parallel backup\E/,
	'pg_dump: option --include-foreign-data is not supported with parallel backup'
);

command_fails_like(
	['pg_restore'],
	qr{\Qpg_restore: error: one of -d/--dbname and -f/--file must be specified\E},
	'pg_restore: error: one of -d/--dbname and -f/--file must be specified');

command_fails_like(
	[ 'pg_restore', '-s', '-a', '-f -' ],
	qr/\Qpg_restore: error: options -s\/--schema-only and -a\/--data-only cannot be used together\E/,
	'pg_restore: options -s/--schema-only and -a/--data-only cannot be used together'
);

command_fails_like(
	[ 'pg_restore', '-d', 'xxx', '-f', 'xxx' ],
	qr/\Qpg_restore: error: options -d\/--dbname and -f\/--file cannot be used together\E/,
	'pg_restore: options -d/--dbname and -f/--file cannot be used together');

command_fails_like(
	[ 'pg_dump', '-c', '-a' ],
	qr/\Qpg_dump: error: options -c\/--clean and -a\/--data-only cannot be used together\E/,
	'pg_dump: options -c/--clean and -a/--data-only cannot be used together');

command_fails_like(
	[ 'pg_restore', '-c', '-a', '-f -' ],
	qr/\Qpg_restore: error: options -c\/--clean and -a\/--data-only cannot be used together\E/,
	'pg_restore: options -c/--clean and -a/--data-only cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '--if-exists' ],
	qr/\Qpg_dump: error: option --if-exists requires option -c\/--clean\E/,
	'pg_dump: option --if-exists requires option -c/--clean');

command_fails_like(
	[ 'pg_dump', '-j3' ],
	qr/\Qpg_dump: error: parallel backup only supported by the directory format\E/,
	'pg_dump: parallel backup only supported by the directory format');

command_fails_like(
	[ 'pg_dump', '-j', '-1' ],
	qr/\Qpg_dump: error: invalid number of parallel jobs\E/,
	'pg_dump: invalid number of parallel jobs');

command_fails_like(
	[ 'pg_dump', '-F', 'garbage' ],
	qr/\Qpg_dump: error: invalid output format\E/,
	'pg_dump: invalid output format');

command_fails_like(
	[ 'pg_restore', '-j', '-1', '-f -' ],
	qr/\Qpg_restore: error: invalid number of parallel jobs\E/,
	'pg_restore: invalid number of parallel jobs');

command_fails_like(
	[ 'pg_restore', '--single-transaction', '-j3', '-f -' ],
	qr/\Qpg_restore: error: cannot specify both --single-transaction and multiple jobs\E/,
	'pg_restore: cannot specify both --single-transaction and multiple jobs');

command_fails_like(
	[ 'pg_dump', '-Z', '-1' ],
	qr/\Qpg_dump: error: compression level must be in range 0..9\E/,
	'pg_dump: compression level must be in range 0..9');

command_fails_like(
	[ 'pg_restore', '--if-exists', '-f -' ],
	qr/\Qpg_restore: error: option --if-exists requires option -c\/--clean\E/,
	'pg_restore: option --if-exists requires option -c/--clean');

command_fails_like(
	[ 'pg_restore', '-f -', '-F', 'garbage' ],
	qr/\Qpg_restore: error: unrecognized archive format "garbage";\E/,
	'pg_dump: unrecognized archive format');

command_fails_like(
	[ 'pg_dump', '--on-conflict-do-nothing' ],
	qr/pg_dump: error: option --on-conflict-do-nothing requires option --inserts, --rows-per-insert, or --column-inserts/,
	'pg_dump: --on-conflict-do-nothing requires --inserts, --rows-per-insert, --column-inserts'
);

# pg_dumpall command-line argument checks
command_fails_like(
	[ 'pg_dumpall', '-g', '-r' ],
	qr/\Qpg_dumpall: error: options -g\/--globals-only and -r\/--roles-only cannot be used together\E/,
	'pg_dumpall: options -g/--globals-only and -r/--roles-only cannot be used together'
);

command_fails_like(
	[ 'pg_dumpall', '-g', '-t' ],
	qr/\Qpg_dumpall: error: options -g\/--globals-only and -t\/--tablespaces-only cannot be used together\E/,
	'pg_dumpall: options -g/--globals-only and -t/--tablespaces-only cannot be used together'
);

command_fails_like(
	[ 'pg_dumpall', '-r', '-t' ],
	qr/\Qpg_dumpall: error: options -r\/--roles-only and -t\/--tablespaces-only cannot be used together\E/,
	'pg_dumpall: options -r/--roles-only and -t/--tablespaces-only cannot be used together'
);

command_fails_like(
	[ 'pg_dumpall', '--if-exists' ],
	qr/\Qpg_dumpall: error: option --if-exists requires option -c\/--clean\E/,
	'pg_dumpall: option --if-exists requires option -c/--clean');

command_fails_like(
	[ 'pg_restore', '-C', '-1', '-f -' ],
	qr/\Qpg_restore: error: options -C\/--create and -1\/--single-transaction cannot be used together\E/,
	'pg_restore: options -C\/--create and -1\/--single-transaction cannot be used together'
);

# also fails for -r and -t, but it seems pointless to add more tests for those.
command_fails_like(
	[ 'pg_dumpall', '--exclude-database=foo', '--globals-only' ],
	qr/\Qpg_dumpall: error: option --exclude-database cannot be used together with -g\/--globals-only\E/,
	'pg_dumpall: option --exclude-database cannot be used together with -g/--globals-only'
);
