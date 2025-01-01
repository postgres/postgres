# Copyright (c) 2022-2025, PostgreSQL Global Development Group

# Test logical replication behavior with subscriber using available index
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my $appname = 'tap_sub';
my $result = '';

# =============================================================================
# Testcase start: Subscription can use index with multiple rows and columns
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y text)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y text)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x,y)");

# insert some initial data within the range 0-9 for x and y
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT (i%10), (i%10)::text FROM generate_series(0,10) i"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# delete 2 rows
$node_publisher->safe_psql('postgres',
	"DELETE FROM test_replica_id_full WHERE x IN (5, 6)");

# update 2 rows
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = 100, y = '200' WHERE x IN (1, 2)");

# wait until the index is used on the subscriber
$node_publisher->wait_for_catchup($appname);
$node_subscriber->poll_query_until('postgres',
	q{select (idx_scan = 4) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
  )
  or die
  "Timed out while waiting for check subscriber tap_sub_rep_full updates 4 rows via index";

# make sure that the subscriber has the correct data after the UPDATE
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full WHERE (x = 100 and y = '200')"
);
is($result, qq(2),
	'ensure subscriber has the correct data at the end of the test');

# make sure that the subscriber has the correct data after the first DELETE
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full where x in (5, 6)");
is($result, qq(0),
	'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: Subscription can use index with multiple rows and columns
# =============================================================================

# =============================================================================
# Testcase start: Subscription can use index on partitioned tables

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE users_table_part(user_id bigint, value_1 int, value_2 int) PARTITION BY RANGE (value_1)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE users_table_part_0 PARTITION OF users_table_part FOR VALUES FROM (0) TO (10)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE users_table_part_1 PARTITION OF users_table_part FOR VALUES FROM (10) TO (20)"
);

$node_publisher->safe_psql('postgres',
	"ALTER TABLE users_table_part REPLICA IDENTITY FULL");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE users_table_part_0 REPLICA IDENTITY FULL");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE users_table_part_1 REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE users_table_part(user_id bigint, value_1 int, value_2 int) PARTITION BY RANGE (value_1)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE users_table_part_0 PARTITION OF users_table_part FOR VALUES FROM (0) TO (10)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE users_table_part_1 PARTITION OF users_table_part FOR VALUES FROM (10) TO (20)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX users_table_part_idx ON users_table_part(user_id, value_1)"
);

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO users_table_part SELECT (i%100), (i%20), i FROM generate_series(0,100) i"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE users_table_part");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# update rows, moving them to other partitions
$node_publisher->safe_psql('postgres',
	"UPDATE users_table_part SET value_1 = 0 WHERE user_id = 4");

# delete rows from different partitions
$node_publisher->safe_psql('postgres',
	"DELETE FROM users_table_part WHERE user_id = 1 and value_1 = 1");
$node_publisher->safe_psql('postgres',
	"DELETE FROM users_table_part WHERE user_id = 12 and value_1 = 12");

# wait until the index is used on the subscriber
$node_publisher->wait_for_catchup($appname);
$node_subscriber->poll_query_until('postgres',
	q{select sum(idx_scan)=3 from pg_stat_all_indexes where indexrelname ilike 'users_table_part_%';}
  )
  or die
  "Timed out while waiting for check subscriber tap_sub_rep_full updates partitioned table";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select sum(user_id+value_1+value_2) from users_table_part");
is($result, qq(10907),
	'ensure subscriber has the correct data at the end of the test');
$result = $node_subscriber->safe_psql('postgres',
	"select count(DISTINCT(user_id,value_1, value_2)) from users_table_part");
is($result, qq(99),
	'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE users_table_part");

# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE users_table_part");

# Testcase end: Subscription can use index on partitioned tables
# =============================================================================

# =============================================================================
# Testcase start: Subscription will not use indexes with only expressions or
# partial index

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE people REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text)");

# index with only an expression
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX people_names_expr_only ON people ((firstname || ' ' || lastname))"
);

# partial index
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX people_names_partial ON people(firstname) WHERE (firstname = 'first_name_1')"
);

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO people SELECT 'first_name_' || i::text, 'last_name_' || i::text FROM generate_series(0,200) i"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE people");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# update 2 rows
$node_publisher->safe_psql('postgres',
	"UPDATE people SET firstname = 'no-name' WHERE firstname = 'first_name_1'"
);
$node_publisher->safe_psql('postgres',
	"UPDATE people SET firstname = 'no-name' WHERE firstname = 'first_name_2' AND lastname = 'last_name_2'"
);

# make sure none of the indexes is used on the subscriber
$node_publisher->wait_for_catchup($appname);
$result = $node_subscriber->safe_psql('postgres',
	"select sum(idx_scan) from pg_stat_all_indexes where indexrelname IN ('people_names_expr_only', 'people_names_partial')"
);
is($result, qq(0),
	'ensure subscriber tap_sub_rep_full updates two rows via seq. scan with index on expressions'
);

$node_publisher->safe_psql('postgres',
	"DELETE FROM people WHERE firstname = 'first_name_3'");
$node_publisher->safe_psql('postgres',
	"DELETE FROM people WHERE firstname = 'first_name_4' AND lastname = 'last_name_4'"
);

# make sure the index is not used on the subscriber
$node_publisher->wait_for_catchup($appname);
$result = $node_subscriber->safe_psql('postgres',
	"select sum(idx_scan) from pg_stat_all_indexes where indexrelname IN ('people_names_expr_only', 'people_names_partial')"
);
is($result, qq(0),
	'ensure subscriber tap_sub_rep_full updates two rows via seq. scan with index on expressions'
);

# make sure that the subscriber has the correct data
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM people");
is($result, qq(199),
	'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE people");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE people");

# Testcase end: Subscription will not use indexes with only expressions or
# partial index
# =============================================================================

# =============================================================================
# Testcase start: Subscription can use index having expressions and columns

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE people REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX people_names ON people (firstname, lastname, (firstname || ' ' || lastname))"
);

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO people SELECT 'first_name_' || i::text, 'last_name_' || i::text FROM generate_series(0, 20) i"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE people");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# update 1 row
$node_publisher->safe_psql('postgres',
	"UPDATE people SET firstname = 'no-name' WHERE firstname = 'first_name_1'"
);

# delete the updated row
$node_publisher->safe_psql('postgres',
	"DELETE FROM people WHERE firstname = 'no-name'");

# wait until the index is used on the subscriber
$node_publisher->wait_for_catchup($appname);
$node_subscriber->poll_query_until('postgres',
	q{select idx_scan=2 from pg_stat_all_indexes where indexrelname = 'people_names';}
  )
  or die
  "Timed out while waiting for check subscriber tap_sub_rep_full deletes two rows via index scan with index on expressions and columns";

# make sure that the subscriber has the correct data
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM people");
is($result, qq(20),
	'ensure subscriber has the correct data at the end of the test');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM people WHERE firstname = 'no-name'");
is($result, qq(0),
	'ensure subscriber has the correct data at the end of the test');

# now, drop the index with the expression, we'll use sequential scan
$node_subscriber->safe_psql('postgres', "DROP INDEX people_names");

# delete 1 row
$node_publisher->safe_psql('postgres',
	"DELETE FROM people WHERE lastname = 'last_name_18'");

# make sure that the subscriber has the correct data
$node_publisher->wait_for_catchup($appname);
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM people WHERE lastname = 'last_name_18'");
is($result, qq(0),
	'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE people");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE people");

# Testcase end: Subscription can use index having expressions and columns
# =============================================================================

# =============================================================================
# Testcase start: Null values and missing column

$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int)");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y int)");

$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x,y)");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# load some data, and update 2 tuples
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full VALUES (1), (2), (3)");
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 1");

# check if the index is used even when the index has NULL values
$node_publisher->wait_for_catchup($appname);
$node_subscriber->poll_query_until('postgres',
	q{select idx_scan=1 from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
  )
  or die
  "Timed out while waiting for check subscriber tap_sub_rep_full updates test_replica_id_full table";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select sum(x) from test_replica_id_full WHERE y IS NULL");
is($result, qq(7),
	'ensure subscriber has the correct data at the end of the test');

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full WHERE y IS NULL");
is($result, qq(3),
	'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: Null values And missing column
# =============================================================================

# =============================================================================
# Testcase start: Subscription using a unique index when Pub/Sub has different
# data
#
# The subscriber has duplicate tuples that publisher does not have. When
# publisher updates/deletes 1 row, subscriber uses indexes and updates/deletes
# exactly 1 row.
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y int)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y int)");
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX test_replica_id_full_idxy ON test_replica_id_full(x,y)"
);

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i, i FROM generate_series(0,21) i"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# duplicate the data in subscriber for y column
$node_subscriber->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i+100, i FROM generate_series(0,21) i"
);

# now, we update only 1 row on the publisher and expect the subscriber to only
# update 1 row although there are two tuples with y = 15 on the subscriber
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = 2000 WHERE y = 15");

# wait until the index is used on the subscriber
$node_publisher->wait_for_catchup($appname);
$node_subscriber->poll_query_until('postgres',
	q{select (idx_scan = 1) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idxy';}
  )
  or die
  "Timed out while waiting for check subscriber tap_sub_rep_full updates one row via index";

# make sure that the subscriber has the correct data
# we only updated 1 row
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM test_replica_id_full WHERE x = 2000");
is($result, qq(1),
	'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase start: Subscription using a unique index when Pub/Sub has different
# data
# =============================================================================

# =============================================================================
# Testcase start: Subscription can use hash index
#

# create tables on pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y text)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y text)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full USING HASH (x)"
);

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i, (i%10)::text FROM generate_series(0,10) i"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# delete 2 rows
$node_publisher->safe_psql('postgres',
	"DELETE FROM test_replica_id_full WHERE x IN (5, 6)");

# update 2 rows
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = 100, y = '200' WHERE x IN (1, 2)");

# wait until the index is used on the subscriber
$node_publisher->wait_for_catchup($appname);
$node_subscriber->poll_query_until('postgres',
	q{select (idx_scan = 4) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
  )
  or die
  "Timed out while waiting for check subscriber tap_sub_rep_full deletes 2 rows and updates 2 rows via index";

# make sure that the subscriber has the correct data after the UPDATE
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full WHERE (x = 100 and y = '200')"
);
is($result, qq(2),
	'ensure subscriber has the correct data at the end of the test');

# make sure that the subscriber has the correct data after the first DELETE
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full where x in (5, 6)");
is($result, qq(0),
	'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: Subscription can use hash index
# =============================================================================

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
