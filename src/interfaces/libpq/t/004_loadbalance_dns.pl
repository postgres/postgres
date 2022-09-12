# Copyright (c) 2023, PostgreSQL Global Development Group
use strict;
use warnings;
use Config;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# This tests loadbalancing based on a DNS entry that contains multiple records
# for different IPs. Since setting up a DNS server is more effort than we
# consider reasonable to run this test, this situation is instead immitated by
# using a hosts file where a single hostname maps to multiple different IP
# addresses. This test requires the adminstrator to add the following lines to
# the hosts file (if we detect that this hasn't happend we skip the test):
#
# 127.0.0.1 pg-loadbalancetest
# 127.0.0.2 pg-loadbalancetest
# 127.0.0.3 pg-loadbalancetest
#
# Windows or Linux are required to run this test because these OSes allow
# binding to 127.0.0.2 and 127.0.0.3 addresess by default, but other OSes
# don't. We need to bind to different IP addresses, so that we can use these
# different IP addresses in the hosts file.
#
# The hosts file needs to be prepared before running this test. We don't do it
# on the fly, because it requires root permissions to change the hosts file. In
# CI we set up the previously mentioned rules in the hosts file, so that this
# load balancing method is tested.

# Cluster setup which is shared for testing both load balancing methods
my $can_bind_to_127_0_0_2 = $Config{osname} eq 'linux' || $PostgreSQL::Test::Utils::windows_os;

# Checks for the requirements for testing load balancing method 2
if (!$can_bind_to_127_0_0_2) {
	plan skip_all => "OS could not bind to 127.0.0.2"
}

my $hosts_path;
if ($windows_os) {
	$hosts_path = 'c:\Windows\System32\Drivers\etc\hosts';
}
else
{
	$hosts_path = '/etc/hosts';
}

my $hosts_content = PostgreSQL::Test::Utils::slurp_file($hosts_path);

if ($hosts_content !~ m/pg-loadbalancetest/) {
	# Host file is not prepared for this test
	plan skip_all => "hosts file was not prepared for DNS load balance test"
}

$PostgreSQL::Test::Cluster::use_tcp = 1;
$PostgreSQL::Test::Cluster::test_pghost = '127.0.0.1';
my $port = PostgreSQL::Test::Cluster::get_free_port();
my $node1 = PostgreSQL::Test::Cluster->new('node1', port => $port);
my $node2 = PostgreSQL::Test::Cluster->new('node2', port => $port, own_host => 1);
my $node3 = PostgreSQL::Test::Cluster->new('node3', port => $port, own_host => 1);

# Create a data directory with initdb
$node1->init();
$node2->init();
$node3->init();

# Start the PostgreSQL server
$node1->start();
$node2->start();
$node3->start();

$node2->connect_ok("host=pg-loadbalancetest port=$port load_balance_hosts=random random_seed=33",
	"seed 33 selects node 2 first",
	sql => "SELECT 'connect4'",
	log_like => [qr/statement: SELECT 'connect4'/]);

$node1->connect_ok("host=pg-loadbalancetest port=$port load_balance_hosts=random random_seed=33",
	"seed 33 does not select node 1 first",
	sql => "SELECT 'connect4'",
	log_unlike => [qr/statement: SELECT 'connect4'/]);

$node3->connect_ok("host=pg-loadbalancetest port=$port load_balance_hosts=random random_seed=33",
	"seed 33 does not select node 3 first",
	sql => "SELECT 'connect4'",
	log_unlike => [qr/statement: SELECT 'connect4'/]);

$node2->stop();

$node1->connect_ok("host=pg-loadbalancetest port=$port load_balance_hosts=random random_seed=33",
	"seed 33 does select node 1 second",
	sql => "SELECT 'connect5'",
	log_like => [qr/statement: SELECT 'connect5'/]);

$node3->connect_ok("host=pg-loadbalancetest port=$port load_balance_hosts=random random_seed=33",
	"seed 33 does not select node 3 second",
	sql => "SELECT 'connect5'",
	log_unlike => [qr/statement: SELECT 'connect5'/]);

done_testing();
