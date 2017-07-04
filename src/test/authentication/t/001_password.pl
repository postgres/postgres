# Set of tests for authentication and pg_hba.conf. The following password
# methods are checked through this test:
# - Plain
# - MD5-encrypted
# - SCRAM-encrypted
# This test cannot run on Windows as Postgres cannot be set up with Unix
# sockets and needs to go through SSPI.

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;
if ($windows_os)
{
	plan skip_all => "authentication tests cannot run on Windows";
}
else
{
	plan tests => 8;
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
}

# Test access for a single role, useful to wrap all tests into one.
sub test_role
{
	my $node          = shift;
	my $role          = shift;
	my $method        = shift;
	my $expected_res  = shift;
	my $status_string = 'failed';

	$status_string = 'success' if ($expected_res eq 0);

	my $res =
	  $node->psql('postgres', 'SELECT 1', extra_params => [ '-U', $role ]);
	is($res, $expected_res,
		"authentication $status_string for method $method, role $role");
}

# Initialize master node
my $node = get_new_node('master');
$node->init;
$node->start;

# Create 3 roles with different password methods for each one. The same
# password is used for all of them.
$node->safe_psql('postgres',
"SET password_encryption='scram-sha-256'; CREATE ROLE scram_role LOGIN PASSWORD 'pass';"
);
$node->safe_psql('postgres',
"SET password_encryption='md5'; CREATE ROLE md5_role LOGIN PASSWORD 'pass';");
$ENV{"PGPASSWORD"} = 'pass';

# For "trust" method, all users should be able to connect.
reset_pg_hba($node, 'trust');
test_role($node, 'scram_role', 'trust', 0);
test_role($node, 'md5_role',   'trust', 0);

# For plain "password" method, all users should also be able to connect.
reset_pg_hba($node, 'password');
test_role($node, 'scram_role', 'password', 0);
test_role($node, 'md5_role',   'password', 0);

# For "scram-sha-256" method, user "scram_role" should be able to connect.
reset_pg_hba($node, 'scram-sha-256');
test_role($node, 'scram_role', 'scram-sha-256', 0);
test_role($node, 'md5_role',   'scram-sha-256', 2);

# For "md5" method, all users should be able to connect (SCRAM
# authentication will be performed for the user with a scram verifier.)
reset_pg_hba($node, 'md5');
test_role($node, 'scram_role', 'md5', 0);
test_role($node, 'md5_role',   'md5', 0);
