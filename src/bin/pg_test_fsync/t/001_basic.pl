
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

#########################################
# Basic checks

program_help_ok('pg_test_fsync');
program_version_ok('pg_test_fsync');
program_options_handling_ok('pg_test_fsync');

#########################################
# Test invalid option combinations

command_fails_like(
	[ 'pg_test_fsync', '--secs-per-test' => 'a' ],
	qr/\Qpg_test_fsync: error: invalid argument for option --secs-per-test\E/,
	'pg_test_fsync: invalid argument for option --secs-per-test');
command_fails_like(
	[ 'pg_test_fsync', '--secs-per-test' => '0' ],
	qr/\Qpg_test_fsync: error: --secs-per-test must be in range 1..4294967295\E/,
	'pg_test_fsync: --secs-per-test must be in range');

done_testing();
