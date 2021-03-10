use strict;
use warnings;
use PostgresNode;
use TestLib;
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
	plan tests => 100;
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
#
# This changes ssl/client.key to ssl/client_tmp.key etc for the rest
# of the tests.
my @keys = (
	"client",     "client-revoked",
	"client-der", "client-encrypted-pem",
	"client-encrypted-der");
foreach my $key (@keys)
{
	copy("ssl/${key}.key", "ssl/${key}_tmp.key")
	  or die
	  "couldn't copy ssl/${key}.key to ssl/${key}_tmp.key for permissions change: $!";
	chmod 0600, "ssl/${key}_tmp.key"
	  or die "failed to change permissions on ssl/${key}_tmp.key: $!";
}

# Also make a copy of that explicitly world-readable.  We can't
# necessarily rely on the file in the source tree having those
# permissions.  Add it to @keys to include it in the final clean
# up phase.
copy("ssl/client.key", "ssl/client_wrongperms_tmp.key");
chmod 0644, "ssl/client_wrongperms_tmp.key";
push @keys, 'client_wrongperms';

#### Set up the server.

note "setting up data directory";
my $node = get_new_node('primary');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

# Run this before we lock down access below.
my $result = $node->safe_psql('postgres', "SHOW ssl_library");
is($result, 'OpenSSL', 'ssl_library parameter');

configure_test_server_for_ssl($node, $SERVERHOSTADDR, $SERVERHOSTCIDR,
	'trust');

note "testing password-protected keys";

open my $sslconf, '>', $node->data_dir . "/sslconfig.conf";
print $sslconf "ssl=on\n";
print $sslconf "ssl_cert_file='server-cn-only.crt'\n";
print $sslconf "ssl_key_file='server-password.key'\n";
print $sslconf "ssl_passphrase_command='echo wrongpassword'\n";
close $sslconf;

command_fails(
	[ 'pg_ctl', '-D', $node->data_dir, '-l', $node->logfile, 'restart' ],
	'restart fails with password-protected key file with wrong password');
$node->_update_pid(0);

open $sslconf, '>', $node->data_dir . "/sslconfig.conf";
print $sslconf "ssl=on\n";
print $sslconf "ssl_cert_file='server-cn-only.crt'\n";
print $sslconf "ssl_key_file='server-password.key'\n";
print $sslconf "ssl_passphrase_command='echo secret1'\n";
close $sslconf;

command_ok(
	[ 'pg_ctl', '-D', $node->data_dir, '-l', $node->logfile, 'restart' ],
	'restart succeeds with password-protected key file');
$node->_update_pid(1);

# Test compatibility of SSL protocols.
# TLSv1.1 is lower than TLSv1.2, so it won't work.
$node->append_conf(
	'postgresql.conf',
	qq{ssl_min_protocol_version='TLSv1.2'
ssl_max_protocol_version='TLSv1.1'});
command_fails(
	[ 'pg_ctl', '-D', $node->data_dir, '-l', $node->logfile, 'restart' ],
	'restart fails with incorrect SSL protocol bounds');
# Go back to the defaults, this works.
$node->append_conf(
	'postgresql.conf',
	qq{ssl_min_protocol_version='TLSv1.2'
ssl_max_protocol_version=''});
command_ok(
	[ 'pg_ctl', '-D', $node->data_dir, '-l', $node->logfile, 'restart' ],
	'restart succeeds with correct SSL protocol bounds');

### Run client-side tests.
###
### Test that libpq accepts/rejects the connection correctly, depending
### on sslmode and whether the server's certificate looks correct. No
### client certificate is used in these tests.

note "running client tests";

switch_server_cert($node, 'server-cn-only');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# The server should not accept non-SSL connections.
test_connect_fails(
	$common_connstr, "sslmode=disable",
	qr/\Qno pg_hba.conf entry\E/,
	"server doesn't accept non-SSL connections");

# Try without a root cert. In sslmode=require, this should work. In verify-ca
# or verify-full mode it should fail.
test_connect_ok(
	$common_connstr,
	"sslrootcert=invalid sslmode=require",
	"connect without server root cert sslmode=require");
test_connect_fails(
	$common_connstr,
	"sslrootcert=invalid sslmode=verify-ca",
	qr/root certificate file "invalid" does not exist/,
	"connect without server root cert sslmode=verify-ca");
test_connect_fails(
	$common_connstr,
	"sslrootcert=invalid sslmode=verify-full",
	qr/root certificate file "invalid" does not exist/,
	"connect without server root cert sslmode=verify-full");

# Try with wrong root cert, should fail. (We're using the client CA as the
# root, but the server's key is signed by the server CA.)
test_connect_fails($common_connstr,
	"sslrootcert=ssl/client_ca.crt sslmode=require",
	qr/SSL error/, "connect with wrong server root cert sslmode=require");
test_connect_fails($common_connstr,
	"sslrootcert=ssl/client_ca.crt sslmode=verify-ca",
	qr/SSL error/, "connect with wrong server root cert sslmode=verify-ca");
test_connect_fails($common_connstr,
	"sslrootcert=ssl/client_ca.crt sslmode=verify-full",
	qr/SSL error/, "connect with wrong server root cert sslmode=verify-full");

# Try with just the server CA's cert. This fails because the root file
# must contain the whole chain up to the root CA.
test_connect_fails($common_connstr,
	"sslrootcert=ssl/server_ca.crt sslmode=verify-ca",
	qr/SSL error/, "connect with server CA cert, without root CA");

# And finally, with the correct root cert.
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=require",
	"connect with correct server CA cert file sslmode=require");
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca",
	"connect with correct server CA cert file sslmode=verify-ca");
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-full",
	"connect with correct server CA cert file sslmode=verify-full");

# Test with cert root file that contains two certificates. The client should
# be able to pick the right one, regardless of the order in the file.
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/both-cas-1.crt sslmode=verify-ca",
	"cert root file that contains two certificates, order 1");
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/both-cas-2.crt sslmode=verify-ca",
	"cert root file that contains two certificates, order 2");

# CRL tests

# Invalid CRL filename is the same as no CRL, succeeds
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=invalid",
	"sslcrl option with invalid file name");

# A CRL belonging to a different CA is not accepted, fails
test_connect_fails(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/client.crl",
	qr/SSL error/,
	"CRL belonging to a different CA");

# The same for CRL directory
test_connect_fails(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/client-crldir",
	qr/SSL error/,
	"directory CRL belonging to a different CA");

# With the correct CRL, succeeds (this cert is not revoked)
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl",
	"CRL with a non-revoked cert");

# The same for CRL directory
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/root+server-crldir",
	"directory CRL with a non-revoked cert");

# Check that connecting with verify-full fails, when the hostname doesn't
# match the hostname in the server's certificate.
$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

test_connect_ok(
	$common_connstr,
	"sslmode=require host=wronghost.test",
	"mismatch between host name and server certificate sslmode=require");
test_connect_ok(
	$common_connstr,
	"sslmode=verify-ca host=wronghost.test",
	"mismatch between host name and server certificate sslmode=verify-ca");
test_connect_fails(
	$common_connstr,
	"sslmode=verify-full host=wronghost.test",
	qr/\Qserver certificate for "common-name.pg-ssltest.test" does not match host name "wronghost.test"\E/,
	"mismatch between host name and server certificate sslmode=verify-full");

# Test Subject Alternative Names.
switch_server_cert($node, 'server-multiple-alt-names');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok(
	$common_connstr,
	"host=dns1.alt-name.pg-ssltest.test",
	"host name matching with X.509 Subject Alternative Names 1");
test_connect_ok(
	$common_connstr,
	"host=dns2.alt-name.pg-ssltest.test",
	"host name matching with X.509 Subject Alternative Names 2");
test_connect_ok(
	$common_connstr,
	"host=foo.wildcard.pg-ssltest.test",
	"host name matching with X.509 Subject Alternative Names wildcard");

test_connect_fails(
	$common_connstr,
	"host=wronghost.alt-name.pg-ssltest.test",
	qr/\Qserver certificate for "dns1.alt-name.pg-ssltest.test" (and 2 other names) does not match host name "wronghost.alt-name.pg-ssltest.test"\E/,
	"host name not matching with X.509 Subject Alternative Names");
test_connect_fails(
	$common_connstr,
	"host=deep.subdomain.wildcard.pg-ssltest.test",
	qr/\Qserver certificate for "dns1.alt-name.pg-ssltest.test" (and 2 other names) does not match host name "deep.subdomain.wildcard.pg-ssltest.test"\E/,
	"host name not matching with X.509 Subject Alternative Names wildcard");

# Test certificate with a single Subject Alternative Name. (this gives a
# slightly different error message, that's all)
switch_server_cert($node, 'server-single-alt-name');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok(
	$common_connstr,
	"host=single.alt-name.pg-ssltest.test",
	"host name matching with a single X.509 Subject Alternative Name");

test_connect_fails(
	$common_connstr,
	"host=wronghost.alt-name.pg-ssltest.test",
	qr/\Qserver certificate for "single.alt-name.pg-ssltest.test" does not match host name "wronghost.alt-name.pg-ssltest.test"\E/,
	"host name not matching with a single X.509 Subject Alternative Name");
test_connect_fails(
	$common_connstr,
	"host=deep.subdomain.wildcard.pg-ssltest.test",
	qr/\Qserver certificate for "single.alt-name.pg-ssltest.test" does not match host name "deep.subdomain.wildcard.pg-ssltest.test"\E/,
	"host name not matching with a single X.509 Subject Alternative Name wildcard"
);

# Test server certificate with a CN and SANs. Per RFCs 2818 and 6125, the CN
# should be ignored when the certificate has both.
switch_server_cert($node, 'server-cn-and-alt-names');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

test_connect_ok(
	$common_connstr,
	"host=dns1.alt-name.pg-ssltest.test",
	"certificate with both a CN and SANs 1");
test_connect_ok(
	$common_connstr,
	"host=dns2.alt-name.pg-ssltest.test",
	"certificate with both a CN and SANs 2");
test_connect_fails(
	$common_connstr,
	"host=common-name.pg-ssltest.test",
	qr/\Qserver certificate for "dns1.alt-name.pg-ssltest.test" (and 1 other name) does not match host name "common-name.pg-ssltest.test"\E/,
	"certificate with both a CN and SANs ignores CN");

# Finally, test a server certificate that has no CN or SANs. Of course, that's
# not a very sensible certificate, but libpq should handle it gracefully.
switch_server_cert($node, 'server-no-names');
$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

test_connect_ok(
	$common_connstr,
	"sslmode=verify-ca host=common-name.pg-ssltest.test",
	"server certificate without CN or SANs sslmode=verify-ca");
test_connect_fails(
	$common_connstr,
	"sslmode=verify-full host=common-name.pg-ssltest.test",
	qr/could not get server's host name from server certificate/,
	"server certificate without CN or SANs sslmode=verify-full");

# Test that the CRL works
switch_server_cert($node, 'server-revoked');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# Without the CRL, succeeds. With it, fails.
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca",
	"connects without client-side CRL");
test_connect_fails(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl",
	qr/SSL error/,
	"does not connect with client-side CRL file");
test_connect_fails(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/root+server-crldir",
	qr/SSL error/,
	"does not connect with client-side CRL directory");

# pg_stat_ssl
command_like(
	[
		'psql',                                '-X',
		'-A',                                  '-F',
		',',                                   '-P',
		'null=_null_',                         '-d',
		"$common_connstr sslrootcert=invalid", '-c',
		"SELECT * FROM pg_stat_ssl WHERE pid = pg_backend_pid()"
	],
	qr{^pid,ssl,version,cipher,bits,client_dn,client_serial,issuer_dn\r?\n
				^\d+,t,TLSv[\d.]+,[\w-]+,\d+,_null_,_null_,_null_\r?$}mx,
	'pg_stat_ssl view without client certificate');

# Test min/max SSL protocol versions.
test_connect_ok(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_min_protocol_version=TLSv1.2 ssl_max_protocol_version=TLSv1.2",
	"connection success with correct range of TLS protocol versions");
test_connect_fails(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_min_protocol_version=TLSv1.2 ssl_max_protocol_version=TLSv1.1",
	qr/invalid SSL protocol version range/,
	"connection failure with incorrect range of TLS protocol versions");
test_connect_fails(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_min_protocol_version=incorrect_tls",
	qr/invalid ssl_min_protocol_version value/,
	"connection failure with an incorrect SSL protocol minimum bound");
test_connect_fails(
	$common_connstr,
	"sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_max_protocol_version=incorrect_tls",
	qr/invalid ssl_max_protocol_version value/,
	"connection failure with an incorrect SSL protocol maximum bound");

### Server-side tests.
###
### Test certificate authorization.

note "running server tests";

$common_connstr =
  "sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=certdb hostaddr=$SERVERHOSTADDR";

# no client cert
test_connect_fails(
	$common_connstr,
	"user=ssltestuser sslcert=invalid",
	qr/connection requires a valid client certificate/,
	"certificate authorization fails without client cert");

# correct client cert in unencrypted PEM
test_connect_ok(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
	"certificate authorization succeeds with correct client cert in PEM format"
);

# correct client cert in unencrypted DER
test_connect_ok(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client-der_tmp.key",
	"certificate authorization succeeds with correct client cert in DER format"
);

# correct client cert in encrypted PEM
test_connect_ok(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client-encrypted-pem_tmp.key sslpassword='dUmmyP^#+'",
	"certificate authorization succeeds with correct client cert in encrypted PEM format"
);

# correct client cert in encrypted DER
test_connect_ok(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client-encrypted-der_tmp.key sslpassword='dUmmyP^#+'",
	"certificate authorization succeeds with correct client cert in encrypted DER format"
);

# correct client cert in encrypted PEM with wrong password
test_connect_fails(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client-encrypted-pem_tmp.key sslpassword='wrong'",
	qr!\Qprivate key file "ssl/client-encrypted-pem_tmp.key": bad decrypt\E!,
	"certificate authorization fails with correct client cert and wrong password in encrypted PEM format"
);

TODO:
{
	# these tests are left here waiting on us to get better pty support
	# so they don't hang. For now they are not performed.

	todo_skip "Need Pty support", 4;

	# correct client cert in encrypted PEM with empty password
	test_connect_fails(
		$common_connstr,
		"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client-encrypted-pem_tmp.key sslpassword=''",
		qr!\Qprivate key file "ssl/client-encrypted-pem_tmp.key": processing error\E!,
		"certificate authorization fails with correct client cert and empty password in encrypted PEM format"
	);

	# correct client cert in encrypted PEM with no password
	test_connect_fails(
		$common_connstr,
		"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client-encrypted-pem_tmp.key",
		qr!\Qprivate key file "ssl/client-encrypted-pem_tmp.key": processing error\E!,
		"certificate authorization fails with correct client cert and no password in encrypted PEM format"
	);

}

# pg_stat_ssl
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
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
		'-c',
		"SELECT * FROM pg_stat_ssl WHERE pid = pg_backend_pid()"
	],
	qr{^pid,ssl,version,cipher,bits,client_dn,client_serial,issuer_dn\r?\n
				^\d+,t,TLSv[\d.]+,[\w-]+,\d+,/CN=ssltestuser,1,\Q/CN=Test CA for PostgreSQL SSL regression test client certs\E\r?$}mx,
	'pg_stat_ssl with client certificate');

# client key with wrong permissions
SKIP:
{
	skip "Permissions check not enforced on Windows", 2 if ($windows_os);

	test_connect_fails(
		$common_connstr,
		"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client_wrongperms_tmp.key",
		qr!\Qprivate key file "ssl/client_wrongperms_tmp.key" has group or world access\E!,
		"certificate authorization fails because of file permissions");
}

# client cert belonging to another user
test_connect_fails(
	$common_connstr,
	"user=anotheruser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
	qr/certificate authentication failed for user "anotheruser"/,
	"certificate authorization fails with client cert belonging to another user"
);

# revoked client cert
test_connect_fails(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client-revoked.crt sslkey=ssl/client-revoked_tmp.key",
	qr/SSL error/,
	"certificate authorization fails with revoked client cert");

# Check that connecting with auth-option verify-full in pg_hba:
# works, iff username matches Common Name
# fails, iff username doesn't match Common Name.
$common_connstr =
  "sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=verifydb hostaddr=$SERVERHOSTADDR";

test_connect_ok(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
	"auth_option clientcert=verify-full succeeds with matching username and Common Name"
);

test_connect_fails(
	$common_connstr,
	"user=anotheruser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
	qr/FATAL/,
	"auth_option clientcert=verify-full fails with mismatching username and Common Name"
);

# Check that connecting with auth-optionverify-ca in pg_hba :
# works, when username doesn't match Common Name
test_connect_ok(
	$common_connstr,
	"user=yetanotheruser sslcert=ssl/client.crt sslkey=ssl/client_tmp.key",
	"auth_option clientcert=verify-ca succeeds with mismatching username and Common Name"
);

# intermediate client_ca.crt is provided by client, and isn't in server's ssl_ca_file
switch_server_cert($node, 'server-cn-only', 'root_ca');
$common_connstr =
  "user=ssltestuser dbname=certdb sslkey=ssl/client_tmp.key sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

test_connect_ok(
	$common_connstr,
	"sslmode=require sslcert=ssl/client+client_ca.crt",
	"intermediate client certificate is provided by client");
test_connect_fails($common_connstr, "sslmode=require sslcert=ssl/client.crt",
	qr/SSL error/, "intermediate client certificate is missing");

# test server-side CRL directory
switch_server_cert($node, 'server-cn-only', undef, undef, 'root+client-crldir');

# revoked client cert
test_connect_fails(
	$common_connstr,
	"user=ssltestuser sslcert=ssl/client-revoked.crt sslkey=ssl/client-revoked_tmp.key",
	qr/SSL error/,
	"certificate authorization fails with revoked client cert with server-side CRL directory");

# clean up
foreach my $key (@keys)
{
	unlink("ssl/${key}_tmp.key");
}
