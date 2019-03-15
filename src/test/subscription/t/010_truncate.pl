# Test TRUNCATE
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 9;

# setup

my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY)");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab1 (a int PRIMARY KEY)");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY)");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab2 (a int PRIMARY KEY)");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab3 (a int PRIMARY KEY)");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab3 (a int PRIMARY KEY)");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab4 (x int PRIMARY KEY, y int REFERENCES tab3)");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab4 (x int PRIMARY KEY, y int REFERENCES tab3)");

$node_subscriber->safe_psql('postgres',
	"CREATE SEQUENCE seq1 OWNED BY tab1.a");
$node_subscriber->safe_psql('postgres', "ALTER SEQUENCE seq1 START 101");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR TABLE tab1");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub2 FOR TABLE tab2 WITH (publish = insert)");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub3 FOR TABLE tab3, tab4");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub2 CONNECTION '$publisher_connstr' PUBLICATION pub2"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub3 CONNECTION '$publisher_connstr' PUBLICATION pub3"
);

# Wait for initial sync of all subscriptions
my $synced_query =
  "SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');";
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

# insert data to truncate

$node_subscriber->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1), (2), (3)");

$node_publisher->wait_for_catchup('sub1');

# truncate and check

$node_publisher->safe_psql('postgres', "TRUNCATE tab1");

$node_publisher->wait_for_catchup('sub1');

my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(0||), 'truncate replicated');

$result = $node_subscriber->safe_psql('postgres', "SELECT nextval('seq1')");
is($result, qq(1), 'sequence not restarted');

# truncate with restart identity

$node_publisher->safe_psql('postgres', "TRUNCATE tab1 RESTART IDENTITY");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres', "SELECT nextval('seq1')");
is($result, qq(101), 'truncate restarted identities');

# test publication that does not replicate truncate

$node_subscriber->safe_psql('postgres',
	"INSERT INTO tab2 VALUES (1), (2), (3)");

$node_publisher->safe_psql('postgres', "TRUNCATE tab2");

$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab2");
is($result, qq(3|1|3), 'truncate not replicated');

$node_publisher->safe_psql('postgres',
	"ALTER PUBLICATION pub2 SET (publish = 'insert, truncate')");

$node_publisher->safe_psql('postgres', "TRUNCATE tab2");

$node_publisher->wait_for_catchup('sub2');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab2");
is($result, qq(0||), 'truncate replicated after publication change');

# test multiple tables connected by foreign keys

$node_subscriber->safe_psql('postgres',
	"INSERT INTO tab3 VALUES (1), (2), (3)");
$node_subscriber->safe_psql('postgres',
	"INSERT INTO tab4 VALUES (11, 1), (111, 1), (22, 2)");

$node_publisher->safe_psql('postgres', "TRUNCATE tab3, tab4");

$node_publisher->wait_for_catchup('sub3');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab3");
is($result, qq(0||), 'truncate of multiple tables replicated');
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(x), max(x) FROM tab4");
is($result, qq(0||), 'truncate of multiple tables replicated');

# test truncate of multiple tables, some of which are not published

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub2");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub2");

$node_subscriber->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1), (2), (3)");
$node_subscriber->safe_psql('postgres',
	"INSERT INTO tab2 VALUES (1), (2), (3)");

$node_publisher->safe_psql('postgres', "TRUNCATE tab1, tab2");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(0||), 'truncate of multiple tables some not published');
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab2");
is($result, qq(3|1|3), 'truncate of multiple tables some not published');
