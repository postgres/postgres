# Test logical replication with partitioned tables
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 15;

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
	"CREATE TABLE tab1_2 PARTITION OF tab1 FOR VALUES IN (5, 6)");
$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION pub1 ADD TABLE tab1, tab1_1");

# subscriber1
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, b text, c text) PARTITION BY LIST (a)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_1 (b text, c text DEFAULT 'sub1_tab1', a int NOT NULL)");
$node_subscriber1->safe_psql('postgres',
	"ALTER TABLE tab1 ATTACH PARTITION tab1_1 FOR VALUES IN (1, 2, 3, 4)");
$node_subscriber1->safe_psql('postgres',
	"CREATE TABLE tab1_2 PARTITION OF tab1 (c DEFAULT 'sub1_tab1') FOR VALUES IN (5, 6)");
$node_subscriber1->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1");

# subscriber 2
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1', b text)");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_1 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_1', b text)");
$node_subscriber2->safe_psql('postgres',
	"CREATE TABLE tab1_2 (a int PRIMARY KEY, c text DEFAULT 'sub2_tab1_2', b text)");
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

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

my $result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1 GROUP BY 1");
is($result, qq(sub1_tab1|3|1|5), 'insert into tab1_1, tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1_1 GROUP BY 1");
is($result, qq(sub2_tab1_1|2|1|3), 'inserts into tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1_2 GROUP BY 1");
is($result, qq(sub2_tab1_2|1|5|5), 'inserts into tab1_2 replicated');

# update (no partition change)
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 2 WHERE a = 1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1 GROUP BY 1");
is($result, qq(sub1_tab1|3|2|5), 'update of tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1_1 GROUP BY 1");
is($result, qq(sub2_tab1_1|2|2|3), 'update of tab1_1 replicated');

# update (partition changes)
$node_publisher->safe_psql('postgres',
	"UPDATE tab1 SET a = 6 WHERE a = 2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1 GROUP BY 1");
is($result, qq(sub1_tab1|3|3|6), 'update of tab1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1_1 GROUP BY 1");
is($result, qq(sub2_tab1_1|1|3|3), 'delete from tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT c, count(*), min(a), max(a) FROM tab1_2 GROUP BY 1");
is($result, qq(sub2_tab1_2|2|5|6), 'insert into tab1_2 replicated');

# delete
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab1 WHERE a IN (3, 5)");
$node_publisher->safe_psql('postgres',
	"DELETE FROM tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(0||), 'delete from tab1_1, tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1_1");
is($result, qq(0||), 'delete from tab1_1 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1_2");
is($result, qq(0||), 'delete from tab1_2 replicated');

# truncate
$node_subscriber1->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1), (2), (5)");
$node_subscriber2->safe_psql('postgres',
	"INSERT INTO tab1_2 VALUES (2)");
$node_publisher->safe_psql('postgres',
	"TRUNCATE tab1_2");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(2|1|2), 'truncate of tab1_2 replicated');

$result = $node_subscriber2->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1_2");
is($result, qq(0||), 'truncate of tab1_2 replicated');

$node_publisher->safe_psql('postgres',
	"TRUNCATE tab1");

$node_publisher->wait_for_catchup('sub1');
$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber1->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(0||), 'truncate of tab1_1 replicated');
$result = $node_subscriber2->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(0||), 'truncate of tab1_1 replicated');
