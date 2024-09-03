
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use File::Copy;

use FindBin;
use lib $FindBin::RealBin;

use SSL::Server;

if ($ENV{with_ssl} ne 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}
if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bssl\b/)
{
	plan skip_all =>
	  'Potentially unsafe test SSL not enabled in PG_TEST_EXTRA';
}

#### Some configuration
my $ssl_server = SSL::Server->new();

sub sslkey
{
	return $ssl_server->sslkey(@_);
}

sub switch_server_cert
{
	$ssl_server->switch_server_cert(@_);
}

# This is the hostname used to connect to the server. This cannot be a
# hostname, because the server certificate is always for the domain
# postgresql-ssl-regression.test.
my $SERVERHOSTADDR = '127.0.0.1';
# This is the pattern to use in pg_hba.conf to match incoming connections.
my $SERVERHOSTCIDR = '127.0.0.1/32';

# Determine whether build supports sslcertmode=require.
my $supports_sslcertmode_require =
  check_pg_config("#define HAVE_SSL_CTX_SET_CERT_CB 1");

# Allocation of base connection string shared among multiple tests.
my $common_connstr;

#### Set up the server.

note "setting up data directory";
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

$ssl_server->configure_test_server_for_ssl($node, $SERVERHOSTADDR,
	$SERVERHOSTCIDR, 'trust', extensions => [qw(sslinfo)]);

# We aren't using any CRL's in this suite so we can keep using server-revoked
# as server certificate for simple client.crt connection much like how the
# 001 test does.
switch_server_cert($node, certfile => 'server-revoked');

# Set of default settings for SSL parameters in connection string.  This
# makes the tests protected against any defaults the environment may have
# in ~/.postgresql/.
my $default_ssl_connstr =
  "sslkey=invalid sslcert=invalid sslrootcert=invalid sslcrl=invalid sslcrldir=invalid";

$common_connstr =
  "$default_ssl_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=certdb hostaddr=$SERVERHOSTADDR host=localhost "
  . "user=ssltestuser sslcert=ssl/client_ext.crt "
  . sslkey('client_ext.key');

# Make sure we can connect even though previous test suites have established this
$node->connect_ok(
	$common_connstr,
	"certificate authorization succeeds with correct client cert in PEM format",
);

my $result;

$result = $node->safe_psql(
	"certdb",
	"SELECT ssl_is_used();",
	connstr => $common_connstr);
is($result, 't', "ssl_is_used() for TLS connection");

$result = $node->safe_psql(
	"certdb",
	"SELECT ssl_version();",
	connstr => $common_connstr
	  . " ssl_min_protocol_version=TLSv1.2 "
	  . "ssl_max_protocol_version=TLSv1.2");
is($result, 'TLSv1.2', "ssl_version() correctly returning TLS protocol");

$result = $node->safe_psql(
	"certdb",
	"SELECT ssl_cipher() = cipher FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
	connstr => $common_connstr);
is($result, 't', "ssl_cipher() compared with pg_stat_ssl");

$result = $node->safe_psql(
	"certdb",
	"SELECT ssl_client_cert_present();",
	connstr => $common_connstr);
is($result, 't', "ssl_client_cert_present() for connection with cert");

$result = $node->safe_psql(
	"trustdb",
	"SELECT ssl_client_cert_present();",
	connstr =>
	  "$default_ssl_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require "
	  . "dbname=trustdb hostaddr=$SERVERHOSTADDR user=ssltestuser host=localhost"
);
is($result, 'f', "ssl_client_cert_present() for connection without cert");

$result = $node->safe_psql(
	"certdb",
	"SELECT ssl_client_serial() = client_serial FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
	connstr => $common_connstr);
is($result, 't', "ssl_client_serial() compared with pg_stat_ssl");

# Must not use safe_psql since we expect an error here
$result = $node->psql(
	"certdb",
	"SELECT ssl_client_dn_field('invalid');",
	connstr => $common_connstr);
is($result, '3', "ssl_client_dn_field() for an invalid field");

$result = $node->safe_psql(
	"trustdb",
	"SELECT ssl_client_dn_field('commonName');",
	connstr =>
	  "$default_ssl_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require "
	  . "dbname=trustdb hostaddr=$SERVERHOSTADDR user=ssltestuser host=localhost"
);
is($result, '', "ssl_client_dn_field() for connection without cert");

$result = $node->safe_psql(
	"certdb",
	"SELECT '/CN=' || ssl_client_dn_field('commonName') = client_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
	connstr => $common_connstr);
is($result, 't', "ssl_client_dn_field() for commonName");

$result = $node->safe_psql(
	"certdb",
	"SELECT ssl_issuer_dn() = issuer_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
	connstr => $common_connstr);
is($result, 't', "ssl_issuer_dn() for connection with cert");

$result = $node->safe_psql(
	"certdb",
	"SELECT '/CN=' || ssl_issuer_field('commonName') = issuer_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
	connstr => $common_connstr);
is($result, 't', "ssl_issuer_field() for commonName");

$result = $node->safe_psql(
	"certdb",
	"SELECT value, critical FROM ssl_extension_info() WHERE name = 'basicConstraints';",
	connstr => $common_connstr);
is($result, 'CA:FALSE|t', 'extract extension from cert');

# Sanity tests for sslcertmode, using ssl_client_cert_present()
my @cases = (
	{ opts => "sslcertmode=allow", present => 't' },
	{ opts => "sslcertmode=allow sslcert=invalid", present => 'f' },
	{ opts => "sslcertmode=disable", present => 'f' },);
if ($supports_sslcertmode_require)
{
	push(@cases, { opts => "sslcertmode=require", present => 't' });
}

foreach my $c (@cases)
{
	$result = $node->safe_psql(
		"trustdb",
		"SELECT ssl_client_cert_present();",
		connstr => "$common_connstr dbname=trustdb $c->{'opts'}");
	is($result, $c->{'present'},
		"ssl_client_cert_present() for $c->{'opts'}");
}

done_testing();
