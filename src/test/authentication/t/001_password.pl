
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

# Set of tests for authentication and pg_hba.conf. The following password
# methods are checked through this test:
# - Plain
# - MD5-encrypted
# - SCRAM-encrypted
# This test can only run with Unix-domain sockets.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
if (!$use_unix_sockets)
{
	plan skip_all =>
	  "authentication tests cannot run without Unix-domain sockets";
}

# Delete pg_hba.conf from the given node, add a new entry to it
# and then execute a reload to refresh it.
sub reset_pg_hba
{
	my $node       = shift;
	my $database   = shift;
	my $role       = shift;
	my $hba_method = shift;

	unlink($node->data_dir . '/pg_hba.conf');
	# just for testing purposes, use a continuation line
	$node->append_conf('pg_hba.conf',
		"local $database $role\\\n $hba_method");
	$node->reload;
	return;
}

# Test access for a connection string, useful to wrap all tests into one.
# Extra named parameters are passed to connect_ok/fails as-is.
sub test_conn
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $connstr, $method, $expected_res, %params) = @_;
	my $status_string = 'failed';
	$status_string = 'success' if ($expected_res eq 0);

	my $testname =
	  "authentication $status_string for method $method, connstr $connstr";

	if ($expected_res eq 0)
	{
		$node->connect_ok($connstr, $testname, %params);
	}
	else
	{
		# No checks of the error message, only the status code.
		$node->connect_fails($connstr, $testname, %params);
	}
}

# Initialize primary node
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->start;

# Create 3 roles with different password methods for each one. The same
# password is used for all of them.
$node->safe_psql('postgres',
	"SET password_encryption='scram-sha-256'; CREATE ROLE scram_role LOGIN PASSWORD 'pass';"
);
$node->safe_psql('postgres',
	"SET password_encryption='md5'; CREATE ROLE md5_role LOGIN PASSWORD 'pass';"
);
# Set up a table for tests of SYSTEM_USER.
$node->safe_psql(
	'postgres',
	"CREATE TABLE sysuser_data (n) AS SELECT NULL FROM generate_series(1, 10);
	 GRANT ALL ON sysuser_data TO md5_role;");
$ENV{"PGPASSWORD"} = 'pass';

# Create a role that contains a comma to stress the parsing.
$node->safe_psql('postgres',
	q{SET password_encryption='md5'; CREATE ROLE "md5,role" LOGIN PASSWORD 'pass';}
);

# Create a database to test regular expression.
$node->safe_psql('postgres', "CREATE database regex_testdb;");

# For "trust" method, all users should be able to connect. These users are not
# considered to be authenticated.
reset_pg_hba($node, 'all', 'all', 'trust');
test_conn($node, 'user=scram_role', 'trust', 0,
	log_unlike => [qr/connection authenticated:/]);
test_conn($node, 'user=md5_role', 'trust', 0,
	log_unlike => [qr/connection authenticated:/]);

# SYSTEM_USER is null when not authenticated.
my $res = $node->safe_psql('postgres', "SELECT SYSTEM_USER IS NULL;");
is($res, 't', "users with trust authentication use SYSTEM_USER = NULL");

# Test SYSTEM_USER with parallel workers when not authenticated.
$res = $node->safe_psql(
	'postgres', qq(
        SET min_parallel_table_scan_size TO 0;
        SET parallel_setup_cost TO 0;
        SET parallel_tuple_cost TO 0;
        SET max_parallel_workers_per_gather TO 2;

        SELECT bool_and(SYSTEM_USER IS NOT DISTINCT FROM n) FROM sysuser_data;),
	connstr => "user=md5_role");
is($res, 't',
	"users with trust authentication use SYSTEM_USER = NULL in parallel workers"
);

# For plain "password" method, all users should also be able to connect.
reset_pg_hba($node, 'all', 'all', 'password');
test_conn($node, 'user=scram_role', 'password', 0,
	log_like =>
	  [qr/connection authenticated: identity="scram_role" method=password/]);
test_conn($node, 'user=md5_role', 'password', 0,
	log_like =>
	  [qr/connection authenticated: identity="md5_role" method=password/]);

# For "scram-sha-256" method, user "scram_role" should be able to connect.
reset_pg_hba($node, 'all', 'all', 'scram-sha-256');
test_conn(
	$node,
	'user=scram_role',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="scram_role" method=scram-sha-256/
	]);
test_conn($node, 'user=md5_role', 'scram-sha-256', 2,
	log_unlike => [qr/connection authenticated:/]);

# Test that bad passwords are rejected.
$ENV{"PGPASSWORD"} = 'badpass';
test_conn($node, 'user=scram_role', 'scram-sha-256', 2,
	log_unlike => [qr/connection authenticated:/]);
$ENV{"PGPASSWORD"} = 'pass';

# For "md5" method, all users should be able to connect (SCRAM
# authentication will be performed for the user with a SCRAM secret.)
reset_pg_hba($node, 'all', 'all', 'md5');
test_conn($node, 'user=scram_role', 'md5', 0,
	log_like =>
	  [qr/connection authenticated: identity="scram_role" method=md5/]);
test_conn($node, 'user=md5_role', 'md5', 0,
	log_like =>
	  [qr/connection authenticated: identity="md5_role" method=md5/]);

# Test SYSTEM_USER <> NULL with parallel workers.
$node->safe_psql(
	'postgres',
	"TRUNCATE sysuser_data;
INSERT INTO sysuser_data SELECT 'md5:md5_role' FROM generate_series(1, 10);",
	connstr => "user=md5_role");
$res = $node->safe_psql(
	'postgres', qq(
        SET min_parallel_table_scan_size TO 0;
        SET parallel_setup_cost TO 0;
        SET parallel_tuple_cost TO 0;
        SET max_parallel_workers_per_gather TO 2;

        SELECT bool_and(SYSTEM_USER IS NOT DISTINCT FROM n) FROM sysuser_data;),
	connstr => "user=md5_role");
is($res, 't',
	"users with md5 authentication use SYSTEM_USER = md5:role in parallel workers"
);

# Tests for channel binding without SSL.
# Using the password authentication method; channel binding can't work
reset_pg_hba($node, 'all', 'all', 'password');
$ENV{"PGCHANNELBINDING"} = 'require';
test_conn($node, 'user=scram_role', 'scram-sha-256', 2);
# SSL not in use; channel binding still can't work
reset_pg_hba($node, 'all', 'all', 'scram-sha-256');
$ENV{"PGCHANNELBINDING"} = 'require';
test_conn($node, 'user=scram_role', 'scram-sha-256', 2);

# Test .pgpass processing; but use a temp file, don't overwrite the real one!
my $pgpassfile = "${PostgreSQL::Test::Utils::tmp_check}/pgpass";

delete $ENV{"PGPASSWORD"};
delete $ENV{"PGCHANNELBINDING"};
$ENV{"PGPASSFILE"} = $pgpassfile;

unlink($pgpassfile);
append_to_file(
	$pgpassfile, qq!
# This very long comment is just here to exercise handling of long lines in the file. This very long comment is just here to exercise handling of long lines in the file. This very long comment is just here to exercise handling of long lines in the file. This very long comment is just here to exercise handling of long lines in the file. This very long comment is just here to exercise handling of long lines in the file.
*:*:postgres:scram_role:pass:this is not part of the password.
!);
chmod 0600, $pgpassfile or die;

reset_pg_hba($node, 'all', 'all', 'password');
test_conn($node, 'user=scram_role', 'password from pgpass', 0);
test_conn($node, 'user=md5_role',   'password from pgpass', 2);

append_to_file(
	$pgpassfile, qq!
*:*:*:md5_role:p\\ass
*:*:*:md5,role:p\\ass
!);

test_conn($node, 'user=md5_role', 'password from pgpass', 0);

# Testing with regular expression for username.  The third regexp matches.
reset_pg_hba($node, 'all', '/^.*nomatch.*$, baduser, /^md.*$', 'password');
test_conn($node, 'user=md5_role', 'password, matching regexp for username', 0,
	log_like =>
	  [qr/connection authenticated: identity="md5_role" method=password/]);

# The third regex does not match anymore.
reset_pg_hba($node, 'all', '/^.*nomatch.*$, baduser, /^m_d.*$', 'password');
test_conn($node, 'user=md5_role',
	'password, non matching regexp for username',
	2, log_unlike => [qr/connection authenticated:/]);

# Test with a comma in the regular expression.  In this case, the use of
# double quotes is mandatory so as this is not considered as two elements
# of the user name list when parsing pg_hba.conf.
reset_pg_hba($node, 'all', '"/^.*5,.*e$"', 'password');
test_conn($node, 'user=md5,role', 'password, matching regexp for username', 0,
	log_like =>
	  [qr/connection authenticated: identity="md5,role" method=password/]);

# Testing with regular expression for dbname. The third regex matches.
reset_pg_hba($node, '/^.*nomatch.*$, baddb, /^regex_t.*b$', 'all',
	'password');
test_conn(
	$node,
	'user=md5_role dbname=regex_testdb',
	'password, matching regexp for dbname',
	0,
	log_like =>
	  [qr/connection authenticated: identity="md5_role" method=password/]);

# The third regexp does not match anymore.
reset_pg_hba($node, '/^.*nomatch.*$, baddb, /^regex_t.*ba$',
	'all', 'password');
test_conn(
	$node,
	'user=md5_role dbname=regex_testdb',
	'password, non matching regexp for dbname',
	2, log_unlike => [qr/connection authenticated:/]);

unlink($pgpassfile);
delete $ENV{"PGPASSFILE"};

note "Authentication tests with specific HBA policies on roles";

# Create database and roles for membership tests
reset_pg_hba($node, 'all', 'all', 'trust');
# Database and root role names match for "samerole" and "samegroup".
$node->safe_psql('postgres', "CREATE DATABASE regress_regression_group;");
$node->safe_psql(
	'postgres',
	qq{CREATE ROLE regress_regression_group LOGIN PASSWORD 'pass';
CREATE ROLE regress_member LOGIN SUPERUSER IN ROLE regress_regression_group PASSWORD 'pass';
CREATE ROLE regress_not_member LOGIN SUPERUSER PASSWORD 'pass';});

# Test role with exact matching, no members allowed.
$ENV{"PGPASSWORD"} = 'pass';
reset_pg_hba($node, 'all', 'regress_regression_group', 'scram-sha-256');
test_conn(
	$node,
	'user=regress_regression_group',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="regress_regression_group" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_member',
	'scram-sha-256',
	2,
	log_unlike => [
		qr/connection authenticated: identity="regress_member" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_not_member',
	'scram-sha-256',
	2,
	log_unlike => [
		qr/connection authenticated: identity="regress_not_member" method=scram-sha-256/
	]);

# Test role membership with '+', where all the members are allowed
# to connect.
reset_pg_hba($node, 'all', '+regress_regression_group', 'scram-sha-256');
test_conn(
	$node,
	'user=regress_regression_group',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="regress_regression_group" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_member',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="regress_member" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_not_member',
	'scram-sha-256',
	2,
	log_unlike => [
		qr/connection authenticated: identity="regress_not_member" method=scram-sha-256/
	]);

# Test role membership is respected for samerole
$ENV{"PGDATABASE"} = 'regress_regression_group';
reset_pg_hba($node, 'samerole', 'all', 'scram-sha-256');
test_conn(
	$node,
	'user=regress_regression_group',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="regress_regression_group" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_member',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="regress_member" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_not_member',
	'scram-sha-256',
	2,
	log_unlike => [
		qr/connection authenticated: identity="regress_not_member" method=scram-sha-256/
	]);

# Test role membership is respected for samegroup
reset_pg_hba($node, 'samegroup', 'all', 'scram-sha-256');
test_conn(
	$node,
	'user=regress_regression_group',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="regress_regression_group" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_member',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="regress_member" method=scram-sha-256/
	]);
test_conn(
	$node,
	'user=regress_not_member',
	'scram-sha-256',
	2,
	log_unlike => [
		qr/connection authenticated: identity="regress_not_member" method=scram-sha-256/
	]);

done_testing();
