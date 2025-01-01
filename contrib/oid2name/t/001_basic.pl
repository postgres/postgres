
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

#########################################
# Basic checks

program_help_ok('oid2name');
program_version_ok('oid2name');
program_options_handling_ok('oid2name');

done_testing();
