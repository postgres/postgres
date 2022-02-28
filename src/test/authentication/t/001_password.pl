
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

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
	my $hba_method = shift;

	unlink($node->data_dir . '/pg_hba.conf');
	# just for testing purposes, use a continuation line
	$node->append_conf('pg_hba.conf', "local all all\\\n $hba_method");
	$node->reload;
	return;
}

# Test access for a single role, useful to wrap all tests into one.  Extra
# named parameters are passed to connect_ok/fails as-is.
sub test_role
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $method, $expected_res, %params) = @_;
	my $status_string = 'failed';
	$status_string = 'success' if ($expected_res eq 0);

	my $connstr = "user=$role";
	my $testname =
	  "authentication $status_string for method $method, role $role";

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
$ENV{"PGPASSWORD"} = 'pass';

# For "trust" method, all users should be able to connect. These users are not
# considered to be authenticated.
reset_pg_hba($node, 'trust');
test_role($node, 'scram_role', 'trust', 0,
	log_unlike => [qr/connection authenticated:/]);
test_role($node, 'md5_role', 'trust', 0,
	log_unlike => [qr/connection authenticated:/]);

# All positive require_auth options should fail...
$node->connect_fails("user=scram_role require_auth=gss",
	"GSS authentication can be required: fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);
$node->connect_fails("user=scram_role require_auth=sspi",
	"SSPI authentication can be required: fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);
$node->connect_fails("user=scram_role require_auth=password",
	"password authentication can be required: fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);
$node->connect_fails("user=scram_role require_auth=md5",
	"md5 authentication can be required: fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);
$node->connect_fails("user=scram_role require_auth=scram-sha-256",
	"SCRAM authentication can be required: fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);
$node->connect_fails("user=scram_role require_auth=password,scram-sha-256",
	"multiple authentication types can be required: fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);

# ...and negative require_auth options should succeed.
$node->connect_ok("user=scram_role require_auth=!gss",
	"GSS authentication can be forbidden: succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!sspi",
	"SSPI authentication can be forbidden: succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!password",
	"password authentication can be forbidden: succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!md5",
	"md5 authentication can be forbidden: succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!scram-sha-256",
	"SCRAM authentication can be forbidden: succeeds with trust auth");
$node->connect_ok("user=scram_role require_auth=!password,!scram-sha-256",
	"multiple authentication types can be forbidden: succeeds with trust auth");

# require_auth=[!]none should interact correctly with trust auth.
$node->connect_ok("user=scram_role require_auth=none",
	"all authentication can be forbidden: succeeds with trust auth");
$node->connect_fails("user=scram_role require_auth=!none",
	"any authentication can be required: fails with trust auth",
	expected_stderr => qr/server did not complete authentication/);

# Negative and positive require_auth options can't be mixed.
$node->connect_fails("user=scram_role require_auth=scram-sha-256,!md5",
	"negative require_auth methods can't be mixed with positive",
	expected_stderr => qr/negative require_auth method "!md5" cannot be mixed with non-negative methods/);
$node->connect_fails("user=scram_role require_auth=!password,scram-sha-256",
	"positive require_auth methods can't be mixed with negative",
	expected_stderr => qr/require_auth method "scram-sha-256" cannot be mixed with negative methods/);

# require_auth methods can't be duplicated.
$node->connect_fails("user=scram_role require_auth=password,md5,password",
	"require_auth methods can't be duplicated: positive case",
	expected_stderr => qr/require_auth method "password" is specified more than once/);
$node->connect_fails("user=scram_role require_auth=!password,!md5,!password",
	"require_auth methods can't be duplicated: negative case",
	expected_stderr => qr/require_auth method "!password" is specified more than once/);
$node->connect_fails("user=scram_role require_auth=none,md5,none",
	"require_auth methods can't be duplicated: none case",
	expected_stderr => qr/require_auth method "none" is specified more than once/);
$node->connect_fails("user=scram_role require_auth=!none,!md5,!none",
	"require_auth methods can't be duplicated: !none case",
	expected_stderr => qr/require_auth method "!none" is specified more than once/);

# Unknown require_auth methods are caught.
$node->connect_fails("user=scram_role require_auth=none,abcdefg",
	"unknown require_auth methods are rejected",
	expected_stderr => qr/invalid require_auth method: "abcdefg"/);

# For plain "password" method, all users should also be able to connect.
reset_pg_hba($node, 'password');
test_role($node, 'scram_role', 'password', 0,
	log_like =>
	  [qr/connection authenticated: identity="scram_role" method=password/]);
test_role($node, 'md5_role', 'password', 0,
	log_like =>
	  [qr/connection authenticated: identity="md5_role" method=password/]);

# require_auth should succeed with a plaintext password...
$node->connect_ok("user=scram_role require_auth=password",
	"password authentication can be required: works with password auth");
$node->connect_ok("user=scram_role require_auth=!none",
	"any authentication can be required: works with password auth");
$node->connect_ok("user=scram_role require_auth=scram-sha-256,password,md5",
	"multiple authentication types can be required: works with password auth");

# ...fail for other auth types...
$node->connect_fails("user=scram_role require_auth=md5",
	"md5 authentication can be required: fails with password auth",
	expected_stderr => qr/server requested a cleartext password/);
$node->connect_fails("user=scram_role require_auth=scram-sha-256",
	"SCRAM authentication can be required: fails with password auth",
	expected_stderr => qr/server requested a cleartext password/);
$node->connect_fails("user=scram_role require_auth=none",
	"all authentication can be forbidden: fails with password auth",
	expected_stderr => qr/server requested a cleartext password/);

# ...and allow password authentication to be prohibited.
$node->connect_fails("user=scram_role require_auth=!password",
	"password authentication can be forbidden: fails with password auth",
	expected_stderr => qr/server requested a cleartext password/);
$node->connect_fails("user=scram_role require_auth=!password,!md5,!scram-sha-256",
	"multiple authentication types can be forbidden: fails with password auth",
	expected_stderr => qr/server requested a cleartext password/);

# For "scram-sha-256" method, user "scram_role" should be able to connect.
reset_pg_hba($node, 'scram-sha-256');
test_role(
	$node,
	'scram_role',
	'scram-sha-256',
	0,
	log_like => [
		qr/connection authenticated: identity="scram_role" method=scram-sha-256/
	]);
test_role($node, 'md5_role', 'scram-sha-256', 2,
	log_unlike => [qr/connection authenticated:/]);

# require_auth should succeed with SCRAM...
$node->connect_ok("user=scram_role require_auth=scram-sha-256",
	"SCRAM authentication can be required: works with SCRAM auth");
$node->connect_ok("user=scram_role require_auth=!none",
	"any authentication can be required: works with SCRAM auth");
$node->connect_ok("user=scram_role require_auth=password,scram-sha-256,md5",
	"multiple authentication types can be required: works with SCRAM auth");

# ...fail for other auth types...
$node->connect_fails("user=scram_role require_auth=password",
	"password authentication can be required: fails with SCRAM auth",
	expected_stderr => qr/server requested SASL authentication/);
$node->connect_fails("user=scram_role require_auth=md5",
	"md5 authentication can be required: fails with SCRAM auth",
	expected_stderr => qr/server requested SASL authentication/);
$node->connect_fails("user=scram_role require_auth=none",
	"all authentication can be forbidden: fails with SCRAM auth",
	expected_stderr => qr/server requested SASL authentication/);

# ...and allow SCRAM authentication to be prohibited.
$node->connect_fails("user=scram_role require_auth=!scram-sha-256",
	"SCRAM authentication can be forbidden: fails with SCRAM auth",
	expected_stderr => qr/server requested SASL authentication/);
$node->connect_fails("user=scram_role require_auth=!password,!md5,!scram-sha-256",
	"multiple authentication types can be forbidden: fails with SCRAM auth",
	expected_stderr => qr/server requested SASL authentication/);

# Test that bad passwords are rejected.
$ENV{"PGPASSWORD"} = 'badpass';
test_role($node, 'scram_role', 'scram-sha-256', 2,
	log_unlike => [qr/connection authenticated:/]);
$ENV{"PGPASSWORD"} = 'pass';

# For "md5" method, all users should be able to connect (SCRAM
# authentication will be performed for the user with a SCRAM secret.)
reset_pg_hba($node, 'md5');
test_role($node, 'scram_role', 'md5', 0,
	log_like =>
	  [qr/connection authenticated: identity="scram_role" method=md5/]);
test_role($node, 'md5_role', 'md5', 0,
	log_like =>
	  [qr/connection authenticated: identity="md5_role" method=md5/]);

# require_auth should succeed with MD5...
$node->connect_ok("user=md5_role require_auth=md5",
	"MD5 authentication can be required: works with MD5 auth");
$node->connect_ok("user=md5_role require_auth=!none",
	"any authentication can be required: works with MD5 auth");
$node->connect_ok("user=md5_role require_auth=md5,scram-sha-256,password",
	"multiple authentication types can be required: works with MD5 auth");

# ...fail for other auth types...
$node->connect_fails("user=md5_role require_auth=password",
	"password authentication can be required: fails with MD5 auth",
	expected_stderr => qr/server requested a hashed password/);
$node->connect_fails("user=md5_role require_auth=scram-sha-256",
	"SCRAM authentication can be required: fails with MD5 auth",
	expected_stderr => qr/server requested a hashed password/);
$node->connect_fails("user=md5_role require_auth=none",
	"all authentication can be forbidden: fails with MD5 auth",
	expected_stderr => qr/server requested a hashed password/);

# ...and allow MD5 authentication to be prohibited.
$node->connect_fails("user=md5_role require_auth=!md5",
	"password authentication can be forbidden: fails with MD5 auth",
	expected_stderr => qr/server requested a hashed password/);
$node->connect_fails("user=md5_role require_auth=!password,!md5,!scram-sha-256",
	"multiple authentication types can be forbidden: fails with MD5 auth",
	expected_stderr => qr/server requested a hashed password/);

# Tests for channel binding without SSL.
# Using the password authentication method; channel binding can't work
reset_pg_hba($node, 'password');
$ENV{"PGCHANNELBINDING"} = 'require';
test_role($node, 'scram_role', 'scram-sha-256', 2);
# SSL not in use; channel binding still can't work
reset_pg_hba($node, 'scram-sha-256');
$ENV{"PGCHANNELBINDING"} = 'require';
test_role($node, 'scram_role', 'scram-sha-256', 2);

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

reset_pg_hba($node, 'password');
test_role($node, 'scram_role', 'password from pgpass', 0);
test_role($node, 'md5_role',   'password from pgpass', 2);

append_to_file(
	$pgpassfile, qq!
*:*:*:md5_role:p\\ass
!);

test_role($node, 'md5_role', 'password from pgpass', 0);

done_testing();
