
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('postgres');
program_version_ok('postgres');
program_options_handling_ok('postgres');

done_testing();
