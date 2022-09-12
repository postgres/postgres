# Copyright (c) 2023, PostgreSQL Global Development Group
use strict;
use warnings;
use Config;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# This tests load balancing across the list of different hosts in the host
# parameter of the connection string.

# Cluster setup which is shared for testing both load balancing methods
my $node1 = PostgreSQL::Test::Cluster->new('node1');
my $node2 = PostgreSQL::Test::Cluster->new('node2', own_host => 1);
my $node3 = PostgreSQL::Test::Cluster->new('node3', own_host => 1);

# Create a data directory with initdb
$node1->init();
$node2->init();
$node3->init();

# Start the PostgreSQL server
$node1->start();
$node2->start();
$node3->start();

# Start the tests for load balancing method 1
my $hostlist = $node1->host . ',' . $node2->host . ',' . $node3->host;
my $portlist = $node1->port . ',' . $node2->port . ',' . $node3->port;

$node1->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=1234",
	"seed 1234 selects node 1 first",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);

$node2->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=1234",
	"seed 1234 does not select node 2 first",
	sql => "SELECT 'connect1'",
	log_unlike => [qr/statement: SELECT 'connect1'/]);

$node3->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=1234",
	"seed 1234 does not select node 3 first",
	sql => "SELECT 'connect1'",
	log_unlike => [qr/statement: SELECT 'connect1'/]);

$node3->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=42",
	"seed 42 selects node 3 first",
	sql => "SELECT 'connect2'",
	log_like => [qr/statement: SELECT 'connect2'/]);

$node1->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=42",
	"seed 42 does not select node 1 first",
	sql => "SELECT 'connect2'",
	log_unlike => [qr/statement: SELECT 'connect2'/]);

$node2->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=42",
	"seed 42 does not select node 2 first",
	sql => "SELECT 'connect2'",
	log_unlike => [qr/statement: SELECT 'connect2'/]);

$node3->stop();

$node1->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=42",
	"seed 42 does select node 1 second",
	sql => "SELECT 'connect3'",
	log_like => [qr/statement: SELECT 'connect3'/]);

$node2->connect_ok("host=$hostlist port=$portlist load_balance_hosts=random random_seed=42",
	"seed 42 does not select node 2 second",
	sql => "SELECT 'connect3'",
	log_unlike => [qr/statement: SELECT 'connect3'/]);

$node3->start();

done_testing();

