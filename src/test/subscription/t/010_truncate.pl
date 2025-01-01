
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test TRUNCATE
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
$node_subscriber->append_conf('postgresql.conf',
	qq(max_logical_replication_workers = 6));
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
$node_subscriber->wait_for_subscription_sync;

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

# Test that truncate works for synchronous logical replication

$node_publisher->safe_psql('postgres',
	"ALTER SYSTEM SET synchronous_standby_names TO 'sub1'");
$node_publisher->safe_psql('postgres', "SELECT pg_reload_conf()");

# insert data to truncate

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab1 VALUES (1), (2), (3)");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(3|1|3), 'check synchronous logical replication');

$node_publisher->safe_psql('postgres', "TRUNCATE tab1");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab1");
is($result, qq(0||),
	'truncate replicated in synchronous logical replication');

$node_publisher->safe_psql('postgres',
	"ALTER SYSTEM RESET synchronous_standby_names");
$node_publisher->safe_psql('postgres', "SELECT pg_reload_conf()");

# test that truncate works for logical replication when there are multiple
# subscriptions for a single table

$node_publisher->safe_psql('postgres', "CREATE TABLE tab5 (a int)");

$node_subscriber->safe_psql('postgres', "CREATE TABLE tab5 (a int)");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub5 FOR TABLE tab5");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub5_1 CONNECTION '$publisher_connstr' PUBLICATION pub5"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub5_2 CONNECTION '$publisher_connstr' PUBLICATION pub5"
);

# wait for initial data sync
$node_subscriber->wait_for_subscription_sync;

# insert data to truncate

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab5 VALUES (1), (2), (3)");

$node_publisher->wait_for_catchup('sub5_1');
$node_publisher->wait_for_catchup('sub5_2');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab5");
is($result, qq(6|1|3), 'insert replicated for multiple subscriptions');

$node_publisher->safe_psql('postgres', "TRUNCATE tab5");

$node_publisher->wait_for_catchup('sub5_1');
$node_publisher->wait_for_catchup('sub5_2');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), min(a), max(a) FROM tab5");
is($result, qq(0||), 'truncate replicated for multiple subscriptions');

# check deadlocks
$result = $node_subscriber->safe_psql('postgres',
	"SELECT deadlocks FROM pg_stat_database WHERE datname='postgres'");
is($result, qq(0), 'no deadlocks detected');

done_testing();
