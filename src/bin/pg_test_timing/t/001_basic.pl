
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

#########################################
# Basic checks

program_help_ok('pg_test_timing');
program_version_ok('pg_test_timing');
program_options_handling_ok('pg_test_timing');

#########################################
# Test invalid option combinations

command_fails_like(
	[ 'pg_test_timing', '--duration' => 'a' ],
	qr/\Qpg_test_timing: invalid argument for option --duration\E/,
	'pg_test_timing: invalid argument for option --duration');
command_fails_like(
	[ 'pg_test_timing', '--duration' => '0' ],
	qr/\Qpg_test_timing: --duration must be in range 1..4294967295\E/,
	'pg_test_timing: --duration must be in range');

done_testing();
