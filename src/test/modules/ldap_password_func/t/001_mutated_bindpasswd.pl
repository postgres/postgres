
# Copyright (c) 2022-2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use File::Copy;
use FindBin;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

use lib "$FindBin::RealBin/../../../ldap";
use LdapServer;

if ($ENV{with_ldap} ne 'yes')
{
	plan skip_all => 'LDAP not supported by this build';
}
elsif (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bldap\b/)
{
	plan skip_all =>
	  'Potentially unsafe test LDAP not enabled in PG_TEST_EXTRA';
}
elsif (!$LdapServer::setup)
{
	plan skip_all =>
	  "ldap tests not supported on $^O or dependencies not installed";
}

my $clear_ldap_rootpw = "FooBaR1";
my $rot13_ldap_rootpw = "SbbOnE1";

my $ldap = LdapServer->new($clear_ldap_rootpw, 'users');   # no anonymous auth
$ldap->ldapadd_file("$FindBin::RealBin/../../../ldap/authdata.ldif");
$ldap->ldapsetpw('uid=test1,dc=example,dc=net', 'secret1');

my ($ldap_server, $ldap_port, $ldap_basedn, $ldap_rootdn) =
  $ldap->prop(qw(server port basedn rootdn));


note "setting up PostgreSQL instance";

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'ldap_password_func'");
$node->start;

$node->safe_psql('postgres', 'CREATE USER test1;');

note "running tests";

sub test_access
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	my ($node, $role, $expected_res, $test_name, %params) = @_;
	my $connstr = "user=$role";

	if ($expected_res eq 0)
	{
		$node->connect_ok($connstr, $test_name, %params);
	}
	else
	{
		# No checks of the error message, only the status code.
		$node->connect_fails($connstr, $test_name, %params);
	}
}

note "use ldapbindpasswd";

$ENV{"PGPASSWORD"} = 'secret1';

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn" ldapbinddn="$ldap_rootdn" ldapbindpasswd=wrong}
);
$node->restart;

test_access($node, 'test1', 2,
	'search+bind authentication fails with wrong ldapbindpasswd');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn" ldapbinddn="$ldap_rootdn" ldapbindpasswd="$clear_ldap_rootpw"}
);
$node->restart;

test_access($node, 'test1', 2,
	'search+bind authentication fails with clear password');

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf('pg_hba.conf',
	qq{local all all ldap ldapserver=$ldap_server ldapport=$ldap_port ldapbasedn="$ldap_basedn" ldapbinddn="$ldap_rootdn" ldapbindpasswd="$rot13_ldap_rootpw"}
);
$node->restart;

test_access($node, 'test1', 0,
	'search+bind authentication succeeds with rot13ed password');

done_testing();
