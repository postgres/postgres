# Copyright (c) 2022-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_upgrade');
program_version_ok('pg_upgrade');
program_options_handling_ok('pg_upgrade');

done_testing();
