
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster with
# streaming replication
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# Initialize primary node
my $node_primary = PostgreSQL::Test::Cluster->new('standby_restarts_primary');
$node_primary->init(allows_streaming => 1, no_data_checksums => 1);
$node_primary->start;

my $slotname = 'physical_slot';
$node_primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('$slotname')");

# Take backup
my $backup_name = 'my_backup';
$node_primary->backup($backup_name);

# Create streaming standby linking to primary
my $node_standby = PostgreSQL::Test::Cluster->new('standby_restarts_standby');
$node_standby->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq[
primary_slot_name = '$slotname'
]);
$node_standby->start;

# Create some content on the primary to have un-checksummed data in the cluster
$node_primary->safe_psql('postgres',
	"CREATE TABLE t AS SELECT generate_series(1,10000) AS a;");

# Wait for standby to catch up
$node_primary->wait_for_catchup($node_standby, 'replay',
	$node_primary->lsn('insert'));

# Check that checksums are turned off on all nodes
test_checksum_state($node_primary, 'off');
test_checksum_state($node_standby, 'off');

# ---------------------------------------------------------------------------
# Enable checksums for the cluster, and make sure that both the primary and
# standby change state.
#

# Initiate enabling of checksums and ensure that the primary switches to
# either "inprogress-on" or "on"
enable_data_checksums($node_primary);
my $result = $node_primary->poll_query_until(
	'postgres',
	"SELECT setting = 'off' FROM pg_catalog.pg_settings WHERE name = 'data_checksums';",
	'f');
is($result, 1, 'ensure primary has transitioned from off');
# Wait for checksum enable to be replayed
$node_primary->wait_for_catchup($node_standby, 'replay');

# Ensure that the standby has switched to "inprogress-on" or "on".  Normally it
# would be "inprogress-on", but it is theoretically possible for the primary to
# complete the checksum enabling *and* have the standby replay that record
# before we reach the check below.
$result = $node_standby->poll_query_until(
	'postgres',
	"SELECT setting = 'off' FROM pg_catalog.pg_settings WHERE name = 'data_checksums';",
	'f');
is($result, 1, 'ensure standby has absorbed the inprogress-on barrier');
$result = $node_standby->safe_psql('postgres',
	"SELECT setting FROM pg_catalog.pg_settings WHERE name = 'data_checksums';"
);

is(($result eq 'inprogress-on' || $result eq 'on'),
	1, 'ensure checksums are on, or in progress, on standby_1');

# Insert some more data which should be checksummed on INSERT
$node_primary->safe_psql('postgres',
	"INSERT INTO t VALUES (generate_series(1, 10000));");

# Wait for checksums enabled on the primary and standby
wait_for_checksum_state($node_primary, 'on');
wait_for_checksum_state($node_standby, 'on');

$result =
  $node_primary->safe_psql('postgres', "SELECT count(a) FROM t WHERE a > 1");
is($result, '19998', 'ensure we can safely read all data with checksums');

$result = $node_primary->poll_query_until(
	'postgres',
	"SELECT count(*) FROM pg_stat_activity WHERE backend_type LIKE 'datachecksum%';",
	'0');
is($result, 1, 'await datachecksums worker/launcher termination');

#
# Disable checksums and ensure it's propagated to standby and that we can
# still read all data
#

# Disable checksums and wait for the operation to be replayed
disable_data_checksums($node_primary);
$node_primary->wait_for_catchup($node_standby, 'replay');
# Ensure that the primary and standby has switched to off
wait_for_checksum_state($node_primary, 'off');
wait_for_checksum_state($node_standby, 'off');
# Double-check reading data without errors
$result =
  $node_primary->safe_psql('postgres', "SELECT count(a) FROM t WHERE a > 1");
is($result, "19998", 'ensure we can safely read all data without checksums');

$node_standby->stop;
$node_primary->stop;
done_testing();
