# Test generic xlog record work for bloom index replication.
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 31;

my $node_master;
my $node_standby;

# Run few queries on both master and standby and check their results match.
sub test_index_replay
{
	my ($test_name) = @_;

	# Wait for standby to catch up
	my $applname = $node_standby->name;
	my $caughtup_query =
"SELECT pg_current_xlog_location() <= write_location FROM pg_stat_replication WHERE application_name = '$applname';";
	$node_master->poll_query_until('postgres', $caughtup_query)
	  or die "Timed out while waiting for standby 1 to catch up";

	my $queries = qq(SET enable_seqscan=off;
SET enable_bitmapscan=on;
SET enable_indexscan=on;
SELECT * FROM tst WHERE i = 0;
SELECT * FROM tst WHERE i = 3;
SELECT * FROM tst WHERE t = 'b';
SELECT * FROM tst WHERE t = 'f';
SELECT * FROM tst WHERE i = 3 AND t = 'c';
SELECT * FROM tst WHERE i = 7 AND t = 'e';
);

	# Run test queries and compare their result
	my $master_result = $node_master->psql("postgres", $queries);
	my $standby_result = $node_standby->psql("postgres", $queries);

	is($master_result, $standby_result, "$test_name: query result matches");
}

# Initialize master node
$node_master = get_new_node('master');
$node_master->init(allows_streaming => 1);
$node_master->start;
my $backup_name = 'my_backup';

# Take backup
$node_master->backup($backup_name);

# Create streaming standby linking to master
$node_standby = get_new_node('standby');
$node_standby->init_from_backup($node_master, $backup_name,
	has_streaming => 1);
$node_standby->start;

# Create some bloom index on master
$node_master->psql("postgres", "CREATE EXTENSION bloom;");
$node_master->psql("postgres", "CREATE TABLE tst (i int4, t text);");
$node_master->psql("postgres",
"INSERT INTO tst SELECT i%10, substr(md5(i::text), 1, 1) FROM generate_series(1,100000) i;"
);
$node_master->psql("postgres",
	"CREATE INDEX bloomidx ON tst USING bloom (i, t) WITH (col1 = 3);");

# Test that queries give same result
test_index_replay('initial');

# Run 10 cycles of table modification. Run test queries after each modification.
for my $i (1 .. 10)
{
	$node_master->psql("postgres", "DELETE FROM tst WHERE i = $i;");
	test_index_replay("delete $i");
	$node_master->psql("postgres", "VACUUM tst;");
	test_index_replay("vacuum $i");
	my ($start, $end) = (100001 + ($i - 1) * 10000, 100000 + $i * 10000);
	$node_master->psql("postgres",
"INSERT INTO tst SELECT i%10, substr(md5(i::text), 1, 1) FROM generate_series($start,$end) i;"
	);
	test_index_replay("insert $i");
}
