# Copyright (c) 2022-2025, PostgreSQL Global Development Group

# Test partial-column publication of tables
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
$node_subscriber->append_conf('postgresql.conf',
	qq(max_logical_replication_workers = 6));
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my $offset = 0;

# setup tables on both nodes

# tab1: simple 1:1 replication
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 (a int PRIMARY KEY, "B" int, c int)
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab1 (a int PRIMARY KEY, "B" int, c int)
));

# tab2: replication from regular to table with fewer columns
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab2 (a int PRIMARY KEY, b varchar, c int);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab2 (a int PRIMARY KEY, b varchar)
));

# tab3: simple 1:1 replication with weird column names
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab3 ("a'" int PRIMARY KEY, "B" varchar, "c'" int)
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab3 ("a'" int PRIMARY KEY, "c'" int)
));

# test_part: partitioned tables, with partitioning (including multi-level
# partitioning, and fewer columns on the subscriber)
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part (a int PRIMARY KEY, b text, c timestamptz) PARTITION BY LIST (a);
	CREATE TABLE test_part_1_1 PARTITION OF test_part FOR VALUES IN (1,2,3,4,5,6);
	CREATE TABLE test_part_2_1 PARTITION OF test_part FOR VALUES IN (7,8,9,10,11,12) PARTITION BY LIST (a);
	CREATE TABLE test_part_2_2 PARTITION OF test_part_2_1 FOR VALUES IN (7,8,9,10);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part (a int PRIMARY KEY, b text) PARTITION BY LIST (a);
	CREATE TABLE test_part_1_1 PARTITION OF test_part FOR VALUES IN (1,2,3,4,5,6);
	CREATE TABLE test_part_2_1 PARTITION OF test_part FOR VALUES IN (7,8,9,10,11,12) PARTITION BY LIST (a);
	CREATE TABLE test_part_2_2 PARTITION OF test_part_2_1 FOR VALUES IN (7,8,9,10);
));

# tab4: table with user-defined enum types
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TYPE test_typ AS ENUM ('blue', 'red');
	CREATE TABLE tab4 (a INT PRIMARY KEY, b test_typ, c int, d text);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TYPE test_typ AS ENUM ('blue', 'red');
	CREATE TABLE tab4 (a INT PRIMARY KEY, b test_typ, d text);
));


# TEST: create publication and subscription for some of the tables with
# column lists
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION pub1
	   FOR TABLE tab1 (a, "B"), tab3 ("a'", "c'"), test_part (a, b), tab4 (a, b, d)
	  WITH (publish_via_partition_root = 'true');
));

# check that we got the right prattrs values for the publication in the
# pg_publication_rel catalog (order by relname, to get stable ordering)
my $result = $node_publisher->safe_psql(
	'postgres', qq(
	SELECT relname, prattrs
	FROM pg_publication_rel pb JOIN pg_class pc ON(pb.prrelid = pc.oid)
	ORDER BY relname
));

is( $result, qq(tab1|1 2
tab3|1 3
tab4|1 2 4
test_part|1 2), 'publication relation updated');

# TEST: insert data into the tables, create subscription and see if sync
# replicates the right columns
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab1 VALUES (1, 2, 3);
	INSERT INTO tab1 VALUES (4, 5, 6);
));

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab3 VALUES (1, 2, 3);
	INSERT INTO tab3 VALUES (4, 5, 6);
));

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab4 VALUES (1, 'red', 3, 'oh my');
	INSERT INTO tab4 VALUES (2, 'blue', 4, 'hello');
));

# replication of partitioned table
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_part VALUES (1, 'abc', '2021-07-04 12:00:00');
	INSERT INTO test_part VALUES (2, 'bcd', '2021-07-03 11:12:13');
	INSERT INTO test_part VALUES (7, 'abc', '2021-07-04 12:00:00');
	INSERT INTO test_part VALUES (8, 'bcd', '2021-07-03 11:12:13');
));

# create subscription for the publication, wait for sync to complete,
# then check the sync results
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1
));

$node_subscriber->wait_for_subscription_sync;

# tab1: only (a,b) is replicated
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1|2|
4|5|), 'insert on column tab1.c is not replicated');

# tab3: only (a,c) is replicated
$result = $node_subscriber->safe_psql('postgres',
	qq(SELECT * FROM tab3 ORDER BY "a'"));
is( $result, qq(1|3
4|6), 'insert on column tab3.b is not replicated');

# tab4: only (a,b,d) is replicated
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab4 ORDER BY a");
is( $result, qq(1|red|oh my
2|blue|hello), 'insert on column tab4.c is not replicated');

# test_part: (a,b) is replicated
$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM test_part ORDER BY a");
is( $result, qq(1|abc
2|bcd
7|abc
8|bcd), 'insert on column test_part.c columns is not replicated');


# TEST: now insert more data into the tables, and wait until we replicate
# them (not by tablesync, but regular decoding and replication)

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab1 VALUES (2, 3, 4);
	INSERT INTO tab1 VALUES (5, 6, 7);
));

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab3 VALUES (2, 3, 4);
	INSERT INTO tab3 VALUES (5, 6, 7);
));

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab4 VALUES (3, 'red', 5, 'foo');
	INSERT INTO tab4 VALUES (4, 'blue', 6, 'bar');
));

# replication of partitioned table
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_part VALUES (3, 'xxx', '2022-02-01 10:00:00');
	INSERT INTO test_part VALUES (4, 'yyy', '2022-03-02 15:12:13');
	INSERT INTO test_part VALUES (9, 'zzz', '2022-04-03 21:00:00');
	INSERT INTO test_part VALUES (10, 'qqq', '2022-05-04 22:12:13');
));

# wait for catchup before checking the subscriber
$node_publisher->wait_for_catchup('sub1');

# tab1: only (a,b) is replicated
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab1 ORDER BY a");
is( $result, qq(1|2|
2|3|
4|5|
5|6|), 'insert on column tab1.c is not replicated');

# tab3: only (a,c) is replicated
$result = $node_subscriber->safe_psql('postgres',
	qq(SELECT * FROM tab3 ORDER BY "a'"));
is( $result, qq(1|3
2|4
4|6
5|7), 'insert on column tab3.b is not replicated');

# tab4: only (a,b,d) is replicated
$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab4 ORDER BY a");
is( $result, qq(1|red|oh my
2|blue|hello
3|red|foo
4|blue|bar), 'insert on column tab4.c is not replicated');

# test_part: (a,b) is replicated
$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM test_part ORDER BY a");
is( $result, qq(1|abc
2|bcd
3|xxx
4|yyy
7|abc
8|bcd
9|zzz
10|qqq), 'insert on column test_part.c columns is not replicated');


# TEST: do some updates on some of the tables, both on columns included
# in the column list and other

# tab1: update of replicated column
$node_publisher->safe_psql('postgres',
	qq(UPDATE tab1 SET "B" = 2 * "B" where a = 1));

# tab1: update of non-replicated column
$node_publisher->safe_psql('postgres',
	qq(UPDATE tab1 SET c = 2*c where a = 4));

# tab3: update of non-replicated
$node_publisher->safe_psql('postgres',
	qq(UPDATE tab3 SET "B" = "B" || ' updated' where "a'" = 4));

# tab3: update of replicated column
$node_publisher->safe_psql('postgres',
	qq(UPDATE tab3 SET "c'" = 2 * "c'" where "a'" = 1));

# tab4
$node_publisher->safe_psql('postgres',
	qq(UPDATE tab4 SET b = 'blue', c = c * 2, d = d || ' updated' where a = 1)
);

# tab4
$node_publisher->safe_psql('postgres',
	qq(UPDATE tab4 SET b = 'red', c = c * 2, d = d || ' updated' where a = 2)
);

# wait for the replication to catch up, and check the UPDATE results got
# replicated correctly, with the right column list
$node_publisher->wait_for_catchup('sub1');

$result =
  $node_subscriber->safe_psql('postgres', qq(SELECT * FROM tab1 ORDER BY a));
is( $result,
	qq(1|4|
2|3|
4|5|
5|6|), 'only update on column tab1.b is replicated');

$result = $node_subscriber->safe_psql('postgres',
	qq(SELECT * FROM tab3 ORDER BY "a'"));
is( $result,
	qq(1|6
2|4
4|6
5|7), 'only update on column tab3.c is replicated');

$result =
  $node_subscriber->safe_psql('postgres', qq(SELECT * FROM tab4 ORDER BY a));

is( $result, qq(1|blue|oh my updated
2|red|hello updated
3|red|foo
4|blue|bar), 'update on column tab4.c is not replicated');


# TEST: add table with a column list, insert data, replicate

# insert some data before adding it to the publication
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab2 VALUES (1, 'abc', 3);
));

$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION pub1 ADD TABLE tab2 (a, b)");

$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION sub1 REFRESH PUBLICATION");

# wait for the tablesync to complete, add a bit more data and then check
# the results of the replication
$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab2 VALUES (2, 'def', 6);
));

$node_publisher->wait_for_catchup('sub1');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab2 ORDER BY a");
is( $result, qq(1|abc
2|def), 'insert on column tab2.c is not replicated');

# do a couple updates, check the correct stuff gets replicated
$node_publisher->safe_psql(
	'postgres', qq(
	UPDATE tab2 SET c = 5 where a = 1;
	UPDATE tab2 SET b = 'xyz' where a = 2;
));

$node_publisher->wait_for_catchup('sub1');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT * FROM tab2 ORDER BY a");
is( $result, qq(1|abc
2|xyz), 'update on column tab2.c is not replicated');


# TEST: add a table to two publications with same column lists, and
# create a single subscription replicating both publications
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab5 (a int PRIMARY KEY, b int, c int, d int);
	CREATE PUBLICATION pub2 FOR TABLE tab5 (a, b);
	CREATE PUBLICATION pub3 FOR TABLE tab5 (a, b);

	-- insert a couple initial records
	INSERT INTO tab5 VALUES (1, 11, 111, 1111);
	INSERT INTO tab5 VALUES (2, 22, 222, 2222);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab5 (a int PRIMARY KEY, b int, d int);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub2, pub3
));

$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub1');

# insert data and make sure the columns in column list get fully replicated
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab5 VALUES (3, 33, 333, 3333);
	INSERT INTO tab5 VALUES (4, 44, 444, 4444);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM tab5 ORDER BY a"),
	qq(1|11|
2|22|
3|33|
4|44|),
	'overlapping publications with overlapping column lists');


# TEST: create a table with a column list, then change the replica
# identity by replacing a primary key (but use a different column in
# the column list)
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab6 (a int PRIMARY KEY, b int, c int, d int);
	CREATE PUBLICATION pub4 FOR TABLE tab6 (a, b);

	-- initial data
	INSERT INTO tab6 VALUES (1, 22, 333, 4444);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab6 (a int PRIMARY KEY, b int, c int, d int);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub4
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab6 VALUES (2, 33, 444, 5555);
	UPDATE tab6 SET b = b * 2, c = c * 3, d = d * 4;
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM tab6 ORDER BY a"),
	qq(1|44||
2|66||), 'replication with the original primary key');

# now redefine the constraint - move the primary key to a different column
# (which is still covered by the column list, though)

$node_publisher->safe_psql(
	'postgres', qq(
	ALTER TABLE tab6 DROP CONSTRAINT tab6_pkey;
	ALTER TABLE tab6 ADD PRIMARY KEY (b);
));

# we need to do the same thing on the subscriber
# XXX What would happen if this happens before the publisher ALTER? Or
# interleaved, somehow? But that seems unrelated to column lists.
$node_subscriber->safe_psql(
	'postgres', qq(
	ALTER TABLE tab6 DROP CONSTRAINT tab6_pkey;
	ALTER TABLE tab6 ADD PRIMARY KEY (b);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SUBSCRIPTION sub1 REFRESH PUBLICATION
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab6 VALUES (3, 55, 666, 8888);
	UPDATE tab6 SET b = b * 2, c = c * 3, d = d * 4;
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM tab6 ORDER BY a"),
	qq(1|88||
2|132||
3|110||),
	'replication with the modified primary key');


# TEST: create a table with a column list, then change the replica
# identity by replacing a primary key with a key on multiple columns
# (all of them covered by the column list)
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab7 (a int PRIMARY KEY, b int, c int, d int);
	CREATE PUBLICATION pub5 FOR TABLE tab7 (a, b);

	-- some initial data
	INSERT INTO tab7 VALUES (1, 22, 333, 4444);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab7 (a int PRIMARY KEY, b int, c int, d int);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub5
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab7 VALUES (2, 33, 444, 5555);
	UPDATE tab7 SET b = b * 2, c = c * 3, d = d * 4;
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM tab7 ORDER BY a"),
	qq(1|44||
2|66||), 'replication with the original primary key');

# now redefine the constraint - move the primary key to a different column
# (which is not covered by the column list)
$node_publisher->safe_psql(
	'postgres', qq(
	ALTER TABLE tab7 DROP CONSTRAINT tab7_pkey;
	ALTER TABLE tab7 ADD PRIMARY KEY (a, b);
));

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab7 VALUES (3, 55, 666, 7777);
	UPDATE tab7 SET b = b * 2, c = c * 3, d = d * 4;
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM tab7 ORDER BY a"),
	qq(1|88||
2|132||
3|110||),
	'replication with the modified primary key');

# now switch the primary key again to another columns not covered by the
# column list, but also generate writes between the drop and creation
# of the new constraint

$node_publisher->safe_psql(
	'postgres', qq(
	ALTER TABLE tab7 DROP CONSTRAINT tab7_pkey;
	INSERT INTO tab7 VALUES (4, 77, 888, 9999);
	-- update/delete is not allowed for tables without RI
	ALTER TABLE tab7 ADD PRIMARY KEY (b, a);
	UPDATE tab7 SET b = b * 2, c = c * 3, d = d * 4;
	DELETE FROM tab7 WHERE a = 1;
));

$node_publisher->safe_psql(
	'postgres', qq(
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM tab7 ORDER BY a"),
	qq(2|264||
3|220||
4|154||),
	'replication with the modified primary key');


# TEST: partitioned tables (with publish_via_partition_root = false)
# and replica identity. The (leaf) partitions may have different RI, so
# we need to check the partition RI (with respect to the column list)
# while attaching the partition.

# First, let's create a partitioned table with two partitions, each with
# a different RI, but a column list not covering all those RI.

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_a (a int, b int, c int) PARTITION BY LIST (a);

	CREATE TABLE test_part_a_1 PARTITION OF test_part_a FOR VALUES IN (1,2,3,4,5);
	ALTER TABLE test_part_a_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_a_1 REPLICA IDENTITY USING INDEX test_part_a_1_pkey;

	CREATE TABLE test_part_a_2 PARTITION OF test_part_a FOR VALUES IN (6,7,8,9,10);
	ALTER TABLE test_part_a_2 ADD PRIMARY KEY (b);
	ALTER TABLE test_part_a_2 REPLICA IDENTITY USING INDEX test_part_a_2_pkey;

	-- initial data, one row in each partition
	INSERT INTO test_part_a VALUES (1, 3);
	INSERT INTO test_part_a VALUES (6, 4);
));

# do the same thing on the subscriber (with the opposite column order)
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_a (b int, a int) PARTITION BY LIST (a);

	CREATE TABLE test_part_a_1 PARTITION OF test_part_a FOR VALUES IN (1,2,3,4,5);
	ALTER TABLE test_part_a_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_a_1 REPLICA IDENTITY USING INDEX test_part_a_1_pkey;

	CREATE TABLE test_part_a_2 PARTITION OF test_part_a FOR VALUES IN (6,7,8,9,10);
	ALTER TABLE test_part_a_2 ADD PRIMARY KEY (b);
	ALTER TABLE test_part_a_2 REPLICA IDENTITY USING INDEX test_part_a_2_pkey;
));

# create a publication replicating just the column "a", which is not enough
# for the second partition
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION pub6 FOR TABLE test_part_a (b, a) WITH (publish_via_partition_root = true);
	ALTER PUBLICATION pub6 ADD TABLE test_part_a_1 (a);
	ALTER PUBLICATION pub6 ADD TABLE test_part_a_2 (b);
));

# create the subscription for the above publication, wait for sync to
# complete
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub6
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_part_a VALUES (2, 5);
	INSERT INTO test_part_a VALUES (7, 6);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT a, b FROM test_part_a ORDER BY a, b"),
	qq(1|3
2|5
6|4
7|6),
	'partitions with different replica identities not replicated correctly');

# This time start with a column list covering RI for all partitions, but
# then update the column list to not cover column "b" (needed by the
# second partition)

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_b (a int, b int) PARTITION BY LIST (a);

	CREATE TABLE test_part_b_1 PARTITION OF test_part_b FOR VALUES IN (1,2,3,4,5);
	ALTER TABLE test_part_b_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_b_1 REPLICA IDENTITY USING INDEX test_part_b_1_pkey;

	CREATE TABLE test_part_b_2 PARTITION OF test_part_b FOR VALUES IN (6,7,8,9,10);
	ALTER TABLE test_part_b_2 ADD PRIMARY KEY (b);
	ALTER TABLE test_part_b_2 REPLICA IDENTITY USING INDEX test_part_b_2_pkey;

	-- initial data, one row in each partitions
	INSERT INTO test_part_b VALUES (1, 1);
	INSERT INTO test_part_b VALUES (6, 2);
));

# do the same thing on the subscriber
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_b (a int, b int) PARTITION BY LIST (a);

	CREATE TABLE test_part_b_1 PARTITION OF test_part_b FOR VALUES IN (1,2,3,4,5);
	ALTER TABLE test_part_b_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_b_1 REPLICA IDENTITY USING INDEX test_part_b_1_pkey;

	CREATE TABLE test_part_b_2 PARTITION OF test_part_b FOR VALUES IN (6,7,8,9,10);
	ALTER TABLE test_part_b_2 ADD PRIMARY KEY (b);
	ALTER TABLE test_part_b_2 REPLICA IDENTITY USING INDEX test_part_b_2_pkey;
));

# create a publication replicating both columns, which is sufficient for
# both partitions
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION pub7 FOR TABLE test_part_b (a, b) WITH (publish_via_partition_root = true);
));

# create the subscription for the above publication, wait for sync to
# complete
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub7
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_part_b VALUES (2, 3);
	INSERT INTO test_part_b VALUES (7, 4);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM test_part_b ORDER BY a, b"),
	qq(1|1
2|3
6|2
7|4),
	'partitions with different replica identities not replicated correctly');


# TEST: This time start with a column list covering RI for all partitions,
# but then update RI for one of the partitions to not be covered by the
# column list anymore.

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_c (a int, b int, c int) PARTITION BY LIST (a);

	CREATE TABLE test_part_c_1 PARTITION OF test_part_c FOR VALUES IN (1,3);
	ALTER TABLE test_part_c_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_c_1 REPLICA IDENTITY USING INDEX test_part_c_1_pkey;

	CREATE TABLE test_part_c_2 PARTITION OF test_part_c FOR VALUES IN (2,4);
	ALTER TABLE test_part_c_2 ADD PRIMARY KEY (b);
	ALTER TABLE test_part_c_2 REPLICA IDENTITY USING INDEX test_part_c_2_pkey;

	-- initial data, one row for each partition
	INSERT INTO test_part_c VALUES (1, 3, 5);
	INSERT INTO test_part_c VALUES (2, 4, 6);
));

# do the same thing on the subscriber
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_c (a int, b int, c int) PARTITION BY LIST (a);

	CREATE TABLE test_part_c_1 PARTITION OF test_part_c FOR VALUES IN (1,3);
	ALTER TABLE test_part_c_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_c_1 REPLICA IDENTITY USING INDEX test_part_c_1_pkey;

	CREATE TABLE test_part_c_2 PARTITION OF test_part_c FOR VALUES IN (2,4);
	ALTER TABLE test_part_c_2 ADD PRIMARY KEY (b);
	ALTER TABLE test_part_c_2 REPLICA IDENTITY USING INDEX test_part_c_2_pkey;
));

# create a publication replicating data through partition root, with a column
# list on the root, and then add the partitions one by one with separate
# column lists (but those are not applied)
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION pub8 FOR TABLE test_part_c WITH (publish_via_partition_root = false);
	ALTER PUBLICATION pub8 ADD TABLE test_part_c_1 (a,c);
	ALTER PUBLICATION pub8 ADD TABLE test_part_c_2 (a,b);
));

# create the subscription for the above publication, wait for sync to
# complete
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub8;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_part_c VALUES (3, 7, 8);
	INSERT INTO test_part_c VALUES (4, 9, 10);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM test_part_c ORDER BY a, b"),
	qq(1||5
2|4|
3||8
4|9|),
	'partitions with different replica identities not replicated correctly');


# create a publication not replicating data through partition root, without
# a column list on the root, and then add the partitions one by one with
# separate column lists
$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION pub8;
	CREATE PUBLICATION pub8 FOR TABLE test_part_c WITH (publish_via_partition_root = false);
	ALTER PUBLICATION pub8 ADD TABLE test_part_c_1 (a);
	ALTER PUBLICATION pub8 ADD TABLE test_part_c_2 (a,b);
));

# add the publication to our subscription, wait for sync to complete
$node_subscriber->safe_psql(
	'postgres', qq(
	ALTER SUBSCRIPTION sub1 REFRESH PUBLICATION;
	TRUNCATE test_part_c;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	TRUNCATE test_part_c;
	INSERT INTO test_part_c VALUES (1, 3, 5);
	INSERT INTO test_part_c VALUES (2, 4, 6);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM test_part_c ORDER BY a, b"),
	qq(1||
2|4|),
	'partitions with different replica identities not replicated correctly');


# TEST: Start with a single partition, with RI compatible with the column
# list, and then attach a partition with incompatible RI.

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_d (a int, b int) PARTITION BY LIST (a);

	CREATE TABLE test_part_d_1 PARTITION OF test_part_d FOR VALUES IN (1,3);
	ALTER TABLE test_part_d_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_d_1 REPLICA IDENTITY USING INDEX test_part_d_1_pkey;

	INSERT INTO test_part_d VALUES (1, 2);
));

# do the same thing on the subscriber (in fact, create both partitions right
# away, no need to delay that)
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_part_d (a int, b int) PARTITION BY LIST (a);

	CREATE TABLE test_part_d_1 PARTITION OF test_part_d FOR VALUES IN (1,3);
	ALTER TABLE test_part_d_1 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_d_1 REPLICA IDENTITY USING INDEX test_part_d_1_pkey;

	CREATE TABLE test_part_d_2 PARTITION OF test_part_d FOR VALUES IN (2,4);
	ALTER TABLE test_part_d_2 ADD PRIMARY KEY (a);
	ALTER TABLE test_part_d_2 REPLICA IDENTITY USING INDEX test_part_d_2_pkey;
));

# create a publication replicating both columns, which is sufficient for
# both partitions
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE PUBLICATION pub9 FOR TABLE test_part_d (a) WITH (publish_via_partition_root = true);
));

# create the subscription for the above publication, wait for sync to
# complete
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub9
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_part_d VALUES (3, 4);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM test_part_d ORDER BY a, b"),
	qq(1|
3|),
	'partitions with different replica identities not replicated correctly');


# TEST: With a table included in the publications which is FOR ALL TABLES, it
# means replicate all columns.

# drop unnecessary tables, so as not to interfere with the FOR ALL TABLES
$node_publisher->safe_psql(
	'postgres', qq(
	DROP TABLE tab1, tab2, tab3, tab4, tab5, tab6, tab7,
			   test_part, test_part_a, test_part_b, test_part_c, test_part_d;
));

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_mix_2 (a int PRIMARY KEY, b int, c int);
	CREATE PUBLICATION pub_mix_3 FOR TABLE test_mix_2 (a, b, c);
	CREATE PUBLICATION pub_mix_4 FOR ALL TABLES;

	-- initial data
	INSERT INTO test_mix_2 VALUES (1, 2, 3);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE test_mix_2 (a int PRIMARY KEY, b int, c int);
	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub_mix_3, pub_mix_4;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_mix_2 VALUES (4, 5, 6);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM test_mix_2"),
	qq(1|2|3
4|5|6),
	'all columns should be replicated');


# TEST: With a table included in the publication which is FOR TABLES IN
# SCHEMA, it means replicate all columns.

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE TABLE test_mix_3 (a int PRIMARY KEY, b int, c int);
));

$node_publisher->safe_psql(
	'postgres', qq(
	DROP TABLE test_mix_2;
	CREATE TABLE test_mix_3 (a int PRIMARY KEY, b int, c int);
	CREATE PUBLICATION pub_mix_5 FOR TABLE test_mix_3 (a, b, c);
	CREATE PUBLICATION pub_mix_6 FOR TABLES IN SCHEMA public;

	-- initial data
	INSERT INTO test_mix_3 VALUES (1, 2, 3);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub_mix_5, pub_mix_6;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_mix_3 VALUES (4, 5, 6);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM test_mix_3"),
	qq(1|2|3
4|5|6),
	'all columns should be replicated');


# TEST: Check handling of publish_via_partition_root - if a partition is
# published through partition root, we should only apply the column list
# defined for the whole table (not the partitions) - both during the initial
# sync and when replicating changes. This is what we do for row filters.

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;

	CREATE TABLE test_root (a int PRIMARY KEY, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE test_root_1 PARTITION OF test_root FOR VALUES FROM (1) TO (10);
	CREATE TABLE test_root_2 PARTITION OF test_root FOR VALUES FROM (10) TO (20);
));

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_root (a int PRIMARY KEY, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE test_root_1 PARTITION OF test_root FOR VALUES FROM (1) TO (10);
	CREATE TABLE test_root_2 PARTITION OF test_root FOR VALUES FROM (10) TO (20);

	CREATE PUBLICATION pub_test_root FOR TABLE test_root (a) WITH (publish_via_partition_root = true);
	CREATE PUBLICATION pub_test_root_1 FOR TABLE test_root_1 (a, b);

	-- initial data
	INSERT INTO test_root VALUES (1, 2, 3);
	INSERT INTO test_root VALUES (10, 20, 30);
));

# Subscribe to pub_test_root and pub_test_root_1 at the same time, which means
# that the initial data will be synced once, and only the column list of the
# parent table (test_root) in the publication pub_test_root will be used for
# both table sync and data replication.
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub_test_root, pub_test_root_1;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_root VALUES (2, 3, 4);
	INSERT INTO test_root VALUES (11, 21, 31);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM test_root ORDER BY a, b, c"),
	qq(1||
2||
10||
11||),
	'publication via partition root applies column list');


# TEST: Multiple publications which publish schema of parent table and
# partition. The partition is published through two publications, once
# through a schema (so no column list) containing the parent, and then
# also directly (with all columns). The expected outcome is there is
# no column list.

$node_publisher->safe_psql(
	'postgres', qq(
	DROP PUBLICATION pub1, pub2, pub3, pub4, pub5, pub6, pub7, pub8;

	CREATE SCHEMA s1;
	CREATE TABLE s1.t (a int, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE t_1 PARTITION OF s1.t FOR VALUES FROM (1) TO (10);

	CREATE PUBLICATION pub1 FOR TABLES IN SCHEMA s1;
	CREATE PUBLICATION pub2 FOR TABLE t_1(a, b, c);

	-- initial data
	INSERT INTO s1.t VALUES (1, 2, 3);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SCHEMA s1;
	CREATE TABLE s1.t (a int, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE t_1 PARTITION OF s1.t FOR VALUES FROM (1) TO (10);

	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1, pub2;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO s1.t VALUES (4, 5, 6);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM s1.t ORDER BY a"),
	qq(1|2|3
4|5|6),
	'two publications, publishing the same relation');

# Now resync the subscription, but with publications in the opposite order.
# The result should be the same.

$node_subscriber->safe_psql(
	'postgres', qq(
	TRUNCATE s1.t;

	ALTER SUBSCRIPTION sub1 SET PUBLICATION pub2, pub1;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO s1.t VALUES (7, 8, 9);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql('postgres', "SELECT * FROM s1.t ORDER BY a"),
	qq(7|8|9),
	'two publications, publishing the same relation');


# TEST: One publication, containing both the parent and child relations.
# The expected outcome is list "a", because that's the column list defined
# for the top-most ancestor added to the publication.

$node_publisher->safe_psql(
	'postgres', qq(
	DROP SCHEMA s1 CASCADE;
	CREATE TABLE t (a int, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE t_1 PARTITION OF t FOR VALUES FROM (1) TO (10)
		   PARTITION BY RANGE (a);
	CREATE TABLE t_2 PARTITION OF t_1 FOR VALUES FROM (1) TO (10);

	CREATE PUBLICATION pub3 FOR TABLE t_1 (a), t_2
	  WITH (PUBLISH_VIA_PARTITION_ROOT);

	-- initial data
	INSERT INTO t VALUES (1, 2, 3);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SCHEMA s1 CASCADE;
	CREATE TABLE t (a int, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE t_1 PARTITION OF t FOR VALUES FROM (1) TO (10)
		   PARTITION BY RANGE (a);
	CREATE TABLE t_2 PARTITION OF t_1 FOR VALUES FROM (1) TO (10);

	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub3;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO t VALUES (4, 5, 6);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM t ORDER BY a, b, c"),
	qq(1||
4||),
	'publication containing both parent and child relation');


# TEST: One publication, containing both the parent and child relations.
# The expected outcome is list "a", because that's the column list defined
# for the top-most ancestor added to the publication.
# Note: The difference from the preceding test is that in this case both
# relations have a column list defined.

$node_publisher->safe_psql(
	'postgres', qq(
	DROP TABLE t;
	CREATE TABLE t (a int, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE t_1 PARTITION OF t FOR VALUES FROM (1) TO (10)
		   PARTITION BY RANGE (a);
	CREATE TABLE t_2 PARTITION OF t_1 FOR VALUES FROM (1) TO (10);

	CREATE PUBLICATION pub4 FOR TABLE t_1 (a), t_2 (b)
	  WITH (PUBLISH_VIA_PARTITION_ROOT);

	-- initial data
	INSERT INTO t VALUES (1, 2, 3);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP TABLE t;
	CREATE TABLE t (a int, b int, c int) PARTITION BY RANGE (a);
	CREATE TABLE t_1 PARTITION OF t FOR VALUES FROM (1) TO (10)
		   PARTITION BY RANGE (a);
	CREATE TABLE t_2 PARTITION OF t_1 FOR VALUES FROM (1) TO (10);

	DROP SUBSCRIPTION sub1;
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub4;
));

$node_subscriber->wait_for_subscription_sync;

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO t VALUES (4, 5, 6);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM t ORDER BY a, b, c"),
	qq(1||
4||),
	'publication containing both parent and child relation');

# TEST: Only columns in the column list should exist in the old tuple of UPDATE
# and DELETE.

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_oldtuple_col (a int PRIMARY KEY, b int, c int);
	CREATE PUBLICATION pub_check_oldtuple FOR TABLE test_oldtuple_col (a, b);
	INSERT INTO test_oldtuple_col VALUES(1, 2, 3);
	SELECT * FROM pg_create_logical_replication_slot('test_slot', 'pgoutput');
	UPDATE test_oldtuple_col SET a = 2;
	DELETE FROM test_oldtuple_col;
));


# Check at 7th byte of binary data for the number of columns in the old tuple.
#
# 7 = 1 (count from 1) + 1 byte (message type) + 4 byte (relid) + 1 byte (flag
# for old key).
#
# The message type of UPDATE is 85('U').
# The message type of DELETE is 68('D').
$result = $node_publisher->safe_psql(
	'postgres', qq(
		SELECT substr(data, 7, 2) = int2send(2::smallint)
		FROM pg_logical_slot_peek_binary_changes('test_slot', NULL, NULL,
			'proto_version', '1',
			'publication_names', 'pub_check_oldtuple')
		WHERE get_byte(data, 0) = 85 OR get_byte(data, 0) = 68
));

is( $result, qq(t
t), 'check the number of columns in the old tuple');

# TEST: Dropped columns are not considered for the column list, and generated
# columns are not replicated if they are not explicitly included in the column
# list. So, the publication having a column list except for those columns and a
# publication without any column list (aka all columns as part of the columns
# list) are considered to have the same column list.
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_mix_4 (a int PRIMARY KEY, b int, c int, d int GENERATED ALWAYS AS (a + 1) STORED);
	ALTER TABLE test_mix_4 DROP COLUMN c;

	CREATE PUBLICATION pub_mix_7 FOR TABLE test_mix_4 (a, b);
	CREATE PUBLICATION pub_mix_8 FOR TABLE test_mix_4;

	-- initial data
	INSERT INTO test_mix_4 VALUES (1, 2);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE TABLE test_mix_4 (a int PRIMARY KEY, b int, c int, d int);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub_mix_7, pub_mix_8;
));

$node_subscriber->wait_for_subscription_sync;

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM test_mix_4 ORDER BY a"),
	qq(1|2||),
	'initial synchronization with multiple publications with the same column list'
);

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO test_mix_4 VALUES (3, 4);
));

$node_publisher->wait_for_catchup('sub1');

is( $node_subscriber->safe_psql(
		'postgres', "SELECT * FROM test_mix_4 ORDER BY a"),
	qq(1|2||
3|4||),
	'replication with multiple publications with the same column list');

# TEST: With a table included in multiple publications with different column
# lists, we should catch the error when creating the subscription.

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE test_mix_1 (a int PRIMARY KEY, b int, c int);
	CREATE PUBLICATION pub_mix_1 FOR TABLE test_mix_1 (a, b);
	CREATE PUBLICATION pub_mix_2 FOR TABLE test_mix_1 (a, c);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION sub1;
	CREATE TABLE test_mix_1 (a int PRIMARY KEY, b int, c int);
));

my ($cmdret, $stdout, $stderr) = $node_subscriber->psql(
	'postgres', qq(
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub_mix_1, pub_mix_2;
));

ok( $stderr =~
	  qr/cannot use different column lists for table "public.test_mix_1" in different publications/,
	'different column lists detected');

# TEST: If the column list is changed after creating the subscription, we
# should catch the error reported by walsender.

$node_publisher->safe_psql(
	'postgres', qq(
	ALTER PUBLICATION pub_mix_1 SET TABLE test_mix_1 (a, c);
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub_mix_1, pub_mix_2;
));

$node_publisher->wait_for_catchup('sub1');

$node_publisher->safe_psql(
	'postgres', qq(
	ALTER PUBLICATION pub_mix_1 SET TABLE test_mix_1 (a, b);
	INSERT INTO test_mix_1 VALUES(1, 1, 1);
));

$offset = $node_publisher->wait_for_log(
	qr/cannot use different column lists for table "public.test_mix_1" in different publications/,
	$offset);

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
