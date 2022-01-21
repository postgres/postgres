
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use File::Copy;

use FindBin;
use lib $FindBin::RealBin;

use SSLServer;

if ($ENV{with_ssl} ne 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}
else
{
	plan tests => 13;
}

#### Some configuration

# This is the hostname used to connect to the server. This cannot be a
# hostname, because the server certificate is always for the domain
# postgresql-ssl-regression.test.
my $SERVERHOSTADDR = '127.0.0.1';
# This is the pattern to use in pg_hba.conf to match incoming connections.
my $SERVERHOSTCIDR = '127.0.0.1/32';

# Allocation of base connection string shared among multiple tests.
my $common_connstr;

# The client's private key must not be world-readable, so take a copy
# of the key stored in the code tree and update its permissions.
my $cert_tempdir = PostgreSQL::Test::Utils::tempdir();
my $client_tmp_key = PostgreSQL::Test::Utils::perl2host("$cert_tempdir/client_ext.key");
copy("ssl/client_ext.key", "$cert_tempdir/client_ext.key")
  or die
  "couldn't copy ssl/client_ext.key to $cert_tempdir/client_ext.key for permissions change: $!";
chmod 0600, "$cert_tempdir/client_ext.key"
  or die "failed to change permissions on $cert_tempdir/client_ext.key: $!";
$client_tmp_key =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;

#### Set up the server.

note "setting up data directory";
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

configure_test_server_for_ssl($node, $SERVERHOSTADDR, $SERVERHOSTCIDR,
	'trust', extensions => [ qw(sslinfo) ]);

# We aren't using any CRL's in this suite so we can keep using server-revoked
# as server certificate for simple client.crt connection much like how the
# 001 test does.
switch_server_cert($node, 'server-revoked');

$common_connstr =
  "sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=certdb hostaddr=$SERVERHOSTADDR " .
  "user=ssltestuser sslcert=ssl/client_ext.crt sslkey=$client_tmp_key";

# Make sure we can connect even though previous test suites have established this
$node->connect_ok(
	$common_connstr,
	"certificate authorization succeeds with correct client cert in PEM format",
);

my $result;

$result = $node->safe_psql("certdb", "SELECT ssl_is_used();",
  connstr => $common_connstr);
is($result, 't', "ssl_is_used() for TLS connection");

$result = $node->safe_psql("certdb", "SELECT ssl_version();",
  connstr => $common_connstr . " ssl_min_protocol_version=TLSv1.2 " .
  "ssl_max_protocol_version=TLSv1.2");
is($result, 'TLSv1.2', "ssl_version() correctly returning TLS protocol");

$result = $node->safe_psql("certdb",
  "SELECT ssl_cipher() = cipher FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
  connstr => $common_connstr);
is($result, 't', "ssl_cipher() compared with pg_stat_ssl");

$result = $node->safe_psql("certdb", "SELECT ssl_client_cert_present();",
  connstr => $common_connstr);
is($result, 't', "ssl_client_cert_present() for connection with cert");

$result = $node->safe_psql("trustdb", "SELECT ssl_client_cert_present();",
  connstr => "sslrootcert=ssl/root+server_ca.crt sslmode=require " .
  "dbname=trustdb hostaddr=$SERVERHOSTADDR user=ssltestuser");
is($result, 'f', "ssl_client_cert_present() for connection without cert");

$result = $node->safe_psql("certdb",
  "SELECT ssl_client_serial() = client_serial FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
  connstr => $common_connstr);
is($result, 't', "ssl_client_serial() compared with pg_stat_ssl");

# Must not use safe_psql since we expect an error here
$result = $node->psql("certdb", "SELECT ssl_client_dn_field('invalid');",
  connstr => $common_connstr);
is($result, '3', "ssl_client_dn_field() for an invalid field");

$result = $node->safe_psql("trustdb", "SELECT ssl_client_dn_field('commonName');",
  connstr => "sslrootcert=ssl/root+server_ca.crt sslmode=require " .
  "dbname=trustdb hostaddr=$SERVERHOSTADDR user=ssltestuser");
is($result, '', "ssl_client_dn_field() for connection without cert");

$result = $node->safe_psql("certdb",
  "SELECT '/CN=' || ssl_client_dn_field('commonName') = client_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
  connstr => $common_connstr);
is($result, 't', "ssl_client_dn_field() for commonName");

$result = $node->safe_psql("certdb",
  "SELECT ssl_issuer_dn() = issuer_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
  connstr => $common_connstr);
is($result, 't', "ssl_issuer_dn() for connection with cert");

$result = $node->safe_psql("certdb",
  "SELECT '/CN=' || ssl_issuer_field('commonName') = issuer_dn FROM pg_stat_ssl WHERE pid = pg_backend_pid();",
  connstr => $common_connstr);
is($result, 't', "ssl_issuer_field() for commonName");

$result = $node->safe_psql("certdb",
  "SELECT value, critical FROM ssl_extension_info() WHERE name = 'basicConstraints';",
  connstr => $common_connstr);
is($result, 'CA:FALSE|t', 'extract extension from cert');
