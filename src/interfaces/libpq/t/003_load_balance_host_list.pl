# Copyright (c) 2023-2024, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
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

$node1->connect_fails(
	"host=$hostlist port=$portlist load_balance_hosts=doesnotexist",
	"load_balance_hosts doesn't accept unknown values",
	expected_stderr => qr/invalid load_balance_hosts value: "doesnotexist"/);

# load_balance_hosts=disable should always choose the first one.
$node1->connect_ok(
	"host=$hostlist port=$portlist load_balance_hosts=disable",
	"load_balance_hosts=disable connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);

# Statistically the following loop with load_balance_hosts=random will almost
# certainly connect at least once to each of the nodes. The chance of that not
# happening is so small that it's negligible: (2/3)^50 = 1.56832855e-9
foreach my $i (1 .. 50)
{
	$node1->connect_ok(
		"host=$hostlist port=$portlist load_balance_hosts=random",
		"repeated connections with random load balancing",
		sql => "SELECT 'connect2'");
}

my $node1_occurrences = () =
  $node1->log_content() =~ /statement: SELECT 'connect2'/g;
my $node2_occurrences = () =
  $node2->log_content() =~ /statement: SELECT 'connect2'/g;
my $node3_occurrences = () =
  $node3->log_content() =~ /statement: SELECT 'connect2'/g;

my $total_occurrences =
  $node1_occurrences + $node2_occurrences + $node3_occurrences;

ok($node1_occurrences > 1, "received at least one connection on node1");
ok($node2_occurrences > 1, "received at least one connection on node2");
ok($node3_occurrences > 1, "received at least one connection on node3");
ok($total_occurrences == 50, "received 50 connections across all nodes");

$node1->stop();
$node2->stop();

# load_balance_hosts=disable should continue trying hosts until it finds a
# working one.
$node3->connect_ok(
	"host=$hostlist port=$portlist load_balance_hosts=disable",
	"load_balance_hosts=disable continues until it connects to the a working node",
	sql => "SELECT 'connect3'",
	log_like => [qr/statement: SELECT 'connect3'/]);

# Also with load_balance_hosts=random we continue to the next nodes if previous
# ones are down. Connect a few times to make sure it's not just lucky.
foreach my $i (1 .. 5)
{
	$node3->connect_ok(
		"host=$hostlist port=$portlist load_balance_hosts=random",
		"load_balance_hosts=random continues until it connects to the a working node",
		sql => "SELECT 'connect4'",
		log_like => [qr/statement: SELECT 'connect4'/]);
}

done_testing();
