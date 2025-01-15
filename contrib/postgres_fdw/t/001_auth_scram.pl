# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Test SCRAM authentication when opening a new connection with a foreign
# server.
#
# The test is executed by testing the SCRAM authentifcation on a looplback
# connection on the same server and with different servers.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

my $hostaddr = '127.0.0.1';
my $user = "user01";

my $db0 = "db0";    # For node1
my $db1 = "db1";    # For node1
my $db2 = "db2";    # For node2
my $fdw_server = "db1_fdw";
my $fdw_server2 = "db2_fdw";

my $node1 = PostgreSQL::Test::Cluster->new('node1');
my $node2 = PostgreSQL::Test::Cluster->new('node2');

$node1->init;
$node2->init;

$node1->start;
$node2->start;

# Test setup

$node1->safe_psql('postgres', qq'CREATE USER $user WITH password \'pass\'');
$node2->safe_psql('postgres', qq'CREATE USER $user WITH password \'pass\'');
$ENV{PGPASSWORD} = "pass";

$node1->safe_psql('postgres', qq'CREATE DATABASE $db0');
$node1->safe_psql('postgres', qq'CREATE DATABASE $db1');
$node2->safe_psql('postgres', qq'CREATE DATABASE $db2');

setup_table($node1, $db1, "t");
setup_table($node2, $db2, "t2");

$node1->safe_psql($db0, 'CREATE EXTENSION IF NOT EXISTS postgres_fdw');
setup_fdw_server($node1, $db0, $fdw_server, $node1, $db1);
setup_fdw_server($node1, $db0, $fdw_server2, $node2, $db2);

setup_user_mapping($node1, $db0, $fdw_server);
setup_user_mapping($node1, $db0, $fdw_server2);

# Make the user have the same SCRAM key on both servers. Forcing to have the
# same iteration and salt.
my $rolpassword = $node1->safe_psql('postgres',
	qq"SELECT rolpassword FROM pg_authid WHERE rolname = '$user';");
$node2->safe_psql('postgres', qq"ALTER ROLE $user PASSWORD '$rolpassword'");

setup_pghba($node1);
setup_pghba($node2);

# End of test setup

test_fdw_auth($node1, $db0, "t", $fdw_server,
	"SCRAM auth on the same database cluster must succeed");
test_fdw_auth($node1, $db0, "t2", $fdw_server2,
	"SCRAM auth on a different database cluster must succeed");
test_auth($node2, $db2, "t2",
	"SCRAM auth directly on foreign server should still succeed");

# Helper functions

sub test_auth
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $db, $tbl, $testname) = @_;
	my $connstr = $node->connstr($db) . qq' user=$user';

	my $ret = $node->safe_psql(
		$db,
		qq'SELECT count(1) FROM $tbl',
		connstr => $connstr);

	is($ret, '10', $testname);
}

sub test_fdw_auth
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $db, $tbl, $fdw, $testname) = @_;
	my $connstr = $node->connstr($db) . qq' user=$user';

	$node->safe_psql(
		$db,
		qq'IMPORT FOREIGN SCHEMA public LIMIT TO ($tbl) FROM SERVER $fdw INTO public;',
		connstr => $connstr);

	test_auth($node, $db, $tbl, $testname);
}

sub setup_pghba
{
	my ($node) = @_;

	unlink($node->data_dir . '/pg_hba.conf');
	$node->append_conf(
		'pg_hba.conf', qq{
	local   all             all                                     scram-sha-256
	host    all             all             $hostaddr/32            scram-sha-256
	});

	$node->restart;
}

sub setup_user_mapping
{
	my ($node, $db, $fdw) = @_;

	$node->safe_psql($db,
		qq'CREATE USER MAPPING FOR $user SERVER $fdw OPTIONS (user \'$user\');'
	);
	$node->safe_psql($db, qq'GRANT USAGE ON FOREIGN SERVER $fdw TO $user;');
	$node->safe_psql($db, qq'GRANT ALL ON SCHEMA public TO $user');
}

sub setup_fdw_server
{
	my ($node, $db, $fdw, $fdw_node, $dbname) = @_;
	my $host = $fdw_node->host;
	my $port = $fdw_node->port;

	$node->safe_psql(
		$db, qq'CREATE SERVER $fdw FOREIGN DATA WRAPPER postgres_fdw options (
		host \'$host\', port \'$port\', dbname \'$dbname\', use_scram_passthrough \'true\') '
	);
}

sub setup_table
{
	my ($node, $db, $tbl) = @_;

	$node->safe_psql($db,
		qq'CREATE TABLE $tbl AS SELECT g, g + 1 FROM generate_series(1,10) g(g)'
	);
	$node->safe_psql($db, qq'GRANT USAGE ON SCHEMA public TO $user');
	$node->safe_psql($db, qq'GRANT SELECT ON $tbl TO $user');
}

done_testing();
