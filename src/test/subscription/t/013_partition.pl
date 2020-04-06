# Test logical replication with partitioned tables
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 24;

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
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1");
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
	"CREATE TABLE tab1 (c text, a int PRIMARY KEY, b text) PARTITION BY LIST (a)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_1 (b text, c text DEFAULT 'sub1_tab1', a int NOT NULL)");

$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_1 FOR VALUES IN (1, 2, 3)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2 PARTITION OF tab1 (c DEFAULT 'sub1_tab1') FOR VALUES IN (4, 5, 6) PARTITION BY LIST (a)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2_1 (c text, b text, a int NOT NULL)");
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab1_2 ATTACH PARTITION tab1_2_1 FOR VALUES IN (5)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2_2 PARTITION OF tab1_2 FOR VALUES IN (4, 6)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_def PARTITION OF tab1 (c DEFAULT 'sub1_tab1') DEFAULT");
$node_subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1");

# subscriber 2
#
# This does not use partitioning.  The tables match the leaf tables on
# the publisher.
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1', b text)");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_1', b text)");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_2 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_2', b text)");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_def (a int PRIMARY KEY, b text, c text DEFAULT 'sub2_tab1_def')");
$node_subscriber2->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub2 CONNECTION '$publisher_connstr' PUBLICATION pub_all");

# Wait for initial sync of all subscriptions
my $synced_query =
  "SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');";
$node_subscriber1->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";
$node_subscriber2->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# insert
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1_1 (a) VALUES (3)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1_2 VALUES (5)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (0)");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

my $result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is($result, qq(sub1_tab1|0
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
is($result, qq(sub2_tab1_1|1
sub2_tab1_1|3), 'inserts into tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is($result, qq(sub2_tab1_2|5), 'inserts into tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_def ORDER BY 1, 2");
is($result, qq(sub2_tab1_def|0), 'inserts into tab1_def replicated');

# update (replicated as update)
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 2 WHERE a = 1");
# All of the following cause an update to be applied to a partitioned
# table on subscriber1: tab1_2 is leaf partition on publisher, whereas
# it's sub-partitioned on subscriber1.
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 6 WHERE a = 5");
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 4 WHERE a = 6");
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 6 WHERE a = 4");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is($result, qq(sub1_tab1|0
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
is($result, qq(sub2_tab1_1|2
sub2_tab1_1|3), 'update of tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is($result, qq(sub2_tab1_2|6), 'tab1_2 updated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_def ORDER BY 1");
is($result, qq(sub2_tab1_def|0), 'tab1_def unchanged');

# update (replicated as delete+insert)
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 1 WHERE a = 0");
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 4 WHERE a = 1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, a FROM tab1 ORDER BY 1, 2");
is($result, qq(sub1_tab1|2
sub1_tab1|3
sub1_tab1|4
sub1_tab1|6), 'update of tab1 (delete from tab1_def + insert into tab1_1) replicated');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1_2_2 ORDER BY 1");
is($result, qq(4
6), 'updates of tab1 (delete + insert) replicated into tab1_2_2 correctly');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_1 ORDER BY 1, 2");
is($result, qq(sub2_tab1_1|2
sub2_tab1_1|3), 'tab1_1 unchanged');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, a FROM tab1_2 ORDER BY 1, 2");
is($result, qq(sub2_tab1_2|4
sub2_tab1_2|6), 'insert into tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a FROM tab1_def ORDER BY 1");
is($result, qq(), 'delete from tab1_def replicated');

# delete
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab1 WHERE a IN (2, 3, 5)");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1");
is($result, qq(), 'delete from tab1_1, tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a FROM tab1_1");
is($result, qq(), 'delete from tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a FROM tab1_2");
is($result, qq(), 'delete from tab1_2 replicated');

# truncate
$node_subscriber1->safe_psql('postgres',
	"INSERT INTO tab1 (a) VALUES (1), (2), (5)");
$node_subscriber2->safe_psql('postgres',
	"INSERT INTO tab1_2 (a) VALUES (2)");
$node_publisher->safe_psql('postgres',
	"TRUNCATE tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1 ORDER BY 1");
is($result, qq(1
2), 'truncate of tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a FROM tab1_2 ORDER BY 1");
is($result, qq(), 'truncate of tab1_2 replicated');

$node_publisher->safe_psql('postgres',
	"TRUNCATE tab1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT a FROM tab1 ORDER BY 1");
is($result, qq(), 'truncate of tab1_1 replicated');
$result = $node_subscriber2->safe_psql('postgres',
	"SELECT a FROM tab1 ORDER BY 1");
is($result, qq(), 'truncate of tab1 replicated');
