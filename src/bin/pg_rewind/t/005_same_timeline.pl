use strict;
use warnings;
use TestLib;
use Test::More tests => 1;

use RewindTest;

# Test that running pg_rewind if the two clusters are on the same
# timeline runs successfully.

RewindTest::setup_cluster();
RewindTest::start_master();
RewindTest::create_standby();
RewindTest::run_pg_rewind('local');
RewindTest::clean_rewind_test();
exit(0);
