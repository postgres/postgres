#
# Test that running pg_rewind with the source and target clusters
# on the same timeline runs successfully.
#
use strict;
use warnings;
use TestLib;
use Test::More tests => 1;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

RewindTest::setup_cluster();
RewindTest::start_master();
RewindTest::create_standby();
RewindTest::run_pg_rewind('local');
RewindTest::clean_rewind_test();
exit(0);
