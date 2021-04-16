# Test replication statistics data in pg_stat_replication_slots is sane after
# drop replication slot and restart.
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 1;

# Test set-up
my $node = get_new_node('test');
$node->init(allows_streaming => 'logical');
$node->append_conf('postgresql.conf', 'synchronous_commit = on');
$node->start;

# Create table.
$node->safe_psql('postgres',
        "CREATE TABLE test_repl_stat(col1 int)");

# Create replication slots.
$node->safe_psql(
	'postgres', qq[
	SELECT pg_create_logical_replication_slot('regression_slot1', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot2', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot3', 'test_decoding');
	SELECT pg_create_logical_replication_slot('regression_slot4', 'test_decoding');
]);

# Insert some data.
$node->safe_psql('postgres', "INSERT INTO test_repl_stat values(generate_series(1, 5));");

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
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('regression_slot4')");

$node->stop;
$node->start;

# Verify statistics data present in pg_stat_replication_slots are sane after
# restart.
my $result = $node->safe_psql('postgres',
	"SELECT slot_name, total_txns > 0 AS total_txn,
	total_bytes > 0 AS total_bytes FROM pg_stat_replication_slots
	ORDER BY slot_name"
);
is($result, qq(regression_slot1|t|t
regression_slot2|t|t
regression_slot3|t|t), 'check replication statistics are updated');

# cleanup
$node->safe_psql('postgres', "DROP TABLE test_repl_stat");
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('regression_slot1')");
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('regression_slot2')");
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('regression_slot3')");

# shutdown
$node->stop;
