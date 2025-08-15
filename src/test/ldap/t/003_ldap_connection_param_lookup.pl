
# Copyright (c) 2025, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use File::Copy;
use LdapServer;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

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
	plan skip_all => $LdapServer::setup_error;
}

# This tests scenarios related to the service name and the service file,
# for the connection options and their environment variables.
my $dummy_node = PostgreSQL::Test::Cluster->new('dummy_node');
$dummy_node->init;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

note "setting up LDAP server";

my $ldap_rootpw = 'secret';
my $ldap = LdapServer->new($ldap_rootpw, 'anonymous');    # use anonymous auth
$ldap->ldapadd_file('authdata.ldif');
$ldap->ldapsetpw('uid=test1,dc=example,dc=net', 'secret1');
$ldap->ldapsetpw('uid=test2,dc=example,dc=net', 'secret2');

# Windows vs non-Windows: CRLF vs LF for the file's newline, relying on
# the fact that libpq uses fgets() when reading the lines of a service file.
my $newline = $windows_os ? "\r\n" : "\n";

my $td = PostgreSQL::Test::Utils::tempdir;

# create ldap file based on postgres connection info
my $ldif_valid = "$td/connection_params.ldif";
append_to_file($ldif_valid, "version:1");
append_to_file($ldif_valid, $newline);
append_to_file($ldif_valid, "dn:cn=mydatabase,dc=example,dc=net");
append_to_file($ldif_valid, $newline);
append_to_file($ldif_valid, "changetype:add");
append_to_file($ldif_valid, $newline);
append_to_file($ldif_valid, "objectclass:top");
append_to_file($ldif_valid, $newline);
append_to_file($ldif_valid, "objectclass:device");
append_to_file($ldif_valid, $newline);
append_to_file($ldif_valid, "cn:mydatabase");
append_to_file($ldif_valid, $newline);
append_to_file($ldif_valid, "description:host=");
append_to_file($ldif_valid, $node->host);
append_to_file($ldif_valid, $newline);
append_to_file($ldif_valid, "description:port=");
append_to_file($ldif_valid, $node->port);

$ldap->ldapadd_file($ldif_valid);

my ($ldap_server, $ldap_port, $ldaps_port, $ldap_url,
	$ldaps_url, $ldap_basedn, $ldap_rootdn
) = $ldap->prop(qw(server port s_port url s_url basedn rootdn));

# don't bother to check the server's cert (though perhaps we should)
$ENV{'LDAPTLS_REQCERT'} = "never";

note "setting up PostgreSQL instance";

# Create the set of service files used in the tests.

# File that includes a valid service name, that uses a decomposed
# connection string for its contents, split on spaces.
my $srvfile_valid = "$td/pg_service_valid.conf";
append_to_file($srvfile_valid, "[my_srv]");
append_to_file($srvfile_valid, $newline);
append_to_file($srvfile_valid, "ldap://localhost:");
append_to_file($srvfile_valid, $ldap_port);
append_to_file($srvfile_valid,
	"/dc=example,dc=net?description?one?(cn=mydatabase)");

# File defined with no contents, used as default value for
# PGSERVICEFILE, so that no lookup is attempted in the user's home
# directory.
my $srvfile_empty = "$td/pg_service_empty.conf";
append_to_file($srvfile_empty, '');

# Default service file in PGSYSCONFDIR.
my $srvfile_default = "$td/pg_service.conf";

# Missing service file.
my $srvfile_missing = "$td/pg_service_missing.conf";

# Set the fallback directory lookup of the service file to the
# temporary directory of this test.  PGSYSCONFDIR is used if the
# service file defined in PGSERVICEFILE cannot be found, or when a
# service file is found but not the service name.
local $ENV{PGSYSCONFDIR} = $td;

# Force PGSERVICEFILE to a default location, so as this test never
# tries to look at a home directory.  This value needs to remain at
# the top of this script before running any tests, and should never be
# changed.
local $ENV{PGSERVICEFILE} = "$srvfile_empty";

# Checks combinations of service name and a valid service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_valid;

	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and PGSERVICEFILE',
		sql => "SELECT 'connect1_1'",
		expected_stdout => qr/connect1_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and PGSERVICEFILE',
		sql => "SELECT 'connect1_2'",
		expected_stdout => qr/connect1_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and PGSERVICEFILE',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';

	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and PGSERVICEFILE',
		sql => "SELECT 'connect1_3'",
		expected_stdout => qr/connect1_3/);

	local $ENV{PGSERVICE} = 'undefined-service';

	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and PGSERVICEFILE',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);
}

# Checks case of incorrect service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_missing;

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with correct "service" string and incorrect PGSERVICEFILE',
		expected_stderr =>
		  qr/service file ".*pg_service_missing.conf" not found/);
}

# Checks case of service file named "pg_service.conf" in PGSYSCONFDIR.
{
	# Create copy of valid file
	my $srvfile_default = "$td/pg_service.conf";
	copy($srvfile_valid, $srvfile_default);

	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and pg_service.conf',
		sql => "SELECT 'connect2_1'",
		expected_stdout => qr/connect2_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and default pg_service.conf',
		sql => "SELECT 'connect2_2'",
		expected_stdout => qr/connect2_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and default pg_service.conf',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';

	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and default pg_service.conf',
		sql => "SELECT 'connect2_3'",
		expected_stdout => qr/connect2_3/);

	local $ENV{PGSERVICE} = 'undefined-service';

	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and default pg_service.conf',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);

	# Remove default pg_service.conf.
	unlink($srvfile_default);
}

$node->teardown_node;

done_testing();
