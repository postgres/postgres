# Test logical replication with partitioned tables
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 51;

# setup

my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber1 = get_new_node('subscriber1');
$node_subscriber1->init(allows_streaming => 'logical');
$node_subscriber1->start;

my $node_subscriber2 = get_new_node('subscriber2');
$node_subscriber2->init(allows_streaming => 'logical');
$node_subscriber2->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# publisher
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION pub1");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_all FOR ALL TABLES");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b text) PARTITION BY LIST (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1_1 (b text, a int NOT NULL)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_1 FOR VALUES IN (1, 2, 3)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1_2 PARTITION OF tab1 FOR VALUES IN (4, 5, 6)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1_def PARTITION OF tab1 DEFAULT");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION pub1 ADD TABLE tab1, tab1_1");

# subscriber1
#
# This is partitioned differently from the publisher.  tab1_2 is
# subpartitioned.  This tests the tuple routing code on the
# subscriber.
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1 (c text, a int PRIMARY KEY, b text) PARTITION BY LIST (a)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_1 (b text, c text DEFAULT 'sub1_tab1', a int NOT NULL)"
);
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_1 FOR VALUES IN (1, 2, 3)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2 PARTITION OF tab1 (c DEFAULT 'sub1_tab1') FOR VALUES IN (4, 5, 6) PARTITION BY LIST (a)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2_1 (c text, b text, a int NOT NULL)");
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab1_2 ATTACH PARTITION tab1_2_1 FOR VALUES IN (5)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2_2 PARTITION OF tab1_2 FOR VALUES IN (4, 6)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_def PARTITION OF tab1 (c DEFAULT 'sub1_tab1') DEFAULT"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);

# subscriber 2
#
# This does not use partitioning.  The tables match the leaf tables on
# the publisher.
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_1', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_2 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_2', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_def (a int PRIMARY KEY, b text, c text DEFAULT 'sub2_tab1_def')"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub2 CONNECTION '$publisher_connstr' PUBLICATION pub_all"
);

# Wait for initial sync of all subscriptions
my $synced_query =
  "SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');";
$node_subscriber1->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";
$node_subscriber2->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# Tests for replication using leaf partition identity and schema

# insert
$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (1)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_1 (a) VALUES (3)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_2 VALUES (5)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (0)");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

my $result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub1_tab1|0
sub1_tab1|1
sub1_tab1|3
sub1_tab1|5), 'inserts into tab1 and its partitions replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_1 ORDER BY 1");
is($result, qq(5), 'inserts into tab1_2 replicated into tab1_2_1 correctly');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_2 ORDER BY 1");
is($result, qq(), 'inserts into tab1_2 replicated into tab1_2_2 correctly');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_1|1
sub2_tab1_1|3), 'inserts into tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is($result, qq(sub2_tab1_2|5), 'inserts into tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_def ORDER BY 1, 2");
is($result, qq(sub2_tab1_def|0), 'inserts into tab1_def replicated');

# update (replicated as update)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 2 WHERE a = 1");
# All of the following cause an update to be applied to a partitioned
# table on subscriber1: tab1_2 is leaf partition on publisher, whereas
# it's sub-partitioned on subscriber1.
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 5");
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 4 WHERE a = 6");
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 4");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub1_tab1|0
sub1_tab1|2
sub1_tab1|3
sub1_tab1|6), 'update of tab1_1, tab1_2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_1 ORDER BY 1");
is($result, qq(), 'updates of tab1_2 replicated into tab1_2_1 correctly');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_2 ORDER BY 1");
is($result, qq(6), 'updates of tab1_2 replicated into tab1_2_2 correctly');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_1|2
sub2_tab1_1|3), 'update of tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is($result, qq(sub2_tab1_2|6), 'tab1_2 updated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_def ORDER BY 1");
is($result, qq(sub2_tab1_def|0), 'tab1_def unchanged');

# update (replicated as delete+insert)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 1 WHERE a = 0");
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 4 WHERE a = 1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub1_tab1|2
sub1_tab1|3
sub1_tab1|4
sub1_tab1|6),
	'update of tab1 (delete from tab1_def + insert into tab1_1) replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_2 ORDER BY 1");
is( $result, qq(4
6), 'updates of tab1 (delete + insert) replicated into tab1_2_2 correctly');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_1|2
sub2_tab1_1|3), 'tab1_1 unchanged');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is( $result, qq(sub2_tab1_2|4
sub2_tab1_2|6), 'insert into tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a FROM tab1_def ORDER BY 1");
is($result, qq(), 'delete from tab1_def replicated');

# delete
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab1 WHERE a IN (2, 3, 5)");
$node_publisher->safe_psql('postgres', "DELETE FROM tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab1");
is($result, qq(), 'delete from tab1_1, tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1_1");
is($result, qq(), 'delete from tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1_2");
is($result, qq(), 'delete from tab1_2 replicated');

# truncate
$node_subscriber1->safe_psql('postgres',
	"INSERT INTO tab1 (a) VALUES (1), (2), (5)");
$node_subscriber2->safe_psql('postgres', "INSERT INTO tab1_2 (a) VALUES (2)");
$node_publisher->safe_psql('postgres', "TRUNCATE tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result =
  $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is( $result, qq(1
2), 'truncate of tab1_2 replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1_2 ORDER BY 1");
is($result, qq(), 'truncate of tab1_2 replicated');

$node_publisher->safe_psql('postgres', "TRUNCATE tab1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result =
  $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is($result, qq(), 'truncate of tab1_1 replicated');
$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is($result, qq(), 'truncate of tab1 replicated');

# Tests for replication using root table identity and schema

# publisher
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY, b text) PARTITION BY LIST (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2_1 (b text, a int NOT NULL)");
$node_publisher->safe_psql('postgres',
	"ALTER TABLE tab2 ATTACH PARTITION tab2_1 FOR VALUES IN (0, 1, 2, 3)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2_2 PARTITION OF tab2 FOR VALUES IN (5, 6)");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab3 (a int PRIMARY KEY, b text) PARTITION BY LIST (a)");
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab3_1 PARTITION OF tab3 FOR VALUES IN (0, 1, 2, 3, 5, 6)");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION pub_all SET (publish_via_partition_root = true)");
# Note: tab3_1's parent is not in the publication, in which case its
# changes are published using own identity.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_viaroot FOR TABLE tab2, tab3_1 WITH (publish_via_partition_root = true)"
);

# subscriber 1
$node_subscriber1->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY, c text DEFAULT 'sub1_tab2', b text) PARTITION BY RANGE (a)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab2_1 (c text DEFAULT 'sub1_tab2', b text, a int NOT NULL)"
);
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab2 ATTACH PARTITION tab2_1 FOR VALUES FROM (0) TO (10)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab3_1 (c text DEFAULT 'sub1_tab3_1', b text, a int NOT NULL PRIMARY KEY)"
);
$node_subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub_viaroot CONNECTION '$publisher_connstr' PUBLICATION pub_viaroot"
);

# subscriber 2
$node_subscriber2->safe_psql('postgres', "DROP TABLE tab1");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1', b text) PARTITION BY HASH (a)"
);
# Note: tab1's partitions are named tab1_1 and tab1_2 on the publisher.
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_part1 (b text, c text, a int NOT NULL)");
$node_subscriber2->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_part1 FOR VALUES WITH (MODULUS 2, REMAINDER 0)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_part2 PARTITION OF tab1 FOR VALUES WITH (MODULUS 2, REMAINDER 1)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab2', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab3 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab3', b text)"
);
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab3_1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab3_1', b text)"
);
# Publication that sub2 points to now publishes via root, so must update
# subscription target relations.
$node_subscriber2->safe_psql('postgres',
	"ALTER SUBSCRIPTION sub2 REFRESH PUBLICATION");

# Wait for initial sync of all subscriptions
$node_subscriber1->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";
$node_subscriber2->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# insert
$node_publisher->safe_psql('postgres', "INSERT INTO tab1 VALUES (1), (0)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_1 (a) VALUES (3)");
$node_publisher->safe_psql('postgres', "INSERT INTO tab1_2 VALUES (5)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab2 VALUES (1), (0), (3), (5)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab3 VALUES (1), (0), (3), (5)");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub1_tab2|0
sub1_tab2|1
sub1_tab2|3
sub1_tab2|5), 'inserts into tab2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab3_1 ORDER BY 1, 2");
is( $result, qq(sub1_tab3_1|0
sub1_tab3_1|1
sub1_tab3_1|3
sub1_tab3_1|5), 'inserts into tab3_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1|0
sub2_tab1|1
sub2_tab1|3
sub2_tab1|5), 'inserts into tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub2_tab2|0
sub2_tab2|1
sub2_tab2|3
sub2_tab2|5), 'inserts into tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab3 ORDER BY 1, 2");
is( $result, qq(sub2_tab3|0
sub2_tab3|1
sub2_tab3|3
sub2_tab3|5), 'inserts into tab3 replicated');

# update (replicated as update)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 6 WHERE a = 5");
$node_publisher->safe_psql('postgres', "UPDATE tab2 SET a = 6 WHERE a = 5");
$node_publisher->safe_psql('postgres', "UPDATE tab3 SET a = 6 WHERE a = 5");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub1_tab2|0
sub1_tab2|1
sub1_tab2|3
sub1_tab2|6), 'update of tab2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab3_1 ORDER BY 1, 2");
is( $result, qq(sub1_tab3_1|0
sub1_tab3_1|1
sub1_tab3_1|3
sub1_tab3_1|6), 'update of tab3_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1|0
sub2_tab1|1
sub2_tab1|3
sub2_tab1|6), 'inserts into tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub2_tab2|0
sub2_tab2|1
sub2_tab2|3
sub2_tab2|6), 'inserts into tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab3 ORDER BY 1, 2");
is( $result, qq(sub2_tab3|0
sub2_tab3|1
sub2_tab3|3
sub2_tab3|6), 'inserts into tab3 replicated');

# update (replicated as delete+insert)
$node_publisher->safe_psql('postgres', "UPDATE tab1 SET a = 2 WHERE a = 6");
$node_publisher->safe_psql('postgres', "UPDATE tab2 SET a = 2 WHERE a = 6");
$node_publisher->safe_psql('postgres', "UPDATE tab3 SET a = 2 WHERE a = 6");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub1_tab2|0
sub1_tab2|1
sub1_tab2|2
sub1_tab2|3), 'update of tab2 replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab3_1 ORDER BY 1, 2");
is( $result, qq(sub1_tab3_1|0
sub1_tab3_1|1
sub1_tab3_1|2
sub1_tab3_1|3), 'update of tab3_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is( $result, qq(sub2_tab1|0
sub2_tab1|1
sub2_tab1|2
sub2_tab1|3), 'update of tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab2 ORDER BY 1, 2");
is( $result, qq(sub2_tab2|0
sub2_tab2|1
sub2_tab2|2
sub2_tab2|3), 'update of tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab3 ORDER BY 1, 2");
is( $result, qq(sub2_tab3|0
sub2_tab3|1
sub2_tab3|2
sub2_tab3|3), 'update of tab3 replicated');

# delete
$node_publisher->safe_psql('postgres', "DELETE FROM tab1");
$node_publisher->safe_psql('postgres', "DELETE FROM tab2");
$node_publisher->safe_psql('postgres', "DELETE FROM tab3");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'delete tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1");
is($result, qq(), 'delete from tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'delete from tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab3");
is($result, qq(), 'delete from tab3 replicated');

# truncate
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1), (2), (5)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab2 VALUES (1), (2), (5)");
# these will NOT be replicated
$node_publisher->safe_psql('postgres', "TRUNCATE tab1_2, tab2_1, tab3_1");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result =
  $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab2 ORDER BY 1");
is( $result, qq(1
2
5), 'truncate of tab2_1 NOT replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1 ORDER BY 1");
is( $result, qq(1
2
5), 'truncate of tab1_2 NOT replicated');

$result =
  $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab2 ORDER BY 1");
is( $result, qq(1
2
5), 'truncate of tab2_1 NOT replicated');

$node_publisher->safe_psql('postgres', "TRUNCATE tab1, tab2, tab3");

$node_publisher->wait_for_catchup('sub_viaroot');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'truncate of tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab1");
is($result, qq(), 'truncate of tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab2");
is($result, qq(), 'truncate of tab2 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab3");
is($result, qq(), 'truncate of tab3 replicated');

$result = $node_subscriber2->safe_psql('postgres', "SELECT a FROM tab3_1");
is($result, qq(), 'truncate of tab3_1 replicated');
