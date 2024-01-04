# Copyright (c) 2023-2024, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use Config;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bload_balance\b/)
{
	plan skip_all =>
	  'Potentially unsafe test load_balance not enabled in PG_TEST_EXTRA';
}

# This tests loadbalancing based on a DNS entry that contains multiple records
# for different IPs. Since setting up a DNS server is more effort than we
# consider reasonable to run this test, this situation is instead imitated by
# using a hosts file where a single hostname maps to multiple different IP
# addresses. This test requires the administrator to add the following lines to
# the hosts file (if we detect that this hasn't happened we skip the test):
#
# 127.0.0.1 pg-loadbalancetest
# 127.0.0.2 pg-loadbalancetest
# 127.0.0.3 pg-loadbalancetest
#
# Windows or Linux are required to run this test because these OSes allow
# binding to 127.0.0.2 and 127.0.0.3 addresses by default, but other OSes
# don't. We need to bind to different IP addresses, so that we can use these
# different IP addresses in the hosts file.
#
# The hosts file needs to be prepared before running this test. We don't do it
# on the fly, because it requires root permissions to change the hosts file. In
# CI we set up the previously mentioned rules in the hosts file, so that this
# load balancing method is tested.

# Cluster setup which is shared for testing both load balancing methods
my $can_bind_to_127_0_0_2 =
  $Config{osname} eq 'linux' || $PostgreSQL::Test::Utils::windows_os;

# Checks for the requirements for testing load balancing method 2
if (!$can_bind_to_127_0_0_2)
{
	plan skip_all => 'load_balance test only supported on Linux and Windows';
}

my $hosts_path;
if ($windows_os)
{
	$hosts_path = 'c:\Windows\System32\Drivers\etc\hosts';
}
else
{
	$hosts_path = '/etc/hosts';
}

my $hosts_content = PostgreSQL::Test::Utils::slurp_file($hosts_path);

my $hosts_count = () =
  $hosts_content =~ /127\.0\.0\.[1-3] pg-loadbalancetest/g;
if ($hosts_count != 3)
{
	# Host file is not prepared for this test
	plan skip_all => "hosts file was not prepared for DNS load balance test";
}

$PostgreSQL::Test::Cluster::use_tcp = 1;
$PostgreSQL::Test::Cluster::test_pghost = '127.0.0.1';
my $port = PostgreSQL::Test::Cluster::get_free_port();
my $node1 = PostgreSQL::Test::Cluster->new('node1', port => $port);
my $node2 =
  PostgreSQL::Test::Cluster->new('node2', port => $port, own_host => 1);
my $node3 =
  PostgreSQL::Test::Cluster->new('node3', port => $port, own_host => 1);

# Create a data directory with initdb
$node1->init();
$node2->init();
$node3->init();

# Start the PostgreSQL server
$node1->start();
$node2->start();
$node3->start();

# load_balance_hosts=disable should always choose the first one.
$node1->connect_ok(
	"host=pg-loadbalancetest port=$port load_balance_hosts=disable",
	"load_balance_hosts=disable connects to the first node",
	sql => "SELECT 'connect1'",
	log_like => [qr/statement: SELECT 'connect1'/]);


# Statistically the following loop with load_balance_hosts=random will almost
# certainly connect at least once to each of the nodes. The chance of that not
# happening is so small that it's negligible: (2/3)^50 = 1.56832855e-9
foreach my $i (1 .. 50)
{
	$node1->connect_ok(
		"host=pg-loadbalancetest port=$port load_balance_hosts=random",
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
	"host=pg-loadbalancetest port=$port load_balance_hosts=disable",
	"load_balance_hosts=disable continues until it connects to the a working node",
	sql => "SELECT 'connect3'",
	log_like => [qr/statement: SELECT 'connect3'/]);

# Also with load_balance_hosts=random we continue to the next nodes if previous
# ones are down. Connect a few times to make sure it's not just lucky.
foreach my $i (1 .. 5)
{
	$node3->connect_ok(
		"host=pg-loadbalancetest port=$port load_balance_hosts=random",
		"load_balance_hosts=random continues until it connects to the a working node",
		sql => "SELECT 'connect4'",
		log_like => [qr/statement: SELECT 'connect4'/]);
}

done_testing();
