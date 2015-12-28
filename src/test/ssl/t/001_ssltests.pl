use strict;
use warnings;
use PostgresNode;
use TestLib;
use TestLib;
use Test::More tests => 38;
use ServerSetup;
use File::Copy;

# Like TestLib.pm, we use IPC::Run
BEGIN
{
	eval {
		require IPC::Run;
		import IPC::Run qw(run start);
		1;
	} or do
	{
		plan skip_all => "IPC::Run not available";
	  }
}

#### Some configuration

# This is the hostname used to connect to the server. This cannot be a
# hostname, because the server certificate is always for the domain
# postgresql-ssl-regression.test.
my $SERVERHOSTADDR = '127.0.0.1';

# Define a couple of helper functions to test connecting to the server.

my $common_connstr;

sub run_test_psql
{
	my $connstr   = $_[0];
	my $logstring = $_[1];

	my $cmd = [
		'psql', '-X', '-A', '-t', '-c', "SELECT 'connected with $connstr'",
		'-d', "$connstr" ];

	my $result = run_log($cmd);
	return $result;
}

#
# The first argument is a (part of a) connection string, and it's also printed
# out as the test case name. It is appended to $common_connstr global variable,
# which also contains a libpq connection string.
#
# The second argument is a hostname to connect to.
sub test_connect_ok
{
	my $connstr = $_[0];

	my $result =
	  run_test_psql("$common_connstr $connstr", "(should succeed)");
	ok($result, $connstr);
}

sub test_connect_fails
{
	my $connstr = $_[0];

	my $result = run_test_psql("$common_connstr $connstr", "(should fail)");
	ok(!$result, "$connstr (should fail)");
}

# The client's private key must not be world-readable. Git doesn't track
# permissions (except for the executable bit), so they might be wrong after
# a checkout.
chmod 0600, "ssl/client.key";

#### Part 0. Set up the server.

diag "setting up data directory...";
my $node = get_new_node();
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;
configure_test_server_for_ssl($node, $SERVERHOSTADDR);
switch_server_cert($node, 'server-cn-only');

### Part 1. Run client-side tests.
###
### Test that libpq accepts/rejects the connection correctly, depending
### on sslmode and whether the server's certificate looks correct. No
### client certificate is used in these tests.

diag "running client tests...";

$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# The server should not accept non-SSL connections
diag "test that the server doesn't accept non-SSL connections";
test_connect_fails("sslmode=disable");

# Try without a root cert. In sslmode=require, this should work. In verify-ca
# or verify-full mode it should fail
diag "connect without server root cert";
test_connect_ok("sslrootcert=invalid sslmode=require");
test_connect_fails("sslrootcert=invalid sslmode=verify-ca");
test_connect_fails("sslrootcert=invalid sslmode=verify-full");

# Try with wrong root cert, should fail. (we're using the client CA as the
# root, but the server's key is signed by the server CA)
diag "connect without wrong server root cert";
test_connect_fails("sslrootcert=ssl/client_ca.crt sslmode=require");
test_connect_fails("sslrootcert=ssl/client_ca.crt sslmode=verify-ca");
test_connect_fails("sslrootcert=ssl/client_ca.crt sslmode=verify-full");

# Try with just the server CA's cert. This fails because the root file
# must contain the whole chain up to the root CA.
diag "connect with server CA cert, without root CA";
test_connect_fails("sslrootcert=ssl/server_ca.crt sslmode=verify-ca");

# And finally, with the correct root cert.
diag "connect with correct server CA cert file";
test_connect_ok("sslrootcert=ssl/root+server_ca.crt sslmode=require");
test_connect_ok("sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca");
test_connect_ok("sslrootcert=ssl/root+server_ca.crt sslmode=verify-full");

# Test with cert root file that contains two certificates. The client should
# be able to pick the right one, regardless of the order in the file.
test_connect_ok("sslrootcert=ssl/both-cas-1.crt sslmode=verify-ca");
test_connect_ok("sslrootcert=ssl/both-cas-2.crt sslmode=verify-ca");

diag "testing sslcrl option with a non-revoked cert";

# Invalid CRL filename is the same as no CRL, succeeds
test_connect_ok(
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=invalid");

# A CRL belonging to a different CA is not accepted, fails
test_connect_fails(
"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/client.crl");

# With the correct CRL, succeeds (this cert is not revoked)
test_connect_ok(
"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl"
);

# Check that connecting with verify-full fails, when the hostname doesn't
# match the hostname in the server's certificate.
diag "test mismatch between hostname and server certificate";
$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok("sslmode=require host=wronghost.test");
test_connect_ok("sslmode=verify-ca host=wronghost.test");
test_connect_fails("sslmode=verify-full host=wronghost.test");

# Test Subject Alternative Names.
switch_server_cert($node, 'server-multiple-alt-names');

diag "test hostname matching with X509 Subject Alternative Names";
$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok("host=dns1.alt-name.pg-ssltest.test");
test_connect_ok("host=dns2.alt-name.pg-ssltest.test");
test_connect_ok("host=foo.wildcard.pg-ssltest.test");

test_connect_fails("host=wronghost.alt-name.pg-ssltest.test");
test_connect_fails("host=deep.subdomain.wildcard.pg-ssltest.test");

# Test certificate with a single Subject Alternative Name. (this gives a
# slightly different error message, that's all)
switch_server_cert($node, 'server-single-alt-name');

diag "test hostname matching with a single X509 Subject Alternative Name";
$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok("host=single.alt-name.pg-ssltest.test");

test_connect_fails("host=wronghost.alt-name.pg-ssltest.test");
test_connect_fails("host=deep.subdomain.wildcard.pg-ssltest.test");

# Test server certificate with a CN and SANs. Per RFCs 2818 and 6125, the CN
# should be ignored when the certificate has both.
switch_server_cert($node, 'server-cn-and-alt-names');

diag "test certificate with both a CN and SANs";
$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok("host=dns1.alt-name.pg-ssltest.test");
test_connect_ok("host=dns2.alt-name.pg-ssltest.test");
test_connect_fails("host=common-name.pg-ssltest.test");

# Finally, test a server certificate that has no CN or SANs. Of course, that's
# not a very sensible certificate, but libpq should handle it gracefully.
switch_server_cert($node, 'server-no-names');
$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

test_connect_ok("sslmode=verify-ca host=common-name.pg-ssltest.test");
test_connect_fails("sslmode=verify-full host=common-name.pg-ssltest.test");

# Test that the CRL works
diag "Testing client-side CRL";
switch_server_cert($node, 'server-revoked');

$common_connstr =
"user=ssltestuser dbname=trustdb sslcert=invalid hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# Without the CRL, succeeds. With it, fails.
test_connect_ok("sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca");
test_connect_fails(
"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl"
);

### Part 2. Server-side tests.
###
### Test certificate authorization.

diag "Testing certificate authorization...";
$common_connstr =
"sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=certdb hostaddr=$SERVERHOSTADDR";

# no client cert
test_connect_fails("user=ssltestuser sslcert=invalid");

# correct client cert
test_connect_ok(
	"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client.key");

# client cert belonging to another user
test_connect_fails(
	"user=anotheruser sslcert=ssl/client.crt sslkey=ssl/client.key");

# revoked client cert
test_connect_fails(
"user=ssltestuser sslcert=ssl/client-revoked.crt sslkey=ssl/client-revoked.key"
);
