# Copyright (c) 2024-2025, PostgreSQL Global Development Group

# Test SCRAM authentication when opening a new connection with a foreign
# server.
#
# The test is executed by testing the SCRAM authentifcation on a loopback
# connection on the same server and with different servers.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if (!$use_unix_sockets)
{
	plan skip_all => "test requires Unix-domain sockets";
}

my $user = "user01";

my $db0 = "db0";                               # For node1
my $db1 = "db1";                               # For node1
my $db2 = "db2";                               # For node2
my $fdw_server = "db1_fdw";
my $fdw_server2 = "db2_fdw";
my $fdw_invalid_server = "db2_fdw_invalid";    # For invalid fdw options
my $fdw_invalid_server2 =
  "db2_fdw_invalid2";    # For invalid scram keys fdw options

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

$node1->safe_psql($db0, 'CREATE EXTENSION IF NOT EXISTS dblink');
setup_fdw_server($node1, $db0, $fdw_server, $node1, $db1);
setup_fdw_server($node1, $db0, $fdw_server2, $node2, $db2);
setup_invalid_fdw_server($node1, $db0, $fdw_invalid_server, $node2, $db2);
setup_fdw_server($node1, $db0, $fdw_invalid_server2, $node2, $db2);

setup_user_mapping($node1, $db0, $fdw_server);
setup_user_mapping($node1, $db0, $fdw_server2);
setup_user_mapping($node1, $db0, $fdw_invalid_server);

# Make the user have the same SCRAM key on both servers. Forcing to have the
# same iteration and salt.
my $rolpassword = $node1->safe_psql('postgres',
	qq"SELECT rolpassword FROM pg_authid WHERE rolname = '$user';");
$node2->safe_psql('postgres', qq"ALTER ROLE $user PASSWORD '$rolpassword'");

unlink($node1->data_dir . '/pg_hba.conf');
unlink($node2->data_dir . '/pg_hba.conf');

$node1->append_conf(
	'pg_hba.conf', qq{
local   db0             all                                     scram-sha-256
local   db1             all                                     scram-sha-256
}
);
$node2->append_conf(
	'pg_hba.conf', qq{
local   db2             all                                     scram-sha-256
}
);

$node1->restart;
$node2->restart;

# End of test setup

test_scram_keys_is_not_overwritten($node1, $db0, $fdw_invalid_server2);

test_fdw_auth($node1, $db0, "t", $fdw_server,
	"SCRAM auth on the same database cluster must succeed");

test_fdw_auth($node1, $db0, "t2", $fdw_server2,
	"SCRAM auth on a different database cluster must succeed");

test_fdw_auth_with_invalid_overwritten_require_auth($fdw_invalid_server);

# Ensure that trust connections fail without superuser opt-in.
unlink($node1->data_dir . '/pg_hba.conf');
unlink($node2->data_dir . '/pg_hba.conf');

$node1->append_conf(
	'pg_hba.conf', qq{
local   db0             all                                     scram-sha-256
local   db1             all                                     trust
}
);
$node2->append_conf(
	'pg_hba.conf', qq{
local   all             all                                     password
}
);

$node1->restart;
$node2->restart;

my ($ret, $stdout, $stderr) = $node1->psql(
	$db0,
	"SELECT * FROM dblink('$fdw_server', 'SELECT * FROM t') AS t(a int, b int)",
	connstr => $node1->connstr($db0) . " user=$user");

is($ret, 3, 'loopback trust fails on the same cluster');
like(
	$stderr,
	qr/failed: authentication method requirement "scram-sha-256" failed: server did not complete authentication/,
	'expected error from loopback trust (same cluster)');

($ret, $stdout, $stderr) = $node1->psql(
	$db0,
	"SELECT * FROM dblink('$fdw_server2', 'SELECT * FROM t2') AS t2(a int, b int)",
	connstr => $node1->connstr($db0) . " user=$user");

is($ret, 3, 'loopback password fails on a different cluster');
like(
	$stderr,
	qr/authentication method requirement "scram-sha-256" failed: server requested a cleartext password/,
	'expected error from loopback password (different cluster)');

# Helper functions

sub test_fdw_auth
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $db, $tbl, $fdw, $testname) = @_;
	my $connstr = $node->connstr($db) . qq' user=$user';

	my $ret = $node->safe_psql(
		$db,
		qq"SELECT count(1) FROM dblink('$fdw', 'SELECT * FROM $tbl') AS $tbl(a int, b int)",
		connstr => $connstr);

	is($ret, '10', $testname);
}

sub test_fdw_auth_with_invalid_overwritten_require_auth
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($fdw) = @_;

	my ($ret, $stdout, $stderr) = $node1->psql(
		$db0,
		"select * from dblink('$fdw', 'select * from t') as t(a int, b int)",
		connstr => $node1->connstr($db0) . " user=$user");

	is($ret, 3, 'loopback trust fails when overwriting require_auth');
	like(
		$stderr,
		qr/password or GSSAPI delegated credentials required/,
		'expected error when connecting to a fdw overwriting the require_auth'
	);
}

sub test_scram_keys_is_not_overwritten
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $db, $fdw) = @_;

	my ($ret, $stdout, $stderr) = $node->psql(
		$db,
		qq'CREATE USER MAPPING FOR $user SERVER $fdw OPTIONS (user \'$user\', scram_client_key \'key\');',
		connstr => $node->connstr($db) . " user=$user");

	is($ret, 3, 'user mapping creation fails when using scram_client_key');
	like(
		$stderr,
		qr/ERROR:  invalid option "scram_client_key"/,
		'user mapping creation fails when using scram_client_key');

	($ret, $stdout, $stderr) = $node->psql(
		$db,
		qq'CREATE USER MAPPING FOR $user SERVER $fdw OPTIONS (user \'$user\', scram_server_key \'key\');',
		connstr => $node->connstr($db) . " user=$user");

	is($ret, 3, 'user mapping creation fails when using scram_server_key');
	like(
		$stderr,
		qr/ERROR:  invalid option "scram_server_key"/,
		'user mapping creation fails when using scram_server_key');
}

sub setup_user_mapping
{
	my ($node, $db, $fdw) = @_;

	$node->safe_psql($db,
		qq'CREATE USER MAPPING FOR $user SERVER $fdw OPTIONS (user \'$user\');'
	);
}

sub setup_fdw_server
{
	my ($node, $db, $fdw, $fdw_node, $dbname) = @_;
	my $host = $fdw_node->host;
	my $port = $fdw_node->port;

	$node->safe_psql(
		$db, qq'CREATE SERVER $fdw FOREIGN DATA WRAPPER dblink_fdw options (
		host \'$host\', port \'$port\', dbname \'$dbname\', use_scram_passthrough \'true\') '
	);
	$node->safe_psql($db, qq'GRANT USAGE ON FOREIGN SERVER $fdw TO $user;');
	$node->safe_psql($db, qq'GRANT ALL ON SCHEMA public TO $user');
}

sub setup_invalid_fdw_server
{
	my ($node, $db, $fdw, $fdw_node, $dbname) = @_;
	my $host = $fdw_node->host;
	my $port = $fdw_node->port;

	$node->safe_psql(
		$db, qq'CREATE SERVER $fdw FOREIGN DATA WRAPPER dblink_fdw options (
		host \'$host\', port \'$port\', dbname \'$dbname\', use_scram_passthrough \'true\', require_auth \'none\') '
	);
	$node->safe_psql($db, qq'GRANT USAGE ON FOREIGN SERVER $fdw TO $user;');
	$node->safe_psql($db, qq'GRANT ALL ON SCHEMA public TO $user');
}

sub setup_table
{
	my ($node, $db, $tbl) = @_;

	$node->safe_psql($db,
		qq'CREATE TABLE $tbl AS SELECT g as a, g + 1 as b FROM generate_series(1,10) g(g)'
	);
	$node->safe_psql($db, qq'GRANT USAGE ON SCHEMA public TO $user');
	$node->safe_psql($db, qq'GRANT SELECT ON $tbl TO $user');
}

done_testing();

