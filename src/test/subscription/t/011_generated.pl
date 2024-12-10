
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Test generated columns
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# setup

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b int GENERATED ALWAYS AS (a * 2) STORED)"
);

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b int GENERATED ALWAYS AS (a * 22) STORED, c int)"
);

# data for initial sync

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 (a) VALUES (1), (2), (3)");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);

# Wait for initial sync of all subscriptions
$node_subscriber->wait_for_subscription_sync;

my $result = $node_subscriber->safe_psql('postgres', "SELECT a, b FROM tab1");
is( $result, qq(1|22
2|44
3|66), 'generated columns initial sync');

# data to replicate

$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (4), (5)");

$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 5");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres', "SELECT * FROM tab1");
is( $result, qq(1|22|
2|44|
3|66|
4|88|
6|132|), 'generated columns replicated');

# try it with a subscriber-side trigger

$node_subscriber->safe_psql(
	'postgres', q{
CREATE FUNCTION tab1_trigger_func() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
  NEW.c := NEW.a + 10;
  RETURN NEW;
END $$;

CREATE TRIGGER test1 BEFORE INSERT OR UPDATE ON tab1
  FOR EACH ROW
  EXECUTE PROCEDURE tab1_trigger_func();

ALTER TABLE tab1 ENABLE REPLICA TRIGGER test1;
});

$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (7), (8)");

$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 9 WHERE a = 7");

$node_publisher->wait_for_catchup('sub1');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY 1");
is( $result, qq(1|22|
2|44|
3|66|
4|88|
6|132|
8|176|18
9|198|19), 'generated columns replicated with trigger');

# cleanup
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");

# =============================================================================
# Exercise logical replication of a generated column to a subscriber side
# regular column. This is done both when the publication parameter
# 'publish_generated_columns' is set to false (to confirm existing default
# behavior), and is set to true (to confirm replication occurs).
#
# The test environment is set up as follows:
#
# - Publication pub1 on the 'postgres' database.
#   pub1 has publish_generated_columns=false.
#
# - Publication pub2 on the 'postgres' database.
#   pub2 has publish_generated_columns=true.
#
# - Subscription sub1 on the 'postgres' database for publication pub1.
#
# - Subscription sub2 on the 'test_pgc_true' database for publication pub2.
# =============================================================================

$node_subscriber->safe_psql('postgres', "CREATE DATABASE test_pgc_true");

# --------------------------------------------------
# Test Case: Generated to regular column replication
# Publisher table has generated column 'b'.
# Subscriber table has regular column 'b'.
# --------------------------------------------------

# Create table and publications. Insert data to verify initial sync.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_gen_to_nogen (a int, b int GENERATED ALWAYS AS (a * 2) STORED);
	INSERT INTO tab_gen_to_nogen (a) VALUES (1), (2), (3);
	CREATE PUBLICATION regress_pub1_gen_to_nogen FOR TABLE tab_gen_to_nogen WITH (publish_generated_columns = false);
	CREATE PUBLICATION regress_pub2_gen_to_nogen FOR TABLE tab_gen_to_nogen WITH (publish_generated_columns = true);
));

# Create the table and subscription in the 'postgres' database.
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_gen_to_nogen (a int, b int);
	CREATE SUBSCRIPTION regress_sub1_gen_to_nogen CONNECTION '$publisher_connstr' PUBLICATION regress_pub1_gen_to_nogen WITH (copy_data = true);
));

# Create the table and subscription in the 'test_pgc_true' database.
$node_subscriber->safe_psql(
	'test_pgc_true', qq(
	CREATE TABLE tab_gen_to_nogen (a int, b int);
	CREATE SUBSCRIPTION regress_sub2_gen_to_nogen CONNECTION '$publisher_connstr' PUBLICATION regress_pub2_gen_to_nogen WITH (copy_data = true);
));

# Wait for the initial synchronization of both subscriptions.
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'regress_sub1_gen_to_nogen', 'postgres');
$node_subscriber->wait_for_subscription_sync($node_publisher,
	'regress_sub2_gen_to_nogen', 'test_pgc_true');

# Verify that generated column data is not copied during the initial
# synchronization when publish_generated_columns is set to false.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_gen_to_nogen ORDER BY a");
is( $result, qq(1|
2|
3|), 'tab_gen_to_nogen initial sync, when publish_generated_columns=false');

# Verify that generated column data is copied during the initial synchronization
# when publish_generated_columns is set to true.
$result = $node_subscriber->safe_psql('test_pgc_true',
	"SELECT a, b FROM tab_gen_to_nogen ORDER BY a");
is( $result, qq(1|2
2|4
3|6),
	'tab_gen_to_nogen initial sync, when publish_generated_columns=true');

# Insert data to verify incremental replication.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_gen_to_nogen VALUES (4), (5)");

# Verify that the generated column data is not replicated during incremental
# replication when publish_generated_columns is set to false.
$node_publisher->wait_for_catchup('regress_sub1_gen_to_nogen');
$result = $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_gen_to_nogen ORDER BY a");
is( $result, qq(1|
2|
3|
4|
5|),
	'tab_gen_to_nogen incremental replication, when publish_generated_columns=false'
);

# Verify that generated column data is replicated during incremental
# synchronization when publish_generated_columns is set to true.
$node_publisher->wait_for_catchup('regress_sub2_gen_to_nogen');
$result = $node_subscriber->safe_psql('test_pgc_true',
	"SELECT a, b FROM tab_gen_to_nogen ORDER BY a");
is( $result, qq(1|2
2|4
3|6
4|8
5|10),
	'tab_gen_to_nogen incremental replication, when publish_generated_columns=true'
);

# cleanup
$node_subscriber->safe_psql('postgres',
	"DROP SUBSCRIPTION regress_sub1_gen_to_nogen");
$node_subscriber->safe_psql('test_pgc_true',
	"DROP SUBSCRIPTION regress_sub2_gen_to_nogen");
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION regress_pub1_gen_to_nogen;
	DROP PUBLICATION regress_pub2_gen_to_nogen;
));
$node_subscriber->safe_psql('test_pgc_true', "DROP table tab_gen_to_nogen");
$node_subscriber->safe_psql('postgres', "DROP DATABASE test_pgc_true");

# =============================================================================
# The following test cases demonstrate how publication column lists interact
# with the publication parameter 'publish_generated_columns'.
#
# Test: Column lists take precedence, so generated columns in a column list
# will be replicated even when publish_generated_columns=false.
#
# Test: When there is a column list, only those generated columns named in the
# column list will be replicated even when publish_generated_columns=true.
# =============================================================================

# --------------------------------------------------
# Test Case: Publisher replicates the column list, including generated columns,
# even when the publish_generated_columns option is set to false.
# --------------------------------------------------

# Create table and publication. Insert data to verify initial sync.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab2 (a int, gen1 int GENERATED ALWAYS AS (a * 2) STORED);
	INSERT INTO tab2 (a) VALUES (1), (2);
	CREATE PUBLICATION pub1 FOR table tab2(gen1) WITH (publish_generated_columns=false);
));

# Create table and subscription.
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab2 (a int, gen1 int);
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1 WITH (copy_data = true);
));

# Wait for initial sync.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub1');

# Initial sync test when publish_generated_columns=false.
# Verify 'gen1' is replicated regardless of the false parameter value.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab2 ORDER BY gen1");
is( $result, qq(|2
|4),
	'tab2 initial sync, when publish_generated_columns=false');

# Insert data to verify incremental replication.
$node_publisher->safe_psql('postgres', "INSERT INTO tab2 VALUES (3), (4)");

# Incremental replication test when publish_generated_columns=false.
# Verify 'gen1' is replicated regardless of the false parameter value.
$node_publisher->wait_for_catchup('sub1');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab2 ORDER BY gen1");
is( $result, qq(|2
|4
|6
|8),
	'tab2 incremental replication, when publish_generated_columns=false');

# cleanup
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");

# --------------------------------------------------
# Test Case: Even when publish_generated_columns is set to true, the publisher
# only publishes the data of columns specified in the column list,
# skipping other generated and non-generated columns.
# --------------------------------------------------

# Create table and publication. Insert data to verify initial sync.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab3 (a int, gen1 int GENERATED ALWAYS AS (a * 2) STORED, gen2 int GENERATED ALWAYS AS (a * 2) STORED);
	INSERT INTO tab3 (a) VALUES (1), (2);
	CREATE PUBLICATION pub1 FOR table tab3(gen1) WITH (publish_generated_columns=true);
));

# Create table and subscription.
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab3 (a int, gen1 int, gen2 int);
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1 WITH (copy_data = true);
));

# Wait for initial sync.
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub1');

# Initial sync test when publish_generated_columns=true.
# Verify only 'gen1' is replicated regardless of the true parameter value.
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab3 ORDER BY gen1");
is( $result, qq(|2|
|4|),
	'tab3 initial sync, when publish_generated_columns=true');

# Insert data to verify incremental replication.
$node_publisher->safe_psql('postgres', "INSERT INTO tab3 VALUES (3), (4)");

# Incremental replication test when publish_generated_columns=true.
# Verify only 'gen1' is replicated regardless of the true parameter value.
$node_publisher->wait_for_catchup('sub1');
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab3 ORDER BY gen1");
is( $result, qq(|2|
|4|
|6|
|8|),
	'tab3 incremental replication, when publish_generated_columns=true');

# cleanup
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");

# =============================================================================
# The following test verifies the expected error when replicating to a
# generated subscriber column. Test the following combinations:
# - regular -> generated
# - generated -> generated
# =============================================================================

# --------------------------------------------------
# A "regular -> generated" or "generated -> generated" replication fails,
# reporting an error that the generated column on the subscriber side cannot
# be replicated.
#
# Test Case: regular -> generated and generated -> generated
# Publisher table has regular column 'c2' and generated column 'c3'.
# Subscriber table has generated columns 'c2' and 'c3'.
# --------------------------------------------------

# Create table and publication. Insert data into the table.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE t1(c1 int, c2 int, c3 int GENERATED ALWAYS AS (c1 * 2) STORED);
	CREATE PUBLICATION pub1 for table t1(c1, c2, c3);
	INSERT INTO t1 VALUES (1);
));

# Create table and subscription.
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE t1(c1 int, c2 int GENERATED ALWAYS AS (c1 + 2) STORED, c3 int GENERATED ALWAYS AS (c1 + 2) STORED);
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1;
));

# Verify that an error occurs.
my $offset = -s $node_subscriber->logfile;
$node_subscriber->wait_for_log(
	qr/ERROR: ( [A-Z0-9]+:)? logical replication target relation "public.t1" has incompatible generated columns: "c2", "c3"/,
	$offset);

# cleanup
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");

done_testing();
