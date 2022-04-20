
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

#
# Test that running pg_rewind with the source and target clusters
# on the same timeline runs successfully.
#
use strict;
use warnings;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

RewindTest::setup_cluster();
RewindTest::start_primary();
RewindTest::create_standby();
RewindTest::run_pg_rewind('local');
RewindTest::clean_rewind_test();

done_testing();
