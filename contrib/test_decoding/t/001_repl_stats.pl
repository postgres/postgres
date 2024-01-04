
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test replication statistics data in pg_stat_replication_slots is sane after
# drop replication slot and restart.
use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Test set-up
my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'synchronous_commit = on');
$node->start;

# Check that replication slot stats are expected.
sub test_slot_stats
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $expected, $msg) = @_;

	my $result = $node->safe_psql(
		'postgres', qq[
		SELECT slot_name, total_txns > 0 AS total_txn,
			   total_bytes > 0 AS total_bytes
			   FROM pg_stat_replication_slots
			   ORDER BY slot_name]);
	is($result, $expected, $msg);
}

# Create table.
$node->safe_psql('postgres', "CREATE TABLE test_repl_stat(col1 int)");

# Create replication slots.
$node->safe_psql(
	'postgres', qq[
	SELECT pg_create_logical_replication_slot('regression_slot1', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot2', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot3', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot4', 'test_decoding');
]);

# Insert some data.
$node->safe_psql('postgres',
	"INSERT INTO test_repl_stat values(generate_series(1, 5));");

$node->safe_psql(
	'postgres', qq[
	SELECT data FROM pg_logical_slot_get_changes('regression_slot1', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot2', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot3', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
	SELECT data FROM pg_logical_slot_get_changes('regression_slot4', NULL,
	NULL, 'include-xids', '0', 'skip-empty-xacts', '1');
]);

# Wait for the statistics to be updated.
$node->poll_query_until(
	'postgres', qq[
	SELECT count(slot_name) >= 4 FROM pg_stat_replication_slots
	WHERE slot_name ~ 'regression_slot'
	AND total_txns > 0 AND total_bytes > 0;
]) or die "Timed out while waiting for statistics to be updated";

# Test to drop one of the replication slot and verify replication statistics data is
# fine after restart.
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot4')");

$node->stop;
$node->start;

# Verify statistics data present in pg_stat_replication_slots are sane after
# restart.
test_slot_stats(
	$node,
	qq(regression_slot1|t|t
regression_slot2|t|t
regression_slot3|t|t),
	'check replication statistics are updated');

# Test to remove one of the replication slots and adjust
# max_replication_slots accordingly to the number of slots. This leads
# to a mismatch between the number of slots present in the stats file and the
# number of stats present in shared memory. We verify
# replication statistics data is fine after restart.

$node->stop;
my $datadir = $node->data_dir;
my $slot3_replslotdir = "$datadir/pg_replslot/regression_slot3";

rmtree($slot3_replslotdir);

$node->append_conf('postgresql.conf', 'max_replication_slots = 2');
$node->start;

# Verify statistics data present in pg_stat_replication_slots are sane after
# restart.
test_slot_stats(
	$node,
	qq(regression_slot1|t|t
regression_slot2|t|t),
	'check replication statistics after removing the slot file');

# cleanup
$node->safe_psql('postgres', "DROP TABLE test_repl_stat");
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot1')");
$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('regression_slot2')");

# shutdown
$node->stop;

done_testing();
