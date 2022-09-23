# Copyright (c) 2021-2022, PostgreSQL Global Development Group

# Test logical replication behavior with row filtering
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
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my $appname           = 'tap_sub';

# ====================================================================
# Testcase start: FOR ALL TABLES
#
# The FOR ALL TABLES test must come first so that it is not affected by
# all the other test tables that are later created.

# create tables pub and sub
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rf_x (x int primary key)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rf_x (x int primary key)");

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rf_x (x) VALUES (0), (5), (10), (15), (20)");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_x FOR TABLE tab_rf_x WHERE (x > 10)");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_forall FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_x, tap_pub_forall"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# The subscription of the FOR ALL TABLES publication means there should be no
# filtering on the tablesync COPY, so all expect all 5 will be present.
my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(x) FROM tab_rf_x");
is($result, qq(5),
	'check initial data copy from table tab_rf_x should not be filtered');

# Similarly, the table filter for tab_rf_x (after the initial phase) has no
# effect when combined with the ALL TABLES.
# Expected: 5 initial rows + 2 new rows = 7 rows
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rf_x (x) VALUES (-99), (99)");
$node_publisher->wait_for_catchup($appname);
$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(x) FROM tab_rf_x");
is($result, qq(7), 'check table tab_rf_x should not be filtered');

# cleanup pub
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_forall");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_x");
$node_publisher->safe_psql('postgres', "DROP TABLE tab_rf_x");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");
$node_subscriber->safe_psql('postgres', "DROP TABLE tab_rf_x");

# Testcase end: FOR ALL TABLES
# ====================================================================

# ====================================================================
# Testcase start: TABLES IN SCHEMA
#
# The TABLES IN SCHEMA test is independent of all other test cases so it
# cleans up after itself.

# create tables pub and sub
$node_publisher->safe_psql('postgres', "CREATE SCHEMA schema_rf_x");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE schema_rf_x.tab_rf_x (x int primary key)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE schema_rf_x.tab_rf_partitioned (x int primary key) PARTITION BY RANGE(x)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE public.tab_rf_partition (LIKE schema_rf_x.tab_rf_partitioned)"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE schema_rf_x.tab_rf_partitioned ATTACH PARTITION public.tab_rf_partition DEFAULT"
);
$node_subscriber->safe_psql('postgres', "CREATE SCHEMA schema_rf_x");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE schema_rf_x.tab_rf_x (x int primary key)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE schema_rf_x.tab_rf_partitioned (x int primary key) PARTITION BY RANGE(x)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE public.tab_rf_partition (LIKE schema_rf_x.tab_rf_partitioned)"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE schema_rf_x.tab_rf_partitioned ATTACH PARTITION public.tab_rf_partition DEFAULT"
);

# insert some initial data
$node_publisher->safe_psql('postgres',
	"INSERT INTO schema_rf_x.tab_rf_x (x) VALUES (0), (5), (10), (15), (20)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO schema_rf_x.tab_rf_partitioned (x) VALUES (1), (20)");

# create pub/sub
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_x FOR TABLE schema_rf_x.tab_rf_x WHERE (x > 10)"
);
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_allinschema FOR TABLES IN SCHEMA schema_rf_x, TABLE schema_rf_x.tab_rf_x WHERE (x > 10)"
);
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_allinschema ADD TABLE public.tab_rf_partition WHERE (x > 10)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_x, tap_pub_allinschema"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# The subscription of the TABLES IN SCHEMA publication means there should be
# no filtering on the tablesync COPY, so expect all 5 will be present.
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(x) FROM schema_rf_x.tab_rf_x");
is($result, qq(5),
	'check initial data copy from table tab_rf_x should not be filtered');

# Similarly, the table filter for tab_rf_x (after the initial phase) has no
# effect when combined with the TABLES IN SCHEMA. Meanwhile, the filter for
# the tab_rf_partition does work because that partition belongs to a different
# schema (and publish_via_partition_root = false).
# Expected:
#     tab_rf_x                       :  5 initial rows + 2 new rows = 7 rows
#     tab_rf_partition               :  1 initial row  + 1 new row  = 2 rows
$node_publisher->safe_psql('postgres',
	"INSERT INTO schema_rf_x.tab_rf_x (x) VALUES (-99), (99)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO schema_rf_x.tab_rf_partitioned (x) VALUES (5), (25)");
$node_publisher->wait_for_catchup($appname);
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(x) FROM schema_rf_x.tab_rf_x");
is($result, qq(7), 'check table tab_rf_x should not be filtered');
$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM public.tab_rf_partition");
is( $result, qq(20
25), 'check table tab_rf_partition should be filtered');

# cleanup pub
$node_publisher->safe_psql('postgres',
	"DROP PUBLICATION tap_pub_allinschema");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION tap_pub_x");
$node_publisher->safe_psql('postgres', "DROP TABLE public.tab_rf_partition");
$node_publisher->safe_psql('postgres',
	"DROP TABLE schema_rf_x.tab_rf_partitioned");
$node_publisher->safe_psql('postgres', "DROP TABLE schema_rf_x.tab_rf_x");
$node_publisher->safe_psql('postgres', "DROP SCHEMA schema_rf_x");
# cleanup sub
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION tap_sub");
$node_subscriber->safe_psql('postgres', "DROP TABLE public.tab_rf_partition");
$node_subscriber->safe_psql('postgres',
	"DROP TABLE schema_rf_x.tab_rf_partitioned");
$node_subscriber->safe_psql('postgres', "DROP TABLE schema_rf_x.tab_rf_x");
$node_subscriber->safe_psql('postgres', "DROP SCHEMA schema_rf_x");

# Testcase end: TABLES IN SCHEMA
# ====================================================================

# ======================================================
# Testcase start: FOR TABLE with row filter publications

# setup structure on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_1 (a int primary key, b text)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_1 REPLICA IDENTITY FULL;");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_2 (c int primary key)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_3 (a int primary key, b boolean)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_4 (c int primary key)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_partitioned (a int primary key, b integer) PARTITION BY RANGE(a)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_less_10k (LIKE tab_rowfilter_partitioned)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_partitioned ATTACH PARTITION tab_rowfilter_less_10k FOR VALUES FROM (MINVALUE) TO (10000)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_greater_10k (LIKE tab_rowfilter_partitioned)"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_partitioned ATTACH PARTITION tab_rowfilter_greater_10k FOR VALUES FROM (10000) TO (MAXVALUE)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_partitioned_2 (a int primary key, b integer) PARTITION BY RANGE(a)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_partition (LIKE tab_rowfilter_partitioned_2)"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_partitioned_2 ATTACH PARTITION tab_rowfilter_partition DEFAULT"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_toast (a text NOT NULL, b text NOT NULL)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_toast ALTER COLUMN a SET STORAGE EXTERNAL");
$node_publisher->safe_psql('postgres',
	"CREATE UNIQUE INDEX tab_rowfilter_toast_ri_index on tab_rowfilter_toast (a, b)"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_toast REPLICA IDENTITY USING INDEX tab_rowfilter_toast_ri_index"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_inherited (a int)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_child (b text) INHERITS (tab_rowfilter_inherited)"
);
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_viaroot_part (a int) PARTITION BY RANGE (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_viaroot_part_1 PARTITION OF tab_rowfilter_viaroot_part FOR VALUES FROM (1) TO (20)"
);

# setup structure on subscriber
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_1 (a int primary key, b text)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_2 (c int primary key)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_3 (a int primary key, b boolean)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_4 (c int primary key)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_partitioned (a int primary key, b integer) PARTITION BY RANGE(a)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_less_10k (LIKE tab_rowfilter_partitioned)");
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_partitioned ATTACH PARTITION tab_rowfilter_less_10k FOR VALUES FROM (MINVALUE) TO (10000)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_greater_10k (LIKE tab_rowfilter_partitioned)"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_partitioned ATTACH PARTITION tab_rowfilter_greater_10k FOR VALUES FROM (10000) TO (MAXVALUE)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_partitioned_2 (a int primary key, b integer) PARTITION BY RANGE(a)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_partition (LIKE tab_rowfilter_partitioned_2)"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_partitioned_2 ATTACH PARTITION tab_rowfilter_partition DEFAULT"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_toast (a text NOT NULL, b text NOT NULL)");
$node_subscriber->safe_psql('postgres',
	"CREATE UNIQUE INDEX tab_rowfilter_toast_ri_index on tab_rowfilter_toast (a, b)"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE tab_rowfilter_toast REPLICA IDENTITY USING INDEX tab_rowfilter_toast_ri_index"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_inherited (a int)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_child (b text) INHERITS (tab_rowfilter_inherited)"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_viaroot_part (a int)");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_rowfilter_viaroot_part_1 (a int)");

# setup logical replication
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_1 FOR TABLE tab_rowfilter_1 WHERE (a > 1000 AND b <> 'filtered')"
);

$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_1 ADD TABLE tab_rowfilter_2 WHERE (c % 7 = 0)"
);

$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_1 SET TABLE tab_rowfilter_1 WHERE (a > 1000 AND b <> 'filtered'), tab_rowfilter_2 WHERE (c % 2 = 0), tab_rowfilter_3"
);

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_2 FOR TABLE tab_rowfilter_2 WHERE (c % 3 = 0)"
);

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_3 FOR TABLE tab_rowfilter_partitioned");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_3 ADD TABLE tab_rowfilter_less_10k WHERE (a < 6000)"
);
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_not_used FOR TABLE tab_rowfilter_1 WHERE (a < 0)"
);

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_4a FOR TABLE tab_rowfilter_4 WHERE (c % 2 = 0)"
);
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_4b FOR TABLE tab_rowfilter_4");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_5a FOR TABLE tab_rowfilter_partitioned_2");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_5b FOR TABLE tab_rowfilter_partition WHERE (a > 10)"
);

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_toast FOR TABLE tab_rowfilter_toast WHERE (a = repeat('1234567890', 200) AND b < '10')"
);

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_inherits FOR TABLE tab_rowfilter_inherited WHERE (a > 15)"
);

# two publications, each publishing the partition through a different ancestor, with
# different row filters
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_viaroot_1 FOR TABLE tab_rowfilter_viaroot_part WHERE (a > 15) WITH (publish_via_partition_root)"
);
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub_viaroot_2 FOR TABLE tab_rowfilter_viaroot_part_1 WHERE (a < 15) WITH (publish_via_partition_root)"
);

#
# The following INSERTs are executed before the CREATE SUBSCRIPTION, so these
# SQL commands are for testing the initial data copy using logical replication.
#
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1, 'not replicated')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1500, 'filtered')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1980, 'not filtered')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) SELECT x, 'test ' || x FROM generate_series(990,1002) x"
);
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_2 (c) SELECT generate_series(1, 20)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_3 (a, b) SELECT x, (x % 3 = 0) FROM generate_series(1, 10) x"
);
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_4 (c) SELECT generate_series(1, 10)");

# insert data into partitioned table and directly on the partition
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_partitioned (a, b) VALUES(1, 100),(7000, 101),(15000, 102),(5500, 300)"
);
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_less_10k (a, b) VALUES(2, 200),(6005, 201)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_greater_10k (a, b) VALUES(16000, 103)");

# insert data into partitioned table.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_partitioned_2 (a, b) VALUES(1, 1),(20, 20)");

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_toast(a, b) VALUES(repeat('1234567890', 200), '1234567890')"
);

# insert data into parent and child table.
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_inherited(a) VALUES(10),(20)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_child(a, b) VALUES(0,'0'),(30,'30'),(40,'40')"
);

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub_1, tap_pub_2, tap_pub_3, tap_pub_4a, tap_pub_4b, tap_pub_5a, tap_pub_5b, tap_pub_toast, tap_pub_inherits, tap_pub_viaroot_2, tap_pub_viaroot_1"
);

# wait for initial table synchronization to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, $appname);

# Check expected replicated rows for tab_rowfilter_1
# tap_pub_1 filter is: (a > 1000 AND b <> 'filtered')
# - INSERT (1, 'not replicated')   NO, because a is not > 1000
# - INSERT (1500, 'filtered')      NO, because b == 'filtered'
# - INSERT (1980, 'not filtered')  YES
# - generate_series(990,1002)      YES, only for 1001,1002 because a > 1000
#
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_rowfilter_1 ORDER BY 1, 2");
is( $result, qq(1001|test 1001
1002|test 1002
1980|not filtered), 'check initial data copy from table tab_rowfilter_1');

# Check expected replicated rows for tab_rowfilter_2
# tap_pub_1 filter is: (c % 2 = 0)
# tap_pub_2 filter is: (c % 3 = 0)
# When there are multiple publications for the same table, the filters
# expressions are OR'ed together. In this case, rows are replicated if
# c value is divided by 2 OR 3 (2, 3, 4, 6, 8, 9, 10, 12, 14, 15, 16, 18, 20)
#
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(c), min(c), max(c) FROM tab_rowfilter_2");
is($result, qq(13|2|20),
	'check initial data copy from table tab_rowfilter_2');

# Check expected replicated rows for tab_rowfilter_4
# (same table in two publications but only one has a filter).
# tap_pub_4a filter is: (c % 2 = 0)
# tap_pub_4b filter is: <no filter>
# Expressions are OR'ed together but when there is no filter it just means
# OR everything - e.g. same as no filter at all.
# Expect all rows: (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(c), min(c), max(c) FROM tab_rowfilter_4");
is($result, qq(10|1|10),
	'check initial data copy from table tab_rowfilter_4');

# Check expected replicated rows for tab_rowfilter_3
# There is no filter. 10 rows are inserted, so 10 rows are replicated.
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(a) FROM tab_rowfilter_3");
is($result, qq(10), 'check initial data copy from table tab_rowfilter_3');

# Check expected replicated rows for partitions
# publication option publish_via_partition_root is false so use the row filter
# from a partition
# tab_rowfilter_partitioned filter: (a < 5000)
# tab_rowfilter_less_10k filter:    (a < 6000)
# tab_rowfilter_greater_10k filter: no filter
#
# INSERT into tab_rowfilter_partitioned:
# - INSERT (1,100)       YES, because 1 < 6000
# - INSERT (7000, 101)   NO,  because 7000 is not < 6000
# - INSERT (15000, 102)  YES, because tab_rowfilter_greater_10k has no filter
# - INSERT (5500, 300)   YES, because 5500 < 6000
#
# INSERT directly into tab_rowfilter_less_10k:
# - INSERT (2, 200)      YES, because 2 < 6000
# - INSERT (6005, 201)   NO, because 6005 is not < 6000
#
# INSERT directly into tab_rowfilter_greater_10k:
# - INSERT (16000, 103)  YES, because tab_rowfilter_greater_10k has no filter
#
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_rowfilter_less_10k ORDER BY 1, 2");
is( $result, qq(1|100
2|200
5500|300), 'check initial data copy from partition tab_rowfilter_less_10k');

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_rowfilter_greater_10k ORDER BY 1, 2");
is( $result, qq(15000|102
16000|103), 'check initial data copy from partition tab_rowfilter_greater_10k'
);

# Check expected replicated rows for partitions
# publication option publish_via_partition_root is false so use the row filter
# from a partition
# tap_pub_5a filter: <no filter>
# tap_pub_5b filter: (a > 10)
# The parent table for this partition is published via tap_pub_5a, so there is
# no filter for the partition. And expressions are OR'ed together so it means
# OR everything - e.g. same as no filter at all.
# Expect all rows: (1, 1) and (20, 20)
#
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_rowfilter_partition ORDER BY 1, 2");
is( $result, qq(1|1
20|20), 'check initial data copy from partition tab_rowfilter_partition');

# Check expected replicated rows for tab_rowfilter_toast
# tab_rowfilter_toast filter: (a = repeat('1234567890', 200) AND b < '10')
# INSERT (repeat('1234567890', 200) ,'1234567890') NO
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(*) FROM tab_rowfilter_toast");
is($result, qq(0), 'check initial data copy from table tab_rowfilter_toast');

# Check expected replicated rows for tab_rowfilter_inherited
# tab_rowfilter_inherited filter is: (a > 15)
# - INSERT (10)        NO, 10 < 15
# - INSERT (20)        YES, 20 > 15
# - INSERT (0, '0')     NO, 0 < 15
# - INSERT (30, '30')   YES, 30 > 15
# - INSERT (40, '40')   YES, 40 > 15
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a FROM tab_rowfilter_inherited ORDER BY a");
is( $result, qq(20
30
40), 'check initial data copy from table tab_rowfilter_inherited');

# The following commands are executed after CREATE SUBSCRIPTION, so these SQL
# commands are for testing normal logical replication behavior.
#
# test row filter (INSERT, UPDATE, DELETE)
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (800, 'test 800')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1600, 'test 1600')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1601, 'test 1601')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1602, 'filtered')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_1 (a, b) VALUES (1700, 'test 1700')");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_rowfilter_1 SET b = NULL WHERE a = 1600");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_rowfilter_1 SET b = 'test 1601 updated' WHERE a = 1601");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_rowfilter_1 SET b = 'test 1602 updated' WHERE a = 1602");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab_rowfilter_1 WHERE a = 1700");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_2 (c) VALUES (21), (22), (23), (24), (25)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_4 (c) VALUES (0), (11), (12)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_inherited (a) VALUES (14), (16)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_child (a, b) VALUES (13, '13'), (17, '17')");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_viaroot_part (a) VALUES (14), (15), (16)");

$node_publisher->wait_for_catchup($appname);

# Check expected replicated rows for tab_rowfilter_2
# tap_pub_1 filter is: (c % 2 = 0)
# tap_pub_2 filter is: (c % 3 = 0)
# When there are multiple publications for the same table, the filters
# expressions are OR'ed together. In this case, rows are replicated if
# c value is divided by 2 OR 3.
#
# Expect original rows (2, 3, 4, 6, 8, 9, 10, 12, 14, 15, 16, 18, 20)
# Plus (21, 22, 24)
#
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(c), min(c), max(c) FROM tab_rowfilter_2");
is($result, qq(16|2|24), 'check replicated rows to tab_rowfilter_2');

# Check expected replicated rows for tab_rowfilter_4
# (same table in two publications but only one has a filter).
# tap_pub_4a filter is: (c % 2 = 0)
# tap_pub_4b filter is: <no filter>
# Expressions are OR'ed together but when there is no filter it just means
# OR everything - e.g. same as no filter at all.
# Expect all rows from initial copy: (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)
# And also (0, 11, 12)
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT count(c), min(c), max(c) FROM tab_rowfilter_4");
is($result, qq(13|0|12), 'check replicated rows to tab_rowfilter_4');

# Check expected replicated rows for tab_rowfilter_1
# tap_pub_1 filter is: (a > 1000 AND b <> 'filtered')
#
# - 1001, 1002, 1980 already exist from initial data copy
# - INSERT (800, 'test 800')   NO, because 800 is not > 1000
# - INSERT (1600, 'test 1600') YES, because 1600 > 1000 and 'test 1600' <> 'filtered',
#								    but row deleted after the update below.
# - INSERT (1601, 'test 1601') YES, because 1601 > 1000 and 'test 1601' <> 'filtered'
# - INSERT (1602, 'filtered') NO, because b == 'filtered'
# - INSERT (1700, 'test 1700') YES, because 1700 > 1000 and 'test 1700' <> 'filtered'
# - UPDATE (1600, NULL)        NO, row filter evaluates to false because NULL is not <> 'filtered'
# - UPDATE (1601, 'test 1601 updated') YES, because 1601 > 1000 and 'test 1601 updated' <> 'filtered'
# - UPDATE (1602, 'test 1602 updated') YES, because 1602 > 1000 and 'test 1602 updated' <> 'filtered'
# - DELETE (1700)              YES, because 1700 > 1000 and 'test 1700' <> 'filtered'
#
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_rowfilter_1 ORDER BY 1, 2");
is( $result, qq(1001|test 1001
1002|test 1002
1601|test 1601 updated
1602|test 1602 updated
1980|not filtered), 'check replicated rows to table tab_rowfilter_1');

# Publish using root partitioned table
# Use a different partitioned table layout (exercise publish_via_partition_root)
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_3 SET (publish_via_partition_root = true)");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION tap_pub_3 SET TABLE tab_rowfilter_partitioned WHERE (a < 5000), tab_rowfilter_less_10k WHERE (a < 6000)"
);
$node_subscriber->safe_psql('postgres',
	"TRUNCATE TABLE tab_rowfilter_partitioned");
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION WITH (copy_data = true)");

# wait for table synchronization to finish
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_partitioned (a, b) VALUES(4000, 400),(4001, 401),(4002, 402)"
);
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_less_10k (a, b) VALUES(4500, 450)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_less_10k (a, b) VALUES(5600, 123)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowfilter_greater_10k (a, b) VALUES(14000, 1950)");
$node_publisher->safe_psql('postgres',
	"UPDATE tab_rowfilter_less_10k SET b = 30 WHERE a = 4001");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab_rowfilter_less_10k WHERE a = 4002");

$node_publisher->wait_for_catchup($appname);

# Check expected replicated rows for partitions
# publication option publish_via_partition_root is true so use the row filter
# from the root partitioned table
# tab_rowfilter_partitioned filter: (a < 5000)
# tab_rowfilter_less_10k filter:    (a < 6000)
# tab_rowfilter_greater_10k filter: no filter
#
# After TRUNCATE, REFRESH PUBLICATION, the initial data copy will apply the
# partitioned table row filter.
# - INSERT (1, 100)      YES, 1 < 5000
# - INSERT (7000, 101)   NO, 7000 is not < 5000
# - INSERT (15000, 102)  NO, 15000 is not < 5000
# - INSERT (5500, 300)   NO, 5500 is not < 5000
# - INSERT (2, 200)      YES, 2 < 5000
# - INSERT (6005, 201)   NO, 6005 is not < 5000
# - INSERT (16000, 103)  NO, 16000 is not < 5000
#
# Execute SQL commands after initial data copy for testing the logical
# replication behavior.
# - INSERT (4000, 400)    YES, 4000 < 5000
# - INSERT (4001, 401)    YES, 4001 < 5000
# - INSERT (4002, 402)    YES, 4002 < 5000
# - INSERT (4500, 450)    YES, 4500 < 5000
# - INSERT (5600, 123)    NO, 5600 is not < 5000
# - INSERT (14000, 1950)  NO, 16000 is not < 5000
# - UPDATE (4001)         YES, 4001 < 5000
# - DELETE (4002)         YES, 4002 < 5000
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a, b FROM tab_rowfilter_partitioned ORDER BY 1, 2");
is( $result, qq(1|100
2|200
4000|400
4001|30
4500|450), 'check publish_via_partition_root behavior');

# Check expected replicated rows for tab_rowfilter_inherited and
# tab_rowfilter_child.
# tab_rowfilter_inherited filter is: (a > 15)
# - INSERT (14)        NO, 14 < 15
# - INSERT (16)        YES, 16 > 15
#
# tab_rowfilter_child filter is: (a > 15)
# - INSERT (13, '13')   NO, 13 < 15
# - INSERT (17, '17')   YES, 17 > 15

$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a FROM tab_rowfilter_inherited ORDER BY a");
is( $result, qq(16
17
20
30
40),
	'check replicated rows to tab_rowfilter_inherited and tab_rowfilter_child'
);

# UPDATE the non-toasted column for table tab_rowfilter_toast
$node_publisher->safe_psql('postgres',
	"UPDATE tab_rowfilter_toast SET b = '1'");

$node_publisher->wait_for_catchup($appname);

# Check expected replicated rows for tab_rowfilter_toast
# tab_rowfilter_toast filter: (a = repeat('1234567890', 200) AND b < '10')
# UPDATE old  (repeat('1234567890', 200) ,'1234567890')  NO
#        new: (repeat('1234567890', 200) ,'1')           YES
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a = repeat('1234567890', 200), b FROM tab_rowfilter_toast");
is($result, qq(t|1), 'check replicated rows to tab_rowfilter_toast');

# Check expected replicated rows for tab_rowfilter_viaroot_part and
# tab_rowfilter_viaroot_part_1. We should replicate only rows matching
# the row filter for the top-level ancestor:
#
# tab_rowfilter_viaroot_part filter is: (a > 15)
# - INSERT (14)        NO, 14 < 15
# - INSERT (15)        NO, 15 = 15
# - INSERT (16)        YES, 16 > 15
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a FROM tab_rowfilter_viaroot_part");
is($result, qq(16), 'check replicated rows to tab_rowfilter_viaroot_part');

# Check there is no data in tab_rowfilter_viaroot_part_1 because rows are
# replicated via the top most parent table tab_rowfilter_viaroot_part
$result =
  $node_subscriber->safe_psql('postgres',
	"SELECT a FROM tab_rowfilter_viaroot_part_1");
is($result, qq(), 'check replicated rows to tab_rowfilter_viaroot_part_1');

# Testcase end: FOR TABLE with row filter publications
# ======================================================

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
