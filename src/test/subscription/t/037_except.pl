
# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical replication tests for EXCEPT TABLE publications
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# Initialize subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $result;

sub test_except_root_partition
{
	my ($pubviaroot) = @_;

	# If the root partitioned table is in the EXCEPT TABLE clause, all its
	# partitions are excluded from publication, regardless of the
	# publish_via_partition_root setting.
	$node_publisher->safe_psql(
		'postgres', qq(
		CREATE PUBLICATION tap_pub_part FOR ALL TABLES EXCEPT TABLE (root1) WITH (publish_via_partition_root = $pubviaroot);
		INSERT INTO root1 VALUES (1), (101);
	));
	$node_subscriber->safe_psql('postgres',
		"CREATE SUBSCRIPTION tap_sub_part CONNECTION '$publisher_connstr' PUBLICATION tap_pub_part"
	);
	$node_subscriber->wait_for_subscription_sync($node_publisher,
		'tap_sub_part');

	# Advance the replication slot to ignore changes generated before this point.
	$node_publisher->safe_psql('postgres',
		"SELECT slot_name FROM pg_replication_slot_advance('test_slot', pg_current_wal_lsn())"
	);
	$node_publisher->safe_psql('postgres',
		"INSERT INTO root1 VALUES (2), (102)");

	# Verify that data inserted into the partitioned table is not published when
	# it is in the EXCEPT TABLE clause.
	$result = $node_publisher->safe_psql('postgres',
		"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tap_pub_part')"
	);
	$node_publisher->wait_for_catchup('tap_sub_part');

	# Verify that no rows are replicated to subscriber for root or partitions.
	foreach my $table (qw(root1 part1 part2 part2_1))
	{
		$result = $node_subscriber->safe_psql('postgres',
			"SELECT count(*) FROM $table");
		is($result, qq(0), "no rows replicated to subscriber for $table");
	}

	$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_part");
	$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_part");
}

# ============================================
# EXCEPT TABLE test cases for non-partitioned tables and inherited tables.
# ============================================

# Create schemas and tables on publisher
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 AS SELECT generate_series(1,10) AS a;
	CREATE TABLE parent (a int);
	CREATE TABLE child (b int) INHERITS (parent);
	CREATE TABLE parent1 (a int);
	CREATE TABLE child1 (b int) INHERITS (parent1);
));

# Create schemas and tables on subscriber
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 (a int);
	CREATE TABLE parent (a int);
	CREATE TABLE child (b int) INHERITS (parent);
	CREATE TABLE parent1 (a int);
	CREATE TABLE child1 (b int) INHERITS (parent1);
));

# Exclude tab1 (non-inheritance case), and also exclude parent and ONLY parent1
# to verify exclusion behavior for inherited tables, including the effect of
# ONLY in the EXCEPT TABLE clause.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tab_pub FOR ALL TABLES EXCEPT TABLE (tab1, parent, only parent1)"
);

# Create a logical replication slot to help with later tests.
$node_publisher->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('test_slot', 'pgoutput')");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tab_sub CONNECTION '$publisher_connstr' PUBLICATION tab_pub"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tab_sub');

# Check the table data does not sync for the tables specified in EXCEPT TABLE
# clause.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(0),
	'check there is no initial data copied for the tables specified in the EXCEPT TABLE clause'
);

# Insert some data into the table listed in the EXCEPT TABLE clause
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab1 VALUES(generate_series(11,20));
	INSERT INTO child VALUES(generate_series(11,20), generate_series(11,20));
));

# Verify that data inserted into a table listed in the EXCEPT TABLE clause is
# not published.
$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) = 0 FROM pg_logical_slot_get_binary_changes('test_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'tab_pub')"
);
is($result, qq(t),
	'verify no changes for table listed in the EXCEPT TABLE clause are present in the replication slot'
);

# This should be published because ONLY parent1 was specified in the
# EXCEPT TABLE clause, so the exclusion applies only to the parent table and not
# to its child.
$node_publisher->safe_psql('postgres',
	"INSERT INTO child1 VALUES(generate_series(11,20), generate_series(11,20))"
);

# Verify that data inserted into a table listed in the EXCEPT TABLE clause is
# not replicated.
$node_publisher->wait_for_catchup('tab_sub');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM tab1");
is($result, qq(0), 'check replicated inserts on subscriber');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM child");
is($result, qq(0), 'check replicated inserts on subscriber');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM child1");
is($result, qq(10), 'check replicated inserts on subscriber');

# cleanup
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION tab_sub;
	TRUNCATE TABLE tab1;
	DROP TABLE parent, parent1, child, child1;
));
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tab_pub;
	TRUNCATE TABLE tab1;
    DROP TABLE parent, parent1, child, child1;
));

# ============================================
# EXCEPT TABLE test cases for partitioned tables
# ============================================
# Setup partitioned table and partitions on the publisher that map to normal
# tables on the subscriber.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE root1(a int) PARTITION BY RANGE(a);
	CREATE TABLE part1 PARTITION OF root1 FOR VALUES FROM (0) TO (100);
	CREATE TABLE part2 PARTITION OF root1 FOR VALUES FROM (100) TO (200) PARTITION BY RANGE(a);
	CREATE TABLE part2_1 PARTITION OF part2 FOR VALUES FROM (100) TO (150);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE root1(a int);
	CREATE TABLE part1(a int);
	CREATE TABLE part2(a int);
	CREATE TABLE part2_1(a int);
));

# Validate the behaviour with both publish_via_partition_root as true and false
test_except_root_partition('false');
test_except_root_partition('true');

# ============================================
# Test when a subscription is subscribing to multiple publications
# ============================================

# OK when a table is excluded by pub1 EXCEPT TABLE, but it is included by pub2
# FOR TABLE.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub1 FOR ALL TABLES EXCEPT TABLE (tab1);
	CREATE PUBLICATION tap_pub2 FOR TABLE tab1;
	INSERT INTO tab1 VALUES(1);
));
$node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_publisher->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT TABLE clause of one publication but included by another"
);
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION tap_pub2;
	TRUNCATE tab1;
));
$node_subscriber->safe_psql('postgres', qq(TRUNCATE tab1));

# OK when a table is excluded by pub1 EXCEPT TABLE, but it is included by pub2
# FOR ALL TABLES.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION tap_pub2 FOR ALL TABLES;
	INSERT INTO tab1 VALUES(1);
));
$node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub1, tap_pub2"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

$node_publisher->safe_psql('postgres', qq(INSERT INTO tab1 VALUES(2)));
$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_publisher->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1
2),
	"check replication of a table in the EXCEPT TABLE clause of one publication but included by another"
);

$node_subscriber->safe_psql('postgres', 'DROP SUBSCRIPTION tap_sub');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub1');
$node_publisher->safe_psql('postgres', 'DROP PUBLICATION tap_pub2');

$node_publisher->stop('fast');

done_testing();
