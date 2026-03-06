
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Basic logical replication test
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
	"CREATE TABLE tab_ins AS SELECT a, a + 1 as b FROM generate_series(1,1002) AS a");

# Replicate the changes without columns
$node_publisher->safe_psql('postgres', "CREATE TABLE tab_no_col()");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_no_col default VALUES");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres', "CREATE EXTENSION postgres_fdw");
$node_subscriber->safe_psql('postgres', "CREATE TABLE tab_ins (a int, b int)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION tap_pub FOR TABLE tab_ins");

my $publisher_host = $node_publisher->host;
my $publisher_port = $node_publisher->port;
$node_subscriber->safe_psql('postgres',
	"CREATE SERVER tap_server FOREIGN DATA WRAPPER postgres_fdw OPTIONS (host '$publisher_host', port '$publisher_port', dbname 'postgres')"
);

$node_subscriber->safe_psql('postgres',
	"CREATE USER MAPPING FOR PUBLIC SERVER tap_server"
);

$node_subscriber->safe_psql('postgres',
	"CREATE FOREIGN TABLE f_tab_ins (a int, b int) SERVER tap_server OPTIONS(table_name 'tab_ins')"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub SERVER tap_server PUBLICATION tap_pub WITH (password_required=false)"
);

# Wait for initial table sync to finish
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM (SELECT f.b = l.b as match FROM tab_ins l, f_tab_ins f WHERE l.a = f.a) WHERE match");
is($result, qq(1002), 'check that initial data was copied to subscriber');

$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_ins SELECT a, a + 1 FROM generate_series(1003,1050) a");

$node_publisher->wait_for_catchup('tap_sub');

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*) FROM (SELECT f.b = l.b as match FROM tab_ins l, f_tab_ins f WHERE l.a = f.a) WHERE match");
is($result, qq(1050), 'check that inserted data was copied to subscriber');

done_testing();
