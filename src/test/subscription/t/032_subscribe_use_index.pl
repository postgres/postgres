# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test logical replication behavior with subscriber uses available index
use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# create publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->append_conf('postgresql.conf',
	"wal_retrieve_retry_interval = 1ms");
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my $appname           = 'tap_sub';

# ====================================================================
# Testcase start: SUBSCRIPTION USES INDEX
#
# Basic test where the subscriber uses index
# and only updates 1 row and deletes
# 1 other row
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x)");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i FROM generate_series(0,21)i;");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 15;");
$node_publisher->wait_for_catchup($appname);

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 1) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates one row via index";

$node_publisher->safe_psql('postgres',
	"DELETE FROM test_replica_id_full WHERE x = 20;");
$node_publisher->wait_for_catchup($appname);

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 2) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full deletes one row via index";

# make sure that the subscriber has the correct data
my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(DISTINCT x) FROM test_replica_id_full");
is($result, qq(20), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: SUBSCRIPTION USES INDEX
# ====================================================================

# ====================================================================
# Testcase start: SUBSCRIPTION DISABLED INDEX SCAN
#
# Show that enable_index_scan option for CREATE SUBSCRIPTION and
# ALTER SUBSCRIPTION works as intended
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x)");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i FROM generate_series(0,21)i;");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full WITH (enable_index_scan = false)"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 15;");
$node_publisher->wait_for_catchup($appname);

# show that index is not used for UPDATE
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 0) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates one row via index";

$node_publisher->safe_psql('postgres',
	"DELETE FROM test_replica_id_full WHERE x = 20;");
$node_publisher->wait_for_catchup($appname);

# show that index is not used for DELETE
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 0) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full deletes one row via index";

# now, enable the index scan via ALTER SUBSCRIPTION command
# and show that we can control the behavior of using index
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_rep_full SET (enable_index_scan = true)"
);

$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 5;");
$node_publisher->wait_for_catchup($appname);

# show that index is used for UPDATE as we changed enable_index_scan to true
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 1) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates one row via index";


# now, disable the index scan via ALTER SUBSCRIPTION command
# and show that we can control the behavior of using index
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub_rep_full SET (enable_index_scan = false)"
);

$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 8;");
$node_publisher->wait_for_catchup($appname);

# show that index is not used for UPDATE as we changed enable_index_scan to false
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 1) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates one row via index";


# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(DISTINCT x) FROM test_replica_id_full");
is($result, qq(18), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: SUBSCRIPTION DISABLED INDEX SCAN
# ====================================================================


# ====================================================================
# Testcase start: SUBSCRIPTION CREATE/DROP INDEX WORKS WITHOUT ISSUES
#
# This test ensures that after CREATE INDEX, the subscriber can automatically
# use one of the indexes (provided that it fulfils the requirements).
# Similarly, after DROP index, the subscriber can automatically switch to
# sequential scan

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int NOT NULL, y int)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int NOT NULL, y int)");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i, i FROM generate_series(0,2100)i;");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# now, create index and see that the index is used
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x)");

# wait until the index is created
$node_subscriber->poll_query_until(
	'postgres', q{select count(*)=1 from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for creating index test_replica_id_full_idx";

$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 15;");
$node_publisher->wait_for_catchup($appname);


# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 1) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates one row via index";


# now, create index on column y as well
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idy ON test_replica_id_full(y)");

# wait until the index is created
$node_subscriber->poll_query_until(
	'postgres', q{select count(*)=1 from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idy';}
) or die "Timed out while waiting for creating index test_replica_id_full_idy";

# now, the update could either use the test_replica_id_full_idy or test_replica_id_full_idy index
# it is not possible for user to control which index to use
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET y = y + 1 WHERE y = 3000;");
$node_publisher->wait_for_catchup($appname);

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select count(idx_scan) = 2 from pg_stat_all_indexes where indexrelname IN ('test_replica_id_full_idy', 'test_replica_id_full_idx');}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full deletes one row via index";

# let's also test dropping test_replica_id_full_idy and
# hence use test_replica_id_full_idx
$node_subscriber->safe_psql('postgres',
	"DROP INDEX test_replica_id_full_idy;");
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 25;");
$node_publisher->wait_for_catchup($appname);

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM test_replica_id_full WHERE x = 15 OR x = 25 OR y = 3000;");
is($result, qq(0), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: SUBSCRIPTION RE-CALCULATES INDEX AFTER CREATE/DROP INDEX
# ====================================================================

# ====================================================================
# Testcase start: SUBSCRIPTION USES INDEX UPDATEs MULTIPLE ROWS
#
# Basic test where the subscriber uses index
# and updates 50 rows
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x)");

# insert some initial data within the range 0-19
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i%20 FROM generate_series(0,1000)i;"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# updates 50 rows
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 15;");
$node_publisher->wait_for_catchup($appname);

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 50) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates 50 rows via index";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full where x = 15;");
is($result, qq(0), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: SUBSCRIPTION USES INDEX UPDATEs MULTIPLE ROWS
# ====================================================================


# ====================================================================
# Testcase start: SUBSCRIPTION USES INDEX WITH MULTIPLE COLUMNS
#
# Basic test where the subscriber uses index
# and deletes 200 rows
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y text)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y text)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x,y)");

# insert some initial data within the range 0-9
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT (i%10), (i%10)::text FROM generate_series(0,1000)i;"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# deletes 200 rows
$node_publisher->safe_psql('postgres',
	"DELETE FROM test_replica_id_full WHERE x IN (5, 6);");

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 200) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full deletes 200 rows via index";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full where x in (5, 6);");
is($result, qq(0), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: SUBSCRIPTION USES INDEX WITH MULTIPLE COLUMNS
# ====================================================================

# ====================================================================
# Testcase start: SUBSCRIPTION USES INDEX WITH DROPPED COLUMNS
#
# Basic test where the subscriber uses index
# and updates multiple rows with a table that has
# dropped columns
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (drop_1 jsonb, x int, drop_2 point, y text, drop_3 timestamptz)"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full DROP COLUMN drop_1");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full DROP COLUMN drop_2");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full DROP COLUMN drop_3");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (drop_1 jsonb, x int, drop_2 point, y text, drop_3 timestamptz)"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full DROP COLUMN drop_1");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full DROP COLUMN drop_2");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full DROP COLUMN drop_3");

$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x,y)");

# insert some initial data within the range 0-9
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT (i%10), (i%10)::text FROM generate_series(0,1000)i;"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# updates 200 rows
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x IN (5, 6);");

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan = 200) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates 200 rows via index";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select sum(x+y::int) from test_replica_id_full;");
is($result, qq(9200), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: SUBSCRIPTION USES INDEX WITH DROPPED COLUMNS
# ====================================================================

# ====================================================================
# Testcase start: SUBSCRIPTION USES INDEX ON PARTITIONED TABLES

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE users_table_part(user_id bigint, value_1 int, value_2 int) PARTITION BY RANGE (value_1);"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE users_table_part_0 PARTITION OF users_table_part FOR VALUES FROM (0) TO (10);"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE users_table_part_1 PARTITION OF users_table_part FOR VALUES FROM (10) TO (20);"
);

$node_publisher->safe_psql('postgres',
	"ALTER TABLE users_table_part REPLICA IDENTITY FULL;");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE users_table_part_0 REPLICA IDENTITY FULL;");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE users_table_part_1 REPLICA IDENTITY FULL;");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE users_table_part(user_id bigint, value_1 int, value_2 int) PARTITION BY RANGE (value_1);"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE users_table_part_0 PARTITION OF users_table_part FOR VALUES FROM (0) TO (10);"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE users_table_part_1 PARTITION OF users_table_part FOR VALUES FROM (10) TO (20);"
);
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX users_table_part_idx ON users_table_part(user_id, value_1)"
);

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO users_table_part SELECT (i%100), (i%20), i FROM generate_series(0,1000)i;"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE users_table_part");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# updates rows and moves between partitions
$node_publisher->safe_psql('postgres',
	"UPDATE users_table_part SET value_1 = 0 WHERE user_id = 4;");

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select sum(idx_scan)=10 from pg_stat_all_indexes where indexrelname ilike 'users_table_part_%';}
) or die "Timed out while waiting for updates on partitioned table with index";

# deletes rows from different partitions
$node_publisher->safe_psql('postgres',
	"DELETE FROM users_table_part WHERE user_id = 1 and value_1 = 1;");
$node_publisher->safe_psql('postgres',
	"DELETE FROM users_table_part WHERE user_id = 12 and value_1 = 12;");

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select sum(idx_scan)=30 from pg_stat_all_indexes where indexrelname ilike 'users_table_part_%';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates partitioned table";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select sum(user_id+value_1+value_2) from users_table_part;");
is($result, qq(550070), 'ensure subscriber has the correct data at the end of the test');
$result = $node_subscriber->safe_psql('postgres',
	"select count(DISTINCT(user_id,value_1, value_2)) from users_table_part;");
is($result, qq(981), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE users_table_part");

# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE users_table_part");

# Testcase end: SUBSCRIPTION USES INDEX ON PARTITIONED TABLES
# ====================================================================

# ====================================================================
# Testcase start: SUBSCRIPTION DOES NOT USE PARTIAL INDEX

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full_part_index (x int);");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full_part_index REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full_part_index (x int);");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_part_idx ON test_replica_id_full_part_index(x) WHERE (x = 5);");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full_part_index SELECT i FROM generate_series(0,21)i;");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full_part_index");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# update 2 rows, one of them is indexed
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full_part_index SET x = x + 1 WHERE x = 5;");
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full_part_index SET x = x + 1 WHERE x = 15;");
$node_publisher->wait_for_catchup($appname);

# make sure that the index is not used
$result = $node_subscriber->safe_psql('postgres',
	"select idx_scan from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_part_idx'");
is($result, qq(0), 'ensure subscriber tap_sub_rep_full updates one row via seq. scan with with partial index');

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM test_replica_id_full_part_index;");
is($result, qq(22), 'ensure subscriber has the correct data at the end of the test');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(DISTINCT x) FROM test_replica_id_full_part_index;");
is($result, qq(20), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full_part_index");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full_part_index");

# Testcase end: SUBSCRIPTION DOES NOT USE PARTIAL INDEX
# ====================================================================


# ====================================================================
# Testcase start: SUBSCRIPTION DOES NOT USE INDEXES WITH ONLY EXPRESSIONS

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text);");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE people REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text);");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX people_names ON people ((firstname || ' ' || lastname));");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO people SELECT 'first_name_' || i::text, 'last_name_' || i::text FROM generate_series(0,200)i;");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE people");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# update 2 rows
$node_publisher->safe_psql('postgres',
	"UPDATE people SET firstname = 'Nan' WHERE firstname = 'first_name_1';");
$node_publisher->safe_psql('postgres',
	"UPDATE people SET firstname = 'Nan' WHERE firstname = 'first_name_2' AND lastname = 'last_name_2';");

# make sure the index is not used on the subscriber
$result = $node_subscriber->safe_psql('postgres',
	"select idx_scan from pg_stat_all_indexes where indexrelname = 'people_names'");
is($result, qq(0), 'ensure subscriber tap_sub_rep_full updates two rows via seq. scan with index on expressions');

$node_publisher->safe_psql('postgres',
	"DELETE FROM people WHERE firstname = 'first_name_3';");
$node_publisher->safe_psql('postgres',
	"DELETE FROM people WHERE firstname = 'first_name_4' AND lastname = 'last_name_4';");

$node_publisher->wait_for_catchup($appname);

# make sure the index is not used on the subscriber
$result = $node_subscriber->safe_psql('postgres',
	"select idx_scan from pg_stat_all_indexes where indexrelname = 'people_names'");
is($result, qq(0), 'ensure subscriber tap_sub_rep_full updates two rows via seq. scan with index on expressions');

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM people;");
is($result, qq(199), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE people");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE people");

# Testcase end: SUBSCRIPTION DOES NOT USE INDEXES WITH ONLY EXPRESSIONS
# ====================================================================

# ====================================================================
# Testcase start: SUBSCRIPTION CAN USE INDEXES WITH EXPRESSIONS AND COLUMNS

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text);");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE people REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE people (firstname text, lastname text);");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX people_names ON people (firstname, lastname, (firstname || ' ' || lastname));");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO people SELECT 'first_name_' || i::text, 'last_name_' || i::text FROM generate_series(0, 200)i;");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE people");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

# update 2 rows
$node_publisher->safe_psql('postgres',
	"UPDATE people SET firstname = 'Nan' WHERE firstname = 'first_name_1';");
$node_publisher->safe_psql('postgres',
	"UPDATE people SET firstname = 'Nan' WHERE firstname = 'first_name_3' AND lastname = 'last_name_3';");

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select idx_scan=2 from pg_stat_all_indexes where indexrelname = 'people_names';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates two rows via index scan with index on expressions and columns";

$node_publisher->safe_psql('postgres',
	"DELETE FROM people WHERE firstname = 'Nan';");

# wait until the index is used on the subscriber
$node_subscriber->poll_query_until(
	'postgres', q{select idx_scan=4 from pg_stat_all_indexes where indexrelname = 'people_names';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full deletes two rows via index scan with index on expressions and columns";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM people;");
is($result, qq(199), 'ensure subscriber has the correct data at the end of the test');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM people WHERE firstname = 'NaN';");
is($result, qq(0), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE people");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE people");

# Testcase end: SUBSCRIPTION CAN USE INDEXES WITH EXPRESSIONS AND COLUMNS
# ====================================================================

# ====================================================================
# Testcase start: Some NULL values

$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int);"
);

$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y int);"
);

$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x,y);"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full VALUES (1), (2), (3);");
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 1;");
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 1 WHERE x = 3;");

# check if the index is used even when the index has NULL values
$node_subscriber->poll_query_until(
	'postgres', q{select idx_scan=2 from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates test_replica_id_full table";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select sum(x) from test_replica_id_full WHERE y IS NULL;");
is($result, qq(8), 'ensure subscriber has the correct data at the end of the test');

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select count(*) from test_replica_id_full WHERE y IS NULL;");
is($result, qq(3), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: Some NULL values
# ====================================================================

# ====================================================================
# Testcase start: Unique index that is not primary key or replica identity

$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y int);"
);

$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int, y int);"
);

$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX test_replica_id_full_unique_idx ON test_replica_id_full(x);"
);

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full (x, y) VALUES (NULL, 1), (NULL, 2), (NULL, 3);");
$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = 1 WHERE y = 2;");

# check if the index is used even when the index has NULL values
$node_subscriber->poll_query_until(
	'postgres', q{select idx_scan=1 from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_unique_idx';}
) or die "Timed out while waiting for check subscriber tap_sub_rep_full updates test_replica_id_full table";

# make sure that the subscriber has the correct data
$result = $node_subscriber->safe_psql('postgres',
	"select sum(y) from test_replica_id_full;");
is($result, qq(6), 'ensure subscriber has the correct data at the end of the test');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

# Testcase end: Unique index that is not primary key or replica identity
# ====================================================================



# ====================================================================
# Testcase start: SUBSCRIPTION BEHAVIOR WITH ENABLE_INDEXSCAN
#
# Even if enable_indexscan = false, we do use the primary keys, this
# is the legacy behavior. However, we do not use non-primary/non replica
# identity columns.
#

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int NOT NULL)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY FULL;");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE test_replica_id_full (x int NOT NULL)");
$node_subscriber->safe_psql('postgres',
	"CREATE INDEX test_replica_id_full_idx ON test_replica_id_full(x)");
$node_subscriber->safe_psql('postgres',
	"ALTER SYSTEM SET enable_indexscan TO off;");
$node_subscriber->safe_psql('postgres',
	"SELECT pg_reload_conf();");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_replica_id_full SELECT i FROM generate_series(0,21)i;");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_rep_full FOR TABLE test_replica_id_full");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub_rep_full CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_rep_full"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 10000 WHERE x = 15;");
$node_publisher->wait_for_catchup($appname);

# show that index is not used when enable_indexscan=false
$result = $node_subscriber->safe_psql('postgres',
	"select idx_scan from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_idx'");
is($result, qq(0), 'ensure subscriber has not used index with enable_indexscan=false');

# we are done with this index, drop to simplify the tests
$node_subscriber->safe_psql('postgres',
	"DROP INDEX test_replica_id_full_idx");

# now, create a unique index and set the replica
$node_publisher->safe_psql('postgres',
	"CREATE UNIQUE INDEX test_replica_id_full_unique ON test_replica_id_full(x);");
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX test_replica_id_full_unique ON test_replica_id_full(x);");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY USING INDEX test_replica_id_full_unique;");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE test_replica_id_full REPLICA IDENTITY USING INDEX test_replica_id_full_unique;");

# wait for the synchronization to finish
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql('postgres',
	"UPDATE test_replica_id_full SET x = x + 10000 WHERE x = 14;");
$node_publisher->wait_for_catchup($appname);

# show that the unique index on replica identity is used even when enable_indexscan=false
# this is a legacy behavior
$node_subscriber->poll_query_until(
	'postgres', q{select (idx_scan=1) from pg_stat_all_indexes where indexrelname = 'test_replica_id_full_unique'}
) or die "Timed out while waiting ensuring subscriber used unique index as replica identity even with enable_indexscan=false";

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM test_replica_id_full WHERE x IN (14,15)");
is($result, qq(0), 'ensure the results are accurate even with enable_indexscan=false');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_rep_full");
$node_publisher->safe_psql('postgres', "DROP TABLE test_replica_id_full");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub_rep_full");
$node_subscriber->safe_psql('postgres', "DROP TABLE test_replica_id_full");

$node_subscriber->safe_psql('postgres',
	"ALTER SYSTEM RESET enable_indexscan;");
$node_subscriber->safe_psql('postgres',
	"SELECT pg_reload_conf();");

# Testcase end: SUBSCRIPTION BEHAVIOR WITH ENABLE_INDEXSCAN
# ====================================================================

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
