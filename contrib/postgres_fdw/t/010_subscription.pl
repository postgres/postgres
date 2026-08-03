
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test postgres_fdw foreign server for use with a subscription.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize publisher node
my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE tab_ins AS SELECT a, a + 1 as b FROM generate_series(1,1002) AS a"
);

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres', "CREATE EXTENSION postgres_fdw");
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE tab_ins (a int, b int)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE tab_ins");

my $publisher_host = $node_publisher->host;
my $publisher_port = $node_publisher->port;
$node_subscriber->safe_psql('postgres',
	"CREATE SERVER tap_server FOREIGN DATA WRAPPER postgres_fdw OPTIONS (host '$publisher_host', port '$publisher_port', dbname 'postgres')"
);

$node_subscriber->safe_psql('postgres',
	"CREATE USER MAPPING FOR PUBLIC SERVER tap_server OPTIONS (use_scram_passthrough 'true')"
);

my ($ret, $stdout, $stderr) = $node_subscriber->psql('postgres',
	"CREATE SUBSCRIPTION tap_sub SERVER tap_server PUBLICATION tap_pub WITH (password_required=false)"
);
isnt($ret, 0, 'CREATE SUBSCRIPTION fails with use_scram_passthrough');
like(
	$stderr,
	qr/ERROR.*SCRAM pass-through authentication is not supported for subscription connections/,
	'CREATE SUBSCRIPTION gives correct connection error');

$node_subscriber->safe_psql('postgres',
	"ALTER USER MAPPING FOR PUBLIC SERVER tap_server OPTIONS (DROP use_scram_passthrough)"
);

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub SERVER tap_server PUBLICATION tap_pub WITH (password_required=false)"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT MAX(a) FROM tab_ins");
is($result, qq(1002), 'check that initial data was copied to subscriber');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT a, a + 1 FROM generate_series(1003,1050) a");

$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT MAX(a) FROM tab_ins");
is($result, qq(1050), 'check that inserted data was copied to subscriber');

# change to CONNECTION and confirm invalidation
my $log_offset = -s $node_subscriber->logfile;
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr'");
$node_subscriber->wait_for_log(
	qr/logical replication worker for subscription "tap_sub" will restart because of a parameter change/,
	$log_offset);

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT a, a + 1 FROM generate_series(1051,1057) a");

$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT MAX(a) FROM tab_ins");
is($result, qq(1057),
	'check subscription after ALTER SUBSCRIPTION ... CONNECTION');

# change back to SERVER and confirm invalidation
$log_offset = -s $node_subscriber->logfile;
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SERVER tap_server");
$node_subscriber->wait_for_log(
	qr/logical replication worker for subscription "tap_sub" will restart because of a parameter change/,
	$log_offset);

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT a, a + 1 FROM generate_series(1058,1073) a");

$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT MAX(a) FROM tab_ins");
is($result, qq(1073),
	'check subscription after ALTER SUBSCRIPTION ... SERVER');

# Check that REASSIGN OWNED processes a subscription only when run in the
# database to which the subscription belongs.
$node_subscriber->safe_psql(
	'postgres', q{
	CREATE ROLE regress_sub_old;
	CREATE ROLE regress_sub_new;
	GRANT pg_create_subscription TO regress_sub_old;
	CREATE DATABASE regress_sub_db;
});

$node_subscriber->safe_psql(
	'regress_sub_db', qq{
	CREATE EXTENSION postgres_fdw;
	CREATE SERVER regress_sub_server FOREIGN DATA WRAPPER postgres_fdw
		OPTIONS (host '$publisher_host', port '$publisher_port', dbname 'postgres');
	CREATE USER MAPPING FOR regress_sub_old SERVER regress_sub_server
		OPTIONS (user 'unused', password 'secret');
	CREATE USER MAPPING FOR regress_sub_new SERVER regress_sub_server
		OPTIONS (user 'unused', password 'secret');
	GRANT USAGE ON FOREIGN SERVER regress_sub_server
		TO regress_sub_old, regress_sub_new;
	GRANT CREATE ON DATABASE regress_sub_db TO regress_sub_old;
	SET SESSION AUTHORIZATION regress_sub_old;
	CREATE SUBSCRIPTION regress_sub SERVER regress_sub_server
		PUBLICATION tap_pub WITH (connect = false, slot_name = NONE);
});

$node_subscriber->safe_psql('postgres',
	'REASSIGN OWNED BY regress_sub_old TO regress_sub_new');
$result = $node_subscriber->safe_psql(
	'postgres',
	q{SELECT subowner::regrole FROM pg_subscription
	  WHERE subname = 'regress_sub'});
is($result, 'regress_sub_old',
	'REASSIGN OWNED in another database skips subscription');

$node_subscriber->safe_psql('regress_sub_db',
	'REASSIGN OWNED BY regress_sub_old TO regress_sub_new');
$result = $node_subscriber->safe_psql(
	'regress_sub_db',
	q{SELECT subowner::regrole FROM pg_subscription
	  WHERE subname = 'regress_sub'});
is($result, 'regress_sub_new',
	'REASSIGN OWNED in subscription database changes owner');

done_testing();
