
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Set of tests for authentication and pg_hba.conf. The following password
# methods are checked through this test:
# - Plain
# - MD5-encrypted
# - SCRAM-encrypted
# This test can only run with Unix-domain sockets.
#
# There's also a few tests of the log_connections GUC here.

use strict;
use warnings FATAL => 'all';
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
	my $node = shift;
	my $database = shift;
	my $role = shift;
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

# Test behavior of log_connections GUC
#
# There wasn't another test file where these tests obviously fit, and we don't
# want to incur the cost of spinning up a new cluster just to test one GUC.

# Make a database for the log_connections tests to avoid test fragility if
# other tests are added to this file in the future
$node->safe_psql('postgres', "CREATE DATABASE test_log_connections");

my $log_connections = $node->safe_psql('test_log_connections', q(SHOW log_connections;));
is($log_connections, 'on', qq(check log connections has expected value 'on'));

$node->connect_ok('test_log_connections',
	qq(log_connections 'on' works as expected for backwards compatibility),
	log_like => [
		qr/connection received/,
		qr/connection authenticated/,
		qr/connection authorized: user=\S+ database=test_log_connections/,
	],
	log_unlike => [
		qr/connection ready/,
	],);

$node->safe_psql('test_log_connections',
	q[ALTER SYSTEM SET log_connections = receipt,authorization,setup_durations;
				   SELECT pg_reload_conf();]);

$node->connect_ok('test_log_connections',
	q(log_connections with subset of specified options logs only those aspects),
	log_like => [
		qr/connection received/,
		qr/connection authorized: user=\S+ database=test_log_connections/,
		qr/connection ready/,
	],
	log_unlike => [
		qr/connection authenticated/,
	],);

$node->safe_psql('test_log_connections',
	qq(ALTER SYSTEM SET log_connections = 'all'; SELECT pg_reload_conf();));

$node->connect_ok('test_log_connections',
	qq(log_connections 'all' logs all available connection aspects),
	log_like => [
		qr/connection received/,
		qr/connection authenticated/,
		qr/connection authorized: user=\S+ database=test_log_connections/,
		qr/connection ready/,
	],);

# Authentication tests

# could fail in FIPS mode
my $md5_works = ($node->psql('postgres', "select md5('')") == 0);

# Create 3 roles with different password methods for each one. The same
# password is used for all of them.
is( $node->psql(
		'postgres',
		"SET password_encryption='scram-sha-256'; CREATE ROLE scram_role LOGIN PASSWORD 'pass';"
	),
	0,
	'created user with SCRAM password');
is( $node->psql(
		'postgres',
		"SET password_encryption='md5'; CREATE ROLE md5_role LOGIN PASSWORD 'pass';"
	),
	$md5_works ? 0 : 3,
	'created user with md5 password');
# Set up a table for tests of SYSTEM_USER.
$node->safe_psql(
	'postgres',
	"CREATE TABLE sysuser_data (n) AS SELECT NULL FROM generate_series(1, 10);
	 GRANT ALL ON sysuser_data TO scram_role;");
$ENV{"PGPASSWORD"} = 'pass';

# Create a role that contains a comma to stress the parsing.
$node->safe_psql('postgres',
	q{SET password_encryption='scram-sha-256'; CREATE ROLE "scram,role" LOGIN PASSWORD 'pass';}
);

# Create a role with a non-default iteration count
$node->safe_psql(
	'postgres',
	"SET password_encryption='scram-sha-256';
	 SET scram_iterations=1024;
	 CREATE ROLE scram_role_iter LOGIN PASSWORD 'pass';
	 RESET scram_iterations;"
);

my $res = $node->safe_psql(
	'postgres',
	"SELECT substr(rolpassword,1,19)
	 FROM pg_authid
	 WHERE rolname = 'scram_role_iter'");
is($res, 'SCRAM-SHA-256$1024:', 'scram_iterations in server side ROLE');

# If we don't have IO::Pty, forget it, because IPC::Run depends on that
# to support pty connections. Also skip if IPC::Run isn't at least 0.98
# as earlier version cause the session to time out.
SKIP:
{
	skip "IO::Pty and IPC::Run >= 0.98 required", 1
	  unless eval { require IO::Pty; IPC::Run->VERSION('0.98'); };

	# Alter the password on the created role using \password in psql to ensure
	# that clientside password changes use the scram_iterations value when
	# calculating SCRAM secrets.
	my $session = $node->interactive_psql('postgres');

	$session->set_query_timer_restart();
	$session->query("SET password_encryption='scram-sha-256';");
	$session->query("SET scram_iterations=42;");
	$session->query_until(qr/Enter new password/,
		"\\password scram_role_iter\n");
	$session->query_until(qr/Enter it again/, "pass\n");
	$session->query_until(qr/postgres=# /, "pass\n");
	$session->quit;

	$res = $node->safe_psql(
		'postgres',
		"SELECT substr(rolpassword,1,17)
		 FROM pg_authid
		 WHERE rolname = 'scram_role_iter'");
	is($res, 'SCRAM-SHA-256$42:',
		'scram_iterations in psql \password command');
}

# Create a database to test regular expression.
$node->safe_psql('postgres', "CREATE database regex_testdb;");

# For "trust" method, all users should be able to connect.
reset_pg_hba($node, 'all', 'all', 'trust');
test_conn($node, 'user=scram_role', 'trust', 0,
	log_like =>
	  [qr/connection authenticated: user="scram_role" method=trust/]);
SKIP:
{
	skip "MD5 not supported" unless $md5_works;
	test_conn($node, 'user=md5_role', 'trust', 0,
		log_like =>
		  [qr/connection authenticated: user="md5_role" method=trust/]);
}

# SYSTEM_USER is null when not authenticated.
$res = $node->safe_psql('postgres', "SELECT SYSTEM_USER IS NULL;");
is($res, 't', "users with trust authentication use SYSTEM_USER = NULL");

# Test SYSTEM_USER with parallel workers when not authenticated.
$res = $node->safe_psql(
	'postgres', qq(
        SET min_parallel_table_scan_size TO 0;
        SET parallel_setup_cost TO 0;
        SET parallel_tuple_cost TO 0;
        SET max_parallel_workers_per_gather TO 2;

        SELECT bool_and(SYSTEM_USER IS NOT DISTINCT FROM n) FROM sysuser_data;),
	connstr => "user=scram_role");
is($res, 't',
	"users with trust authentication use SYSTEM_USER = NULL in parallel workers"
);

# Explicitly specifying an empty require_auth (the default) should always
# succeed.
$node->connect_ok("user=scram_role require_auth=",
	"empty require_auth succeeds");

# All these values of require_auth should fail, as trust is expected.
$node->connect_fails(
	"user=scram_role require_auth=gss",
	"GSS authentication required, fails with trust auth",
	expected_stderr =>
	  qr/authentication method requirement "gss" failed: server did not complete authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=sspi",
	"SSPI authentication required, fails with trust auth",
	expected_stderr =>
	  qr/authentication method requirement "sspi" failed: server did not complete authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=password",
	"password authentication required, fails with trust auth",
	expected_stderr =>
	  qr/authentication method requirement "password" failed: server did not complete authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=md5",
	"MD5 authentication required, fails with trust auth",
	expected_stderr =>
	  qr/authentication method requirement "md5" failed: server did not complete authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=scram-sha-256",
	"SCRAM authentication required, fails with trust auth",
	expected_stderr =>
	  qr/authentication method requirement "scram-sha-256" failed: server did not complete authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=password,scram-sha-256",
	"password and SCRAM authentication required, fails with trust auth",
	expected_stderr =>
	  qr/authentication method requirement "password,scram-sha-256" failed: server did not complete authentication/
);

# These negative patterns of require_auth should succeed.
$node->connect_ok("user=scram_role require_auth=!gss",
	"GSS authentication can be forbidden, succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!sspi",
	"SSPI authentication can be forbidden, succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!password",
	"password authentication can be forbidden, succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!md5",
	"md5 authentication can be forbidden, succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!scram-sha-256",
	"SCRAM authentication can be forbidden, succeeds with trust auth");
$node->connect_ok(
	"user=scram_role require_auth=!password,!scram-sha-256",
	"multiple authentication types forbidden, succeeds with trust auth");

# require_auth=[!]none should interact correctly with trust auth.
$node->connect_ok("user=scram_role require_auth=none",
	"all authentication types forbidden, succeeds with trust auth");
$node->connect_fails(
	"user=scram_role require_auth=!none",
	"any authentication types required, fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);

# Negative and positive require_auth methods can't be mixed.
$node->connect_fails(
	"user=scram_role require_auth=scram-sha-256,!md5",
	"negative require_auth methods cannot be mixed with positive ones",
	expected_stderr =>
	  qr/negative require_auth method "!md5" cannot be mixed with non-negative methods/
);
$node->connect_fails(
	"user=scram_role require_auth=!password,!none,scram-sha-256",
	"positive require_auth methods cannot be mixed with negative one",
	expected_stderr =>
	  qr/require_auth method "scram-sha-256" cannot be mixed with negative methods/
);

# require_auth methods cannot have duplicated values.
$node->connect_fails(
	"user=scram_role require_auth=password,md5,password",
	"require_auth methods cannot include duplicates, positive case",
	expected_stderr =>
	  qr/require_auth method "password" is specified more than once/);
$node->connect_fails(
	"user=scram_role require_auth=!password,!md5,!password",
	"require_auth methods cannot be duplicated, negative case",
	expected_stderr =>
	  qr/require_auth method "!password" is specified more than once/);
$node->connect_fails(
	"user=scram_role require_auth=none,md5,none",
	"require_auth methods cannot be duplicated, none case",
	expected_stderr =>
	  qr/require_auth method "none" is specified more than once/);
$node->connect_fails(
	"user=scram_role require_auth=!none,!md5,!none",
	"require_auth methods cannot be duplicated, !none case",
	expected_stderr =>
	  qr/require_auth method "!none" is specified more than once/);
$node->connect_fails(
	"user=scram_role require_auth=scram-sha-256,scram-sha-256",
	"require_auth methods cannot be duplicated, scram-sha-256 case",
	expected_stderr =>
	  qr/require_auth method "scram-sha-256" is specified more than once/);
$node->connect_fails(
	"user=scram_role require_auth=!scram-sha-256,!scram-sha-256",
	"require_auth methods cannot be duplicated, !scram-sha-256 case",
	expected_stderr =>
	  qr/require_auth method "!scram-sha-256" is specified more than once/);

# Unknown value defined in require_auth.
$node->connect_fails(
	"user=scram_role require_auth=none,abcdefg",
	"unknown require_auth methods are rejected",
	expected_stderr => qr/invalid require_auth value: "abcdefg"/);

# For plain "password" method, all users should also be able to connect.
reset_pg_hba($node, 'all', 'all', 'password');
test_conn($node, 'user=scram_role', 'password', 0,
	log_like =>
	  [qr/connection authenticated: identity="scram_role" method=password/]);
SKIP:
{
	skip "MD5 not supported" unless $md5_works;
	test_conn($node, 'user=md5_role', 'password', 0,
		log_like =>
		  [qr/connection authenticated: identity="md5_role" method=password/]
	);
}

# require_auth succeeds here with a plaintext password.
$node->connect_ok("user=scram_role require_auth=password",
	"password authentication required, works with password auth");
$node->connect_ok("user=scram_role require_auth=!none",
	"any authentication required, works with password auth");
$node->connect_ok(
	"user=scram_role require_auth=scram-sha-256,password,md5",
	"multiple authentication types required, works with password auth");

# require_auth fails for other authentication types.
$node->connect_fails(
	"user=scram_role require_auth=md5",
	"md5 authentication required, fails with password auth",
	expected_stderr =>
	  qr/authentication method requirement "md5" failed: server requested a cleartext password/
);
$node->connect_fails(
	"user=scram_role require_auth=scram-sha-256",
	"SCRAM authentication required, fails with password auth",
	expected_stderr =>
	  qr/authentication method requirement "scram-sha-256" failed: server requested a cleartext password/
);
$node->connect_fails(
	"user=scram_role require_auth=none",
	"all authentication forbidden, fails with password auth",
	expected_stderr =>
	  qr/authentication method requirement "none" failed: server requested a cleartext password/
);

# Disallowing password authentication fails, even if requested by server.
$node->connect_fails(
	"user=scram_role require_auth=!password",
	"password authentication forbidden, fails with password auth",
	expected_stderr => qr/server requested a cleartext password/);
$node->connect_fails(
	"user=scram_role require_auth=!password,!md5,!scram-sha-256",
	"multiple authentication types forbidden, fails with password auth",
	expected_stderr =>
	  qr/ method requirement "!password,!md5,!scram-sha-256" failed: server requested a cleartext password/
);

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
test_conn(
	$node,
	'user=scram_role_iter',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="scram_role_iter" method=scram-sha-256/
	]);
test_conn($node, 'user=md5_role', 'scram-sha-256', 2,
	log_unlike => [qr/connection authenticated:/]);

# require_auth should succeed with SCRAM when it is required.
$node->connect_ok(
	"user=scram_role require_auth=scram-sha-256",
	"SCRAM authentication required, works with SCRAM auth");
$node->connect_ok("user=scram_role require_auth=!none",
	"any authentication required, works with SCRAM auth");
$node->connect_ok(
	"user=scram_role require_auth=password,scram-sha-256,md5",
	"multiple authentication types required, works with SCRAM auth");

# Authentication fails for other authentication types.
$node->connect_fails(
	"user=scram_role require_auth=password",
	"password authentication required, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "password" failed: server requested SASL authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=md5",
	"md5 authentication required, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "md5" failed: server requested SASL authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=none",
	"all authentication forbidden, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "none" failed: server requested SASL authentication/
);

# Authentication fails if SCRAM authentication is forbidden.
$node->connect_fails(
	"user=scram_role require_auth=!scram-sha-256",
	"SCRAM authentication forbidden, fails with SCRAM auth",
	expected_stderr => qr/server requested SCRAM-SHA-256 authentication/);
$node->connect_fails(
	"user=scram_role require_auth=!password,!md5,!scram-sha-256",
	"multiple authentication types forbidden, fails with SCRAM auth",
	expected_stderr => qr/server requested SCRAM-SHA-256 authentication/);

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
SKIP:
{
	skip "MD5 not supported" unless $md5_works;
	test_conn($node, 'user=md5_role', 'md5', 0,
		log_like =>
		  [qr/connection authenticated: identity="md5_role" method=md5/]);
}

# require_auth succeeds with SCRAM required.
$node->connect_ok(
	"user=scram_role require_auth=scram-sha-256",
	"SCRAM authentication required, works with SCRAM auth");
$node->connect_ok("user=scram_role require_auth=!none",
	"any authentication required, works with SCRAM auth");
$node->connect_ok(
	"user=scram_role require_auth=md5,scram-sha-256,password",
	"multiple authentication types required, works with SCRAM auth");

# Authentication fails if other types are required.
$node->connect_fails(
	"user=scram_role require_auth=password",
	"password authentication required, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "password" failed: server requested SASL authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=md5",
	"MD5 authentication required, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "md5" failed: server requested SASL authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=none",
	"all authentication types forbidden, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "none" failed: server requested SASL authentication/
);

# Authentication fails if SCRAM is forbidden.
$node->connect_fails(
	"user=scram_role require_auth=!scram-sha-256",
	"password authentication forbidden, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "!scram-sha-256" failed: server requested SCRAM-SHA-256 authentication/
);
$node->connect_fails(
	"user=scram_role require_auth=!password,!md5,!scram-sha-256",
	"multiple authentication types forbidden, fails with SCRAM auth",
	expected_stderr =>
	  qr/authentication method requirement "!password,!md5,!scram-sha-256" failed: server requested SCRAM-SHA-256 authentication/
);

# Test SYSTEM_USER <> NULL with parallel workers.
$node->safe_psql(
	'postgres',
	"TRUNCATE sysuser_data;
INSERT INTO sysuser_data SELECT 'md5:scram_role' FROM generate_series(1, 10);",
	connstr => "user=scram_role");
$res = $node->safe_psql(
	'postgres', qq(
        SET min_parallel_table_scan_size TO 0;
        SET parallel_setup_cost TO 0;
        SET parallel_tuple_cost TO 0;
        SET max_parallel_workers_per_gather TO 2;

        SELECT bool_and(SYSTEM_USER IS NOT DISTINCT FROM n) FROM sysuser_data;),
	connstr => "user=scram_role");
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
test_conn($node, 'user=md5_role', 'password from pgpass', 2);

append_to_file(
	$pgpassfile, qq!
*:*:*:scram_role:p\\ass
*:*:*:scram,role:p\\ass
!);

test_conn($node, 'user=scram_role', 'password from pgpass', 0);

# Testing with regular expression for username.  The third regexp matches.
reset_pg_hba($node, 'all', '/^.*nomatch.*$, baduser, /^scr.*$', 'password');
test_conn(
	$node,
	'user=scram_role',
	'password, matching regexp for username',
	0,
	log_like =>
	  [qr/connection authenticated: identity="scram_role" method=password/]);

# The third regex does not match anymore.
reset_pg_hba($node, 'all', '/^.*nomatch.*$, baduser, /^sc_r.*$', 'password');
test_conn($node, 'user=scram_role',
	'password, non matching regexp for username',
	2, log_unlike => [qr/connection authenticated:/]);

# Test with a comma in the regular expression.  In this case, the use of
# double quotes is mandatory so as this is not considered as two elements
# of the user name list when parsing pg_hba.conf.
reset_pg_hba($node, 'all', '"/^.*m,.*e$"', 'password');
test_conn(
	$node,
	'user=scram,role',
	'password, matching regexp for username',
	0,
	log_like =>
	  [qr/connection authenticated: identity="scram,role" method=password/]);

# Testing with regular expression for dbname. The third regex matches.
reset_pg_hba($node, '/^.*nomatch.*$, baddb, /^regex_t.*b$', 'all',
	'password');
test_conn(
	$node,
	'user=scram_role dbname=regex_testdb',
	'password, matching regexp for dbname',
	0,
	log_like =>
	  [qr/connection authenticated: identity="scram_role" method=password/]);

# The third regexp does not match anymore.
reset_pg_hba($node, '/^.*nomatch.*$, baddb, /^regex_t.*ba$',
	'all', 'password');
test_conn(
	$node,
	'user=scram_role dbname=regex_testdb',
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
