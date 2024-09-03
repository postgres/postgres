
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use Config qw ( %Config );
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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

my $ssl_server = SSL::Server->new();

sub sslkey
{
	return $ssl_server->sslkey(@_);
}

sub switch_server_cert
{
	$ssl_server->switch_server_cert(@_);
}

# Determine whether this build uses OpenSSL or LibreSSL. As a heuristic, the
# HAVE_SSL_CTX_SET_CERT_CB macro isn't defined for LibreSSL.
my $libressl = not check_pg_config("#define HAVE_SSL_CTX_SET_CERT_CB 1");

#### Some configuration

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

# Run this before we lock down access below.
my $result = $node->safe_psql('postgres', "SHOW ssl_library");
is($result, $ssl_server->ssl_library(), 'ssl_library parameter');

$ssl_server->configure_test_server_for_ssl($node, $SERVERHOSTADDR,
	$SERVERHOSTCIDR, 'trust');

note "testing password-protected keys";

switch_server_cert(
	$node,
	certfile => 'server-cn-only',
	cafile => 'root+client_ca',
	keyfile => 'server-password',
	passphrase_cmd => 'echo wrongpassword',
	restart => 'no');

$result = $node->restart(fail_ok => 1);
is($result, 0,
	'restart fails with password-protected key file with wrong password');

switch_server_cert(
	$node,
	certfile => 'server-cn-only',
	cafile => 'root+client_ca',
	keyfile => 'server-password',
	passphrase_cmd => 'echo secret1',
	restart => 'no');

$result = $node->restart(fail_ok => 1);
is($result, 1, 'restart succeeds with password-protected key file');

# Test compatibility of SSL protocols.
# TLSv1.1 is lower than TLSv1.2, so it won't work.
$node->append_conf(
	'postgresql.conf',
	qq{ssl_min_protocol_version='TLSv1.2'
ssl_max_protocol_version='TLSv1.1'});
$result = $node->restart(fail_ok => 1);
is($result, 0, 'restart fails with incorrect SSL protocol bounds');

# Go back to the defaults, this works.
$node->append_conf(
	'postgresql.conf',
	qq{ssl_min_protocol_version='TLSv1.2'
ssl_max_protocol_version=''});
$result = $node->restart(fail_ok => 1);
is($result, 1, 'restart succeeds with correct SSL protocol bounds');

### Run client-side tests.
###
### Test that libpq accepts/rejects the connection correctly, depending
### on sslmode and whether the server's certificate looks correct. No
### client certificate is used in these tests.

note "running client tests";

switch_server_cert($node, certfile => 'server-cn-only');

# Set of default settings for SSL parameters in connection string.  This
# makes the tests protected against any defaults the environment may have
# in ~/.postgresql/.
my $default_ssl_connstr =
  "sslkey=invalid sslcert=invalid sslrootcert=invalid sslcrl=invalid sslcrldir=invalid";

$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# The server should not accept non-SSL connections.
$node->connect_fails(
	"$common_connstr sslmode=disable",
	"server doesn't accept non-SSL connections",
	expected_stderr => qr/\Qno pg_hba.conf entry\E/);

# Try without a root cert. In sslmode=require, this should work. In verify-ca
# or verify-full mode it should fail.
$node->connect_ok(
	"$common_connstr sslrootcert=invalid sslmode=require",
	"connect without server root cert sslmode=require");
$node->connect_fails(
	"$common_connstr sslrootcert=invalid sslmode=verify-ca",
	"connect without server root cert sslmode=verify-ca",
	expected_stderr => qr/root certificate file "invalid" does not exist/);
$node->connect_fails(
	"$common_connstr sslrootcert=invalid sslmode=verify-full",
	"connect without server root cert sslmode=verify-full",
	expected_stderr => qr/root certificate file "invalid" does not exist/);

# Try with wrong root cert, should fail. (We're using the client CA as the
# root, but the server's key is signed by the server CA.)
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/client_ca.crt sslmode=require",
	"connect with wrong server root cert sslmode=require",
	expected_stderr => qr/SSL error: certificate verify failed/);
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/client_ca.crt sslmode=verify-ca",
	"connect with wrong server root cert sslmode=verify-ca",
	expected_stderr => qr/SSL error: certificate verify failed/);
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/client_ca.crt sslmode=verify-full",
	"connect with wrong server root cert sslmode=verify-full",
	expected_stderr => qr/SSL error: certificate verify failed/);

# Try with just the server CA's cert. This fails because the root file
# must contain the whole chain up to the root CA.
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/server_ca.crt sslmode=verify-ca",
	"connect with server CA cert, without root CA",
	expected_stderr => qr/SSL error: certificate verify failed/);

# And finally, with the correct root cert.
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require",
	"connect with correct server CA cert file sslmode=require");
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca",
	"connect with correct server CA cert file sslmode=verify-ca");
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-full",
	"connect with correct server CA cert file sslmode=verify-full");

# Test with cert root file that contains two certificates. The client should
# be able to pick the right one, regardless of the order in the file.
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/both-cas-1.crt sslmode=verify-ca",
	"cert root file that contains two certificates, order 1");
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/both-cas-2.crt sslmode=verify-ca",
	"cert root file that contains two certificates, order 2");

# sslcertmode=allow and disable should both work without a client certificate.
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require sslcertmode=disable",
	"connect with sslcertmode=disable");
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require sslcertmode=allow",
	"connect with sslcertmode=allow");

# sslcertmode=require, however, should fail.
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require sslcertmode=require",
	"connect with sslcertmode=require fails without a client certificate",
	expected_stderr => $supports_sslcertmode_require
	? qr/server accepted connection without a valid SSL certificate/
	: qr/sslcertmode value "require" is not supported/);

# CRL tests

# Invalid CRL filename is the same as no CRL, succeeds
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=invalid",
	"sslcrl option with invalid file name");

# A CRL belonging to a different CA is not accepted, fails
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/client.crl",
	"CRL belonging to a different CA",
	expected_stderr => qr/SSL error: certificate verify failed/);

# The same for CRL directory.  sslcrl='' is added here to override the
# invalid default, so as this does not interfere with this case.
$node->connect_fails(
	"$common_connstr sslcrl='' sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/client-crldir",
	"directory CRL belonging to a different CA",
	expected_stderr => qr/SSL error: certificate verify failed/);

# With the correct CRL, succeeds (this cert is not revoked)
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl",
	"CRL with a non-revoked cert");

# The same for CRL directory
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/root+server-crldir",
	"directory CRL with a non-revoked cert");

# Check that connecting with verify-full fails, when the hostname doesn't
# match the hostname in the server's certificate.
$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

$node->connect_ok("$common_connstr sslmode=require host=wronghost.test",
	"mismatch between host name and server certificate sslmode=require");
$node->connect_ok(
	"$common_connstr sslmode=verify-ca host=wronghost.test",
	"mismatch between host name and server certificate sslmode=verify-ca");
$node->connect_fails(
	"$common_connstr sslmode=verify-full host=wronghost.test",
	"mismatch between host name and server certificate sslmode=verify-full",
	expected_stderr =>
	  qr/\Qserver certificate for "common-name.pg-ssltest.test" does not match host name "wronghost.test"\E/
);

# Test with an IP address in the Common Name. This is a strange corner case that
# nevertheless is supported, as long as the address string matches exactly.
switch_server_cert($node, certfile => 'server-ip-cn-only');

$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

$node->connect_ok("$common_connstr host=192.0.2.1",
	"IP address in the Common Name");

$node->connect_fails(
	"$common_connstr host=192.000.002.001",
	"mismatch between host name and server certificate IP address",
	expected_stderr =>
	  qr/\Qserver certificate for "192.0.2.1" does not match host name "192.000.002.001"\E/
);

# Similarly, we'll also match an IP address in a dNSName SAN. (This is
# long-standing behavior.)
switch_server_cert($node, certfile => 'server-ip-in-dnsname');

$node->connect_ok("$common_connstr host=192.0.2.1",
	"IP address in a dNSName");

# Test Subject Alternative Names.
switch_server_cert($node, certfile => 'server-multiple-alt-names');

$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

$node->connect_ok(
	"$common_connstr host=dns1.alt-name.pg-ssltest.test",
	"host name matching with X.509 Subject Alternative Names 1");
$node->connect_ok(
	"$common_connstr host=dns2.alt-name.pg-ssltest.test",
	"host name matching with X.509 Subject Alternative Names 2");
$node->connect_ok("$common_connstr host=foo.wildcard.pg-ssltest.test",
	"host name matching with X.509 Subject Alternative Names wildcard");

$node->connect_fails(
	"$common_connstr host=wronghost.alt-name.pg-ssltest.test",
	"host name not matching with X.509 Subject Alternative Names",
	expected_stderr =>
	  qr/\Qserver certificate for "dns1.alt-name.pg-ssltest.test" (and 2 other names) does not match host name "wronghost.alt-name.pg-ssltest.test"\E/
);
$node->connect_fails(
	"$common_connstr host=deep.subdomain.wildcard.pg-ssltest.test",
	"host name not matching with X.509 Subject Alternative Names wildcard",
	expected_stderr =>
	  qr/\Qserver certificate for "dns1.alt-name.pg-ssltest.test" (and 2 other names) does not match host name "deep.subdomain.wildcard.pg-ssltest.test"\E/
);

# Test certificate with a single Subject Alternative Name. (this gives a
# slightly different error message, that's all)
switch_server_cert($node, certfile => 'server-single-alt-name');

$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

$node->connect_ok(
	"$common_connstr host=single.alt-name.pg-ssltest.test",
	"host name matching with a single X.509 Subject Alternative Name");

$node->connect_fails(
	"$common_connstr host=wronghost.alt-name.pg-ssltest.test",
	"host name not matching with a single X.509 Subject Alternative Name",
	expected_stderr =>
	  qr/\Qserver certificate for "single.alt-name.pg-ssltest.test" does not match host name "wronghost.alt-name.pg-ssltest.test"\E/
);
$node->connect_fails(
	"$common_connstr host=deep.subdomain.wildcard.pg-ssltest.test",
	"host name not matching with a single X.509 Subject Alternative Name wildcard",
	expected_stderr =>
	  qr/\Qserver certificate for "single.alt-name.pg-ssltest.test" does not match host name "deep.subdomain.wildcard.pg-ssltest.test"\E/
);

SKIP:
{
	skip 'IPv6 addresses in certificates not support on this platform', 1
	  unless check_pg_config('#define HAVE_INET_PTON 1');

	# Test certificate with IP addresses in the SANs.
	switch_server_cert($node, certfile => 'server-ip-alt-names');

	$node->connect_ok("$common_connstr host=192.0.2.1",
		"host matching an IPv4 address (Subject Alternative Name 1)");

	$node->connect_ok(
		"$common_connstr host=192.000.002.001",
		"host matching an IPv4 address in alternate form (Subject Alternative Name 1)"
	);

	$node->connect_fails(
		"$common_connstr host=192.0.2.2",
		"host not matching an IPv4 address (Subject Alternative Name 1)",
		expected_stderr =>
		  qr/\Qserver certificate for "192.0.2.1" (and 1 other name) does not match host name "192.0.2.2"\E/
	);

	$node->connect_ok("$common_connstr host=2001:DB8::1",
		"host matching an IPv6 address (Subject Alternative Name 2)");

	$node->connect_ok(
		"$common_connstr host=2001:db8:0:0:0:0:0:1",
		"host matching an IPv6 address in alternate form (Subject Alternative Name 2)"
	);

	$node->connect_ok(
		"$common_connstr host=2001:db8::0.0.0.1",
		"host matching an IPv6 address in mixed form (Subject Alternative Name 2)"
	);

	$node->connect_fails(
		"$common_connstr host=::1",
		"host not matching an IPv6 address (Subject Alternative Name 2)",
		expected_stderr =>
		  qr/\Qserver certificate for "192.0.2.1" (and 1 other name) does not match host name "::1"\E/
	);

	$node->connect_fails(
		"$common_connstr host=2001:DB8::1/128",
		"IPv6 host with CIDR mask does not match",
		expected_stderr =>
		  qr/\Qserver certificate for "192.0.2.1" (and 1 other name) does not match host name "2001:DB8::1\/128"\E/
	);
}

# Test server certificate with a CN and DNS SANs. Per RFCs 2818 and 6125, the CN
# should be ignored when the certificate has both.
switch_server_cert($node, certfile => 'server-cn-and-alt-names');

$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

$node->connect_ok("$common_connstr host=dns1.alt-name.pg-ssltest.test",
	"certificate with both a CN and SANs 1");
$node->connect_ok("$common_connstr host=dns2.alt-name.pg-ssltest.test",
	"certificate with both a CN and SANs 2");
$node->connect_fails(
	"$common_connstr host=common-name.pg-ssltest.test",
	"certificate with both a CN and SANs ignores CN",
	expected_stderr =>
	  qr/\Qserver certificate for "dns1.alt-name.pg-ssltest.test" (and 1 other name) does not match host name "common-name.pg-ssltest.test"\E/
);

SKIP:
{
	skip 'IPv6 addresses in certificates not support on this platform', 1
	  unless check_pg_config('#define HAVE_INET_PTON 1');

	# But we will fall back to check the CN if the SANs contain only IP addresses.
	switch_server_cert($node, certfile => 'server-cn-and-ip-alt-names');

	$node->connect_ok(
		"$common_connstr host=common-name.pg-ssltest.test",
		"certificate with both a CN and IP SANs matches CN");
	$node->connect_ok("$common_connstr host=192.0.2.1",
		"certificate with both a CN and IP SANs matches SAN 1");
	$node->connect_ok("$common_connstr host=2001:db8::1",
		"certificate with both a CN and IP SANs matches SAN 2");

	# And now the same tests, but with IP addresses and DNS names swapped.
	switch_server_cert($node, certfile => 'server-ip-cn-and-alt-names');

	$node->connect_ok("$common_connstr host=192.0.2.2",
		"certificate with both an IP CN and IP SANs 1");
	$node->connect_ok("$common_connstr host=2001:db8::1",
		"certificate with both an IP CN and IP SANs 2");
	$node->connect_fails(
		"$common_connstr host=192.0.2.1",
		"certificate with both an IP CN and IP SANs ignores CN",
		expected_stderr =>
		  qr/\Qserver certificate for "192.0.2.2" (and 1 other name) does not match host name "192.0.2.1"\E/
	);
}

switch_server_cert($node, certfile => 'server-ip-cn-and-dns-alt-names');

$node->connect_ok("$common_connstr host=192.0.2.1",
	"certificate with both an IP CN and DNS SANs matches CN");
$node->connect_ok(
	"$common_connstr host=dns1.alt-name.pg-ssltest.test",
	"certificate with both an IP CN and DNS SANs matches SAN 1");
$node->connect_ok(
	"$common_connstr host=dns2.alt-name.pg-ssltest.test",
	"certificate with both an IP CN and DNS SANs matches SAN 2");

# Finally, test a server certificate that has no CN or SANs. Of course, that's
# not a very sensible certificate, but libpq should handle it gracefully.
switch_server_cert($node, certfile => 'server-no-names');
$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

$node->connect_ok(
	"$common_connstr sslmode=verify-ca host=common-name.pg-ssltest.test",
	"server certificate without CN or SANs sslmode=verify-ca");
$node->connect_fails(
	$common_connstr . " "
	  . "sslmode=verify-full host=common-name.pg-ssltest.test",
	"server certificate without CN or SANs sslmode=verify-full",
	expected_stderr =>
	  qr/could not get server's host name from server certificate/);

# Test system trusted roots.
switch_server_cert(
	$node,
	certfile => 'server-cn-only+server_ca',
	keyfile => 'server-cn-only',
	cafile => 'root_ca');
$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb sslrootcert=system hostaddr=$SERVERHOSTADDR";

# By default our custom-CA-signed certificate should not be trusted.
# OpenSSL 3.0 reports a missing/invalid system CA as "unregistered schema"
# instead of a failed certificate verification.
$node->connect_fails(
	"$common_connstr sslmode=verify-full host=common-name.pg-ssltest.test",
	"sslrootcert=system does not connect with private CA",
	expected_stderr =>
	  qr/SSL error: (certificate verify failed|unregistered scheme)/);

# Modes other than verify-full cannot be mixed with sslrootcert=system.
$node->connect_fails(
	"$common_connstr sslmode=verify-ca host=common-name.pg-ssltest.test",
	"sslrootcert=system only accepts sslmode=verify-full",
	expected_stderr =>
	  qr/weak sslmode "verify-ca" may not be used with sslrootcert=system/);

SKIP:
{
	skip "SSL_CERT_FILE is not supported with LibreSSL" if $libressl;

	# We can modify the definition of "system" to get it trusted again.
	local $ENV{SSL_CERT_FILE} = $node->data_dir . "/root_ca.crt";
	$node->connect_ok(
		"$common_connstr sslmode=verify-full host=common-name.pg-ssltest.test",
		"sslrootcert=system connects with overridden SSL_CERT_FILE");

	# verify-full mode should be the default for system CAs.
	$node->connect_fails(
		"$common_connstr host=common-name.pg-ssltest.test.bad",
		"sslrootcert=system defaults to sslmode=verify-full",
		expected_stderr =>
		  qr/server certificate for "common-name.pg-ssltest.test" does not match host name "common-name.pg-ssltest.test.bad"/
	);
}

# Test that the CRL works
switch_server_cert($node, certfile => 'server-revoked');

$common_connstr =
  "$default_ssl_connstr user=ssltestuser dbname=trustdb hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# Without the CRL, succeeds. With it, fails.
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca",
	"connects without client-side CRL");
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl",
	"does not connect with client-side CRL file",
	expected_stderr => qr/SSL error: certificate verify failed/);
# sslcrl='' is added here to override the invalid default, so as this
# does not interfere with this case.
$node->connect_fails(
	"$common_connstr sslcrl='' sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/root+server-crldir",
	"does not connect with client-side CRL directory",
	expected_stderr => qr/SSL error: certificate verify failed/);

# pg_stat_ssl
command_like(
	[
		'psql', '-X',
		'-A', '-F',
		',', '-P',
		'null=_null_', '-d',
		"$common_connstr sslrootcert=invalid", '-c',
		"SELECT * FROM pg_stat_ssl WHERE pid = pg_backend_pid()"
	],
	qr{^pid,ssl,version,cipher,bits,client_dn,client_serial,issuer_dn\r?\n
				^\d+,t,TLSv[\d.]+,[\w-]+,\d+,_null_,_null_,_null_\r?$}mx,
	'pg_stat_ssl view without client certificate');

# Test min/max SSL protocol versions.
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_min_protocol_version=TLSv1.2 ssl_max_protocol_version=TLSv1.2",
	"connection success with correct range of TLS protocol versions");
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_min_protocol_version=TLSv1.2 ssl_max_protocol_version=TLSv1.1",
	"connection failure with incorrect range of TLS protocol versions",
	expected_stderr => qr/invalid SSL protocol version range/);
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_min_protocol_version=incorrect_tls",
	"connection failure with an incorrect SSL protocol minimum bound",
	expected_stderr => qr/invalid "ssl_min_protocol_version" value/);
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_max_protocol_version=incorrect_tls",
	"connection failure with an incorrect SSL protocol maximum bound",
	expected_stderr => qr/invalid "ssl_max_protocol_version" value/);

### Server-side tests.
###
### Test certificate authorization.

note "running server tests";

$common_connstr =
  "$default_ssl_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=certdb hostaddr=$SERVERHOSTADDR host=localhost";

# no client cert
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=invalid",
	"certificate authorization fails without client cert",
	expected_stderr => qr/connection requires a valid client certificate/);

# correct client cert in unencrypted PEM
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
	  . sslkey('client.key'),
	"certificate authorization succeeds with correct client cert in PEM format"
);

# correct client cert in unencrypted DER
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
	  . sslkey('client-der.key'),
	"certificate authorization succeeds with correct client cert in DER format"
);

# correct client cert in encrypted PEM
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
	  . sslkey('client-encrypted-pem.key')
	  . " sslpassword='dUmmyP^#+'",
	"certificate authorization succeeds with correct client cert in encrypted PEM format"
);

# correct client cert in encrypted DER
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
	  . sslkey('client-encrypted-der.key')
	  . " sslpassword='dUmmyP^#+'",
	"certificate authorization succeeds with correct client cert in encrypted DER format"
);

# correct client cert with sslcertmode=allow or require
if ($supports_sslcertmode_require)
{
	$node->connect_ok(
		"$common_connstr user=ssltestuser sslcertmode=require sslcert=ssl/client.crt "
		  . sslkey('client.key'),
		"certificate authorization succeeds with correct client cert and sslcertmode=require"
	);
}
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcertmode=allow sslcert=ssl/client.crt "
	  . sslkey('client.key'),
	"certificate authorization succeeds with correct client cert and sslcertmode=allow"
);

# client cert is not sent if sslcertmode=disable.
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcertmode=disable sslcert=ssl/client.crt "
	  . sslkey('client.key'),
	"certificate authorization fails with correct client cert and sslcertmode=disable",
	expected_stderr => qr/connection requires a valid client certificate/);

# correct client cert in encrypted PEM with wrong password
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
	  . sslkey('client-encrypted-pem.key')
	  . " sslpassword='wrong'",
	"certificate authorization fails with correct client cert and wrong password in encrypted PEM format",
	expected_stderr =>
	  qr!private key file \".*client-encrypted-pem\.key\": bad decrypt!,);


# correct client cert using whole DN
my $dn_connstr = "$common_connstr dbname=certdb_dn";

$node->connect_ok(
	"$dn_connstr user=ssltestuser sslcert=ssl/client-dn.crt "
	  . sslkey('client-dn.key'),
	"certificate authorization succeeds with DN mapping",
	log_like => [
		qr/connection authenticated: identity="CN=ssltestuser-dn,OU=Testing,OU=Engineering,O=PGDG" method=cert/
	],);

# same thing but with a regex
$dn_connstr = "$common_connstr dbname=certdb_dn_re";

$node->connect_ok(
	"$dn_connstr user=ssltestuser sslcert=ssl/client-dn.crt "
	  . sslkey('client-dn.key'),
	"certificate authorization succeeds with DN regex mapping");

# same thing but using explicit CN
$dn_connstr = "$common_connstr dbname=certdb_cn";

$node->connect_ok(
	"$dn_connstr user=ssltestuser sslcert=ssl/client-dn.crt "
	  . sslkey('client-dn.key'),
	"certificate authorization succeeds with CN mapping",
	# the full DN should still be used as the authenticated identity
	log_like => [
		qr/connection authenticated: identity="CN=ssltestuser-dn,OU=Testing,OU=Engineering,O=PGDG" method=cert/
	],);



TODO:
{
	# these tests are left here waiting on us to get better pty support
	# so they don't hang. For now they are not performed.

	todo_skip "Need Pty support", 4;

	# correct client cert in encrypted PEM with empty password
	$node->connect_fails(
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
		  . sslkey('client-encrypted-pem.key')
		  . " sslpassword=''",
		"certificate authorization fails with correct client cert and empty password in encrypted PEM format",
		expected_stderr =>
		  qr!private key file \".*client-encrypted-pem\.key\": processing error!
	);

	# correct client cert in encrypted PEM with no password
	$node->connect_fails(
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
		  . sslkey('client-encrypted-pem.key'),
		"certificate authorization fails with correct client cert and no password in encrypted PEM format",
		expected_stderr =>
		  qr!private key file \".*client-encrypted-pem\.key\": processing error!
	);

}

# pg_stat_ssl

my $serialno = `$ENV{OPENSSL} x509 -serial -noout -in ssl/client.crt`;
if ($? == 0)
{
	# OpenSSL prints serial numbers in hexadecimal and converting the serial
	# from hex requires a 64-bit capable Perl as the serialnumber is based on
	# the current timestamp. On 32-bit fall back to checking for it being an
	# integer like how we do when grabbing the serial fails.
	if ($Config{ivsize} == 8)
	{
		no warnings qw(portable);

		$serialno =~ s/^serial=//;
		$serialno =~ s/\s+//g;
		$serialno = hex($serialno);
	}
	else
	{
		$serialno = '\d+';
	}
}
else
{
	# OpenSSL isn't functioning on the user's PATH. This probably isn't worth
	# skipping the test over, so just fall back to a generic integer match.
	warn "couldn't run \"$ENV{OPENSSL} x509\" to get client cert serialno";
	$serialno = '\d+';
}

command_like(
	[
		'psql',
		'-X',
		'-A',
		'-F',
		',',
		'-P',
		'null=_null_',
		'-d',
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
		  . sslkey('client.key'),
		'-c',
		"SELECT * FROM pg_stat_ssl WHERE pid = pg_backend_pid()"
	],
	qr{^pid,ssl,version,cipher,bits,client_dn,client_serial,issuer_dn\r?\n
				^\d+,t,TLSv[\d.]+,[\w-]+,\d+,/?CN=ssltestuser,$serialno,/?\QCN=Test CA for PostgreSQL SSL regression test client certs\E\r?$}mx,
	'pg_stat_ssl with client certificate');

# client key with wrong permissions
SKIP:
{
	skip "Permissions check not enforced on Windows", 2 if ($windows_os);

	$node->connect_fails(
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
		  . sslkey('client_wrongperms.key'),
		"certificate authorization fails because of file permissions",
		expected_stderr =>
		  qr!private key file \".*client_wrongperms\.key\" has group or world access!
	);
}

# client cert belonging to another user
$node->connect_fails(
	"$common_connstr user=anotheruser sslcert=ssl/client.crt "
	  . sslkey('client.key'),
	"certificate authorization fails with client cert belonging to another user",
	expected_stderr =>
	  qr/certificate authentication failed for user "anotheruser"/,
	# certificate authentication should be logged even on failure
	# temporarily(?) skip this check due to timing issue
	#	log_like =>
	#	  [qr/connection authenticated: identity="CN=ssltestuser" method=cert/],
);

# revoked client cert
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=ssl/client-revoked.crt "
	  . sslkey('client-revoked.key'),
	"certificate authorization fails with revoked client cert",
	expected_stderr => qr|SSL error: ssl[a-z0-9/]* alert certificate revoked|,
	# temporarily(?) skip this check due to timing issue
	#	log_like => [
	#		qr{Client certificate verification failed at depth 0: certificate revoked},
	#		qr{Failed certificate data \(unverified\): subject "/CN=ssltestuser", serial number 2315134995201656577, issuer "/CN=Test CA for PostgreSQL SSL regression test client certs"},
	#	],
	# revoked certificates should not authenticate the user
	log_unlike => [qr/connection authenticated:/],);

# Check that connecting with auth-option verify-full in pg_hba:
# works, iff username matches Common Name
# fails, iff username doesn't match Common Name.
$common_connstr =
  "$default_ssl_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=verifydb hostaddr=$SERVERHOSTADDR host=localhost";

$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt "
	  . sslkey('client.key'),
	"auth_option clientcert=verify-full succeeds with matching username and Common Name",
	log_like =>
	  [qr/connection authenticated: user="ssltestuser" method=trust/],);

$node->connect_fails(
	"$common_connstr user=anotheruser sslcert=ssl/client.crt "
	  . sslkey('client.key'),
	"auth_option clientcert=verify-full fails with mismatching username and Common Name",
	expected_stderr =>
	  qr/FATAL: .* "trust" authentication failed for user "anotheruser"/,
	# verify-full does not provide authentication
	log_unlike => [qr/connection authenticated:/],);

# Check that connecting with auth-option verify-ca in pg_hba :
# works, when username doesn't match Common Name
$node->connect_ok(
	"$common_connstr user=yetanotheruser sslcert=ssl/client.crt "
	  . sslkey('client.key'),
	"auth_option clientcert=verify-ca succeeds with mismatching username and Common Name",
	log_like =>
	  [qr/connection authenticated: user="yetanotheruser" method=trust/],);

# intermediate client_ca.crt is provided by client, and isn't in server's ssl_ca_file
switch_server_cert($node, certfile => 'server-cn-only', cafile => 'root_ca');
$common_connstr =
	"$default_ssl_connstr user=ssltestuser dbname=certdb "
  . sslkey('client.key')
  . " sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR host=localhost";

$node->connect_ok(
	"$common_connstr sslmode=require sslcert=ssl/client+client_ca.crt",
	"intermediate client certificate is provided by client");

$node->connect_fails(
	$common_connstr . " " . "sslmode=require sslcert=ssl/client.crt",
	"intermediate client certificate is missing",
	expected_stderr => qr/SSL error: tlsv1 alert unknown ca/,
	# temporarily(?) skip this check due to timing issue
	#	log_like => [
	#		qr{Client certificate verification failed at depth 0: unable to get local issuer certificate},
	#		qr{Failed certificate data \(unverified\): subject "/CN=ssltestuser", serial number 2315134995201656576, issuer "/CN=Test CA for PostgreSQL SSL regression test client certs"},
	#	]
);

$node->connect_fails(
	"$common_connstr sslmode=require sslcert=ssl/client-long.crt "
	  . sslkey('client-long.key'),
	"logged client certificate Subjects are truncated if they're too long",
	expected_stderr => qr/SSL error: tlsv1 alert unknown ca/,
	# temporarily(?) skip this check due to timing issue
	#	log_like => [
	#		qr{Client certificate verification failed at depth 0: unable to get local issuer certificate},
	#		qr{Failed certificate data \(unverified\): subject "\.\.\./CN=ssl-123456789012345678901234567890123456789012345678901234567890", serial number 2315418733629425152, issuer "/CN=Test CA for PostgreSQL SSL regression test client certs"},
	#	]
);

# Use an invalid cafile here so that the next test won't be able to verify the
# client CA.
switch_server_cert(
	$node,
	certfile => 'server-cn-only',
	cafile => 'server-cn-only');

# intermediate CA is provided but doesn't have a trusted root (checks error
# logging for cert chain depths > 0)
$node->connect_fails(
	"$common_connstr sslmode=require sslcert=ssl/client+client_ca.crt",
	"intermediate client certificate is untrusted",
	expected_stderr => qr/SSL error: tlsv1 alert unknown ca/,
	# temporarily(?) skip this check due to timing issue
	#	log_like => [
	#		qr{Client certificate verification failed at depth 1: unable to get local issuer certificate},
	#		qr{Failed certificate data \(unverified\): subject "/CN=Test CA for PostgreSQL SSL regression test client certs", serial number 2315134995201656577, issuer "/CN=Test root CA for PostgreSQL SSL regression test suite"},
	#	]
);

# test server-side CRL directory
switch_server_cert(
	$node,
	certfile => 'server-cn-only',
	crldir => 'root+client-crldir');

# revoked client cert
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=ssl/client-revoked.crt "
	  . sslkey('client-revoked.key'),
	"certificate authorization fails with revoked client cert with server-side CRL directory",
	expected_stderr => qr|SSL error: ssl[a-z0-9/]* alert certificate revoked|,
	# temporarily(?) skip this check due to timing issue
	#	log_like => [
	#		qr{Client certificate verification failed at depth 0: certificate revoked},
	#		qr{Failed certificate data \(unverified\): subject "/CN=ssltestuser", serial number 2315134995201656577, issuer "/CN=Test CA for PostgreSQL SSL regression test client certs"},
	#	]
);

# revoked client cert, non-ASCII subject
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=ssl/client-revoked-utf8.crt "
	  . sslkey('client-revoked-utf8.key'),
	"certificate authorization fails with revoked UTF-8 client cert with server-side CRL directory",
	expected_stderr => qr|SSL error: ssl[a-z0-9/]* alert certificate revoked|,
	# temporarily(?) skip this check due to timing issue
	#	log_like => [
	#		qr{Client certificate verification failed at depth 0: certificate revoked},
	#		qr{Failed certificate data \(unverified\): subject "/CN=\\xce\\x9f\\xce\\xb4\\xcf\\x85\\xcf\\x83\\xcf\\x83\\xce\\xad\\xce\\xb1\\xcf\\x82", serial number 2315420958437414144, issuer "/CN=Test CA for PostgreSQL SSL regression test client certs"},
	#	]
);

done_testing();
