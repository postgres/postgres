
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

#
# Test checking options of pg_rewind.
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_rewind');
program_version_ok('pg_rewind');
program_options_handling_ok('pg_rewind');

my $primary_pgdata = PostgreSQL::Test::Utils::tempdir;
my $standby_pgdata = PostgreSQL::Test::Utils::tempdir;
command_fails(
	[
		'pg_rewind', '--debug',
		'--target-pgdata', $primary_pgdata,
		'--source-pgdata', $standby_pgdata,
		'extra_arg1'
	],
	'too many arguments');
command_fails([ 'pg_rewind', '--target-pgdata', $primary_pgdata ],
	'no source specified');
command_fails(
	[
		'pg_rewind', '--debug',
		'--target-pgdata', $primary_pgdata,
		'--source-pgdata', $standby_pgdata,
		'--source-server', 'incorrect_source'
	],
	'both remote and local sources specified');
command_fails(
	[
		'pg_rewind', '--debug',
		'--target-pgdata', $primary_pgdata,
		'--source-pgdata', $standby_pgdata,
		'--write-recovery-conf'
	],
	'no local source with --write-recovery-conf');

done_testing();
