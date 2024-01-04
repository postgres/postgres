# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

program_help_ok('pg_combinebackup');
program_version_ok('pg_combinebackup');
program_options_handling_ok('pg_combinebackup');

command_fails_like(
	['pg_combinebackup'],
	qr/no input directories specified/,
	'input directories must be specified');
command_fails_like(
	[ 'pg_combinebackup', $tempdir ],
	qr/no output directory specified/,
	'output directory must be specified');

done_testing();
