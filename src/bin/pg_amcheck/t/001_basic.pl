
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;

use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_amcheck');
program_version_ok('pg_amcheck');
program_options_handling_ok('pg_amcheck');

done_testing();
