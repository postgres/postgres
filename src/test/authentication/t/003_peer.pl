
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

# Tests for peer authentication and user name map.
# The test is skipped if the platform does not support peer authentication,
# and is only able to run with Unix-domain sockets.

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
	$node->append_conf('pg_hba.conf', "local all all $hba_method");
	$node->reload;
	return;
}

# Delete pg_ident.conf from the given node, add a new entry to it
# and then execute a reload to refresh it.
sub reset_pg_ident
{
	my $node        = shift;
	my $map_name    = shift;
	my $system_user = shift;
	my $pg_user     = shift;

	unlink($node->data_dir . '/pg_ident.conf');
	$node->append_conf('pg_ident.conf', "$map_name $system_user $pg_user");
	$node->reload;
	return;
}

# Test access for a single role, useful to wrap all tests into one.
sub test_role
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $method, $expected_res, $test_details, %params) = @_;
	my $status_string = 'failed';
	$status_string = 'success' if ($expected_res eq 0);

	my $connstr = "user=$role";
	my $testname =
	  "authentication $status_string for method $method, role $role "
	  . $test_details;

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

# Find $pattern in log file of $node.
sub find_in_log
{
	my ($node, $offset, $pattern) = @_;

	my $log = PostgreSQL::Test::Utils::slurp_file($node->logfile, $offset);
	return 0 if (length($log) <= 0);

	return $log =~ m/$pattern/;
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->start;

# Set pg_hba.conf with the peer authentication.
reset_pg_hba($node, 'peer');

# Check if peer authentication is supported on this platform.
my $log_offset = -s $node->logfile;
$node->psql('postgres');
if (find_in_log(
		$node, $log_offset,
		qr/peer authentication is not supported on this platform/))
{
	plan skip_all => 'peer authentication is not supported on this platform';
}

# Add a database role, to use for the user name map.
$node->safe_psql('postgres', qq{CREATE ROLE testmapuser LOGIN});

# Extract as well the system user for the user name map.
my $system_user =
  $node->safe_psql('postgres',
	q(select (string_to_array(SYSTEM_USER, ':'))[2]));

# Tests without the user name map.
# Failure as connection is attempted with a database role not mapping
# to an authorized system user.
test_role(
	$node, qq{testmapuser}, 'peer', 2,
	'without user name map',
	log_like => [qr/Peer authentication failed for user "testmapuser"/]);

# Tests with a user name map.
reset_pg_ident($node, 'mypeermap', $system_user, 'testmapuser');
reset_pg_hba($node, 'peer map=mypeermap');

# Success as the database role matches with the system user in the map.
test_role($node, qq{testmapuser}, 'peer', 0, 'with user name map',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Test with regular expression in user name map.
# Extract the last 3 characters from the system_user
# or the entire system_user (if its length is <= -3).
my $regex_test_string = substr($system_user, -3);

# Success as the regular expression matches.
reset_pg_ident($node, 'mypeermap', qq{/^.*$regex_test_string\$},
	'testmapuser');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with regular expression in user name map',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);


# Concatenate system_user to system_user.
$regex_test_string = $system_user . $system_user;

# Failure as the regular expression does not match.
reset_pg_ident($node, 'mypeermap', qq{/^.*$regex_test_string\$},
	'testmapuser');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	2,
	'with regular expression in user name map',
	log_like => [qr/no match in usermap "mypeermap" for user "testmapuser"/]);

done_testing();
