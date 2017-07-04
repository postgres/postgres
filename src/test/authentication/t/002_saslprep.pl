# Test password normalization in SCRAM.
#
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
	plan tests => 12;
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
sub test_login
{
	my $node          = shift;
	my $role          = shift;
	my $password      = shift;
	my $expected_res  = shift;
	my $status_string = 'failed';

	$status_string = 'success' if ($expected_res eq 0);

	$ENV{"PGPASSWORD"} = $password;
	my $res =
	  $node->psql('postgres', 'SELECT 1', extra_params => [ '-U', $role ]);
	is($res, $expected_res,
		"authentication $status_string for role $role with password $password"
	);
}

# Initialize master node. Force UTF-8 encoding, so that we can use non-ASCII
# characters in the passwords below.
my $node = get_new_node('master');
$node->init(extra => [ '--locale=C', '--encoding=UTF8' ]);
$node->start;

# These tests are based on the example strings from RFC4013.txt,
# Section "3. Examples":
#
# #  Input            Output     Comments
# -  -----            ------     --------
# 1  I<U+00AD>X       IX         SOFT HYPHEN mapped to nothing
# 2  user             user       no transformation
# 3  USER             USER       case preserved, will not match #2
# 4  <U+00AA>         a          output is NFKC, input in ISO 8859-1
# 5  <U+2168>         IX         output is NFKC, will match #1
# 6  <U+0007>                    Error - prohibited character
# 7  <U+0627><U+0031>            Error - bidirectional check

# Create test roles.
$node->safe_psql(
	'postgres',
	"SET password_encryption='scram-sha-256';
SET client_encoding='utf8';
CREATE ROLE saslpreptest1_role LOGIN PASSWORD 'IX';
CREATE ROLE saslpreptest4a_role LOGIN PASSWORD 'a';
CREATE ROLE saslpreptest4b_role LOGIN PASSWORD E'\\xc2\\xaa';
CREATE ROLE saslpreptest6_role LOGIN PASSWORD E'foo\\x07bar';
CREATE ROLE saslpreptest7_role LOGIN PASSWORD E'foo\\u0627\\u0031bar';
");

# Require password from now on.
reset_pg_hba($node, 'scram-sha-256');

# Check that #1 and #5 are treated the same as just 'IX'
test_login($node, 'saslpreptest1_role', "I\xc2\xadX",   0);
test_login($node, 'saslpreptest1_role', "\xe2\x85\xa8", 0);

# but different from lower case 'ix'
test_login($node, 'saslpreptest1_role', "ix", 2);

# Check #4
test_login($node, 'saslpreptest4a_role', "a",        0);
test_login($node, 'saslpreptest4a_role', "\xc2\xaa", 0);
test_login($node, 'saslpreptest4b_role', "a",        0);
test_login($node, 'saslpreptest4b_role', "\xc2\xaa", 0);

# Check #6 and #7 - In PostgreSQL, contrary to the spec, if the password
# contains prohibited characters, we use it as is, without normalization.
test_login($node, 'saslpreptest6_role', "foo\x07bar", 0);
test_login($node, 'saslpreptest6_role', "foobar",     2);

test_login($node, 'saslpreptest7_role', "foo\xd8\xa71bar", 0);
test_login($node, 'saslpreptest7_role', "foo1\xd8\xa7bar", 2);
test_login($node, 'saslpreptest7_role', "foobar",          2);
