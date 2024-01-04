
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

# Tests for peer authentication and user name map.
# The test is skipped if the platform does not support peer authentication,
# and is only able to run with Unix-domain sockets.

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
	my $node = shift;
	my $map_name = shift;
	my $system_user = shift;
	my $pg_user = shift;

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

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->start;

# Set pg_hba.conf with the peer authentication.
reset_pg_hba($node, 'peer');

# Check if peer authentication is supported on this platform.
my $log_offset = -s $node->logfile;
$node->psql('postgres');
if ($node->log_contains(
		qr/peer authentication is not supported on this platform/,
		$log_offset))
{
	plan skip_all => 'peer authentication is not supported on this platform';
}

# Add a database role and a group, to use for the user name map.
$node->safe_psql('postgres', qq{CREATE ROLE testmapuser LOGIN});
$node->safe_psql('postgres', "CREATE ROLE testmapgroup NOLOGIN");
$node->safe_psql('postgres', "GRANT testmapgroup TO testmapuser");
# Note the double quotes here.
$node->safe_psql('postgres', 'CREATE ROLE "testmapgroupliteral\\1" LOGIN');
$node->safe_psql('postgres', 'GRANT "testmapgroupliteral\\1" TO testmapuser');

# Extract as well the system user for the user name map.
my $system_user =
  $node->safe_psql('postgres',
	q(select (string_to_array(SYSTEM_USER, ':'))[2]));

# While on it, check the status of huge pages, that can be either on
# or off, but never unknown.
my $huge_pages_status =
  $node->safe_psql('postgres', q(SHOW huge_pages_status;));
isnt($huge_pages_status, 'unknown', "check huge_pages_status");

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

# Tests with the "all" keyword.
reset_pg_ident($node, 'mypeermap', $system_user, 'all');

test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with keyword "all" as database user in user name map',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Tests with the "all" keyword, but quoted (no effect here).
reset_pg_ident($node, 'mypeermap', $system_user, '"all"');

test_role(
	$node,
	qq{testmapuser},
	'peer',
	2,
	'with quoted keyword "all" as database user in user name map',
	log_like => [qr/no match in usermap "mypeermap" for user "testmapuser"/]);

# Success as the regexp of the database user matches
reset_pg_ident($node, 'mypeermap', $system_user, qq{/^testm.*\$});
test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with regexp of database user in user name map',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Failure as the regexp of the database user does not match.
reset_pg_ident($node, 'mypeermap', $system_user, qq{/^doesnotmatch.*\$});
test_role(
	$node,
	qq{testmapuser},
	'peer',
	2,
	'with bad regexp of database user in user name map',
	log_like => [qr/no match in usermap "mypeermap" for user "testmapuser"/]);

# Test with regular expression in user name map.
# Extract the last 3 characters from the system_user
# or the entire system_user (if its length is <= -3).
my $regex_test_string = substr($system_user, -3);

# Success as the system user regular expression matches.
reset_pg_ident($node, 'mypeermap', qq{/^.*$regex_test_string\$},
	'testmapuser');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with regexp of system user in user name map',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Success as both regular expressions match.
reset_pg_ident($node, 'mypeermap', qq{/^.*$regex_test_string\$},
	qq{/^testm.*\$});
test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with regexps for both system and database user in user name map',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Success as the regular expression matches and database role is the "all"
# keyword.
reset_pg_ident($node, 'mypeermap', qq{/^.*$regex_test_string\$}, 'all');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with regexp of system user and keyword "all" in user name map',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Success as the regular expression matches and \1 is replaced in the given
# subexpression.
reset_pg_ident($node, 'mypeermap', qq{/^$system_user(.*)\$}, 'test\1mapuser');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with regular expression in user name map with \1 replaced',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Success as the regular expression matches and \1 is replaced in the given
# subexpression, even if quoted.
reset_pg_ident($node, 'mypeermap', qq{/^$system_user(.*)\$},
	'"test\1mapuser"');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'with regular expression in user name map with quoted \1 replaced',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Failure as the regular expression does not include a subexpression, but
# the database user contains \1, requesting a replacement.
reset_pg_ident($node, 'mypeermap', qq{/^$system_user\$}, '\1testmapuser');
test_role(
	$node,
	qq{testmapuser},
	'peer', 2,
	'with regular expression in user name map with \1 not replaced',
	log_like => [
		qr/regular expression "\^$system_user\$" has no subexpressions as requested by backreference in "\\1testmapuser"/
	]);

# Concatenate system_user to system_user.
my $bad_regex_test_string = $system_user . $system_user;

# Failure as the regexp of system user does not match.
reset_pg_ident($node, 'mypeermap', qq{/^.*$bad_regex_test_string\$},
	'testmapuser');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	2,
	'with regexp of system user in user name map',
	log_like => [qr/no match in usermap "mypeermap" for user "testmapuser"/]);

# Test using a group role match for the database user.
reset_pg_ident($node, 'mypeermap', $system_user, '+testmapgroup');

test_role($node, qq{testmapuser}, 'peer', 0, 'plain user with group',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

test_role(
	$node, qq{testmapgroup}, 'peer', 2,
	'group user with group',
	log_like => [qr/role "testmapgroup" is not permitted to log in/]);

# Now apply quotes to the group match, nullifying its effect.
reset_pg_ident($node, 'mypeermap', $system_user, '"+testmapgroup"');
test_role(
	$node,
	qq{testmapuser},
	'peer',
	2,
	'plain user with quoted group name',
	log_like => [qr/no match in usermap "mypeermap" for user "testmapuser"/]);

# Test using a regexp for the system user, with a group membership
# check for the database user.
reset_pg_ident($node, 'mypeermap', qq{/^.*$regex_test_string\$},
	'+testmapgroup');

test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'regexp of system user as group member',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

test_role(
	$node,
	qq{testmapgroup},
	'peer',
	2,
	'regexp of system user as non-member of group',
	log_like => [qr/role "testmapgroup" is not permitted to log in/]);

# Test that membership checks and regexes will use literal \1 instead of
# replacing it, as subexpression replacement is not allowed in this case.
reset_pg_ident($node, 'mypeermap', qq{/^.*$regex_test_string(.*)\$},
	'+testmapgroupliteral\\1');

test_role(
	$node,
	qq{testmapuser},
	'peer',
	0,
	'membership check with literal \1',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

# Do the same with a quoted regular expression for the database user this
# time.  No replacement of \1 is done.
reset_pg_ident(
	$node, 'mypeermap',
	qq{/^.*$regex_test_string(.*)\$},
	'"/^testmapgroupliteral\\\\1$"');

test_role(
	$node,
	'testmapgroupliteral\\\\1',
	'peer',
	0,
	'regexp of database user with literal \1',
	log_like =>
	  [qr/connection authenticated: identity="$system_user" method=peer/]);

done_testing();
