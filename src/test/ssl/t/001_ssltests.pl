
# Copyright (c) 2021-2022, PostgreSQL Global Development Group

use strict;
use warnings;
use Config qw ( %Config );
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
	plan tests => 110;
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
# This changes to using keys stored in a temporary path for the rest of
# the tests. To get the full path for inclusion in connection strings, the
# %key hash can be interrogated.
my $cert_tempdir = PostgreSQL::Test::Utils::tempdir();
my %key;
my @keys = (
	"client.key",               "client-revoked.key",
	"client-der.key",           "client-encrypted-pem.key",
	"client-encrypted-der.key", "client-dn.key");
foreach my $keyfile (@keys)
{
	copy("ssl/$keyfile", "$cert_tempdir/$keyfile")
	  or die
	  "couldn't copy ssl/$keyfile to $cert_tempdir/$keyfile for permissions change: $!";
	chmod 0600, "$cert_tempdir/$keyfile"
	  or die "failed to change permissions on $cert_tempdir/$keyfile: $!";
	$key{$keyfile} = PostgreSQL::Test::Utils::perl2host("$cert_tempdir/$keyfile");
	$key{$keyfile} =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
}

# Also make a copy of that explicitly world-readable.  We can't
# necessarily rely on the file in the source tree having those
# permissions.
copy("ssl/client.key", "$cert_tempdir/client_wrongperms.key")
  or die
  "couldn't copy ssl/client_key to $cert_tempdir/client_wrongperms.key for permission change: $!";
chmod 0644, "$cert_tempdir/client_wrongperms.key"
  or die "failed to change permissions on $cert_tempdir/client_wrongperms.key: $!";
$key{'client_wrongperms.key'} = PostgreSQL::Test::Utils::perl2host("$cert_tempdir/client_wrongperms.key");
$key{'client_wrongperms.key'} =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
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

# The same for CRL directory
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/client-crldir",
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
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

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

# Test Subject Alternative Names.
switch_server_cert($node, 'server-multiple-alt-names');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

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
switch_server_cert($node, 'server-single-alt-name');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

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

# Test server certificate with a CN and SANs. Per RFCs 2818 and 6125, the CN
# should be ignored when the certificate has both.
switch_server_cert($node, 'server-cn-and-alt-names');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR sslmode=verify-full";

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

# Finally, test a server certificate that has no CN or SANs. Of course, that's
# not a very sensible certificate, but libpq should handle it gracefully.
switch_server_cert($node, 'server-no-names');
$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

$node->connect_ok(
	"$common_connstr sslmode=verify-ca host=common-name.pg-ssltest.test",
	"server certificate without CN or SANs sslmode=verify-ca");
$node->connect_fails(
	$common_connstr . " "
	  . "sslmode=verify-full host=common-name.pg-ssltest.test",
	"server certificate without CN or SANs sslmode=verify-full",
	expected_stderr =>
	  qr/could not get server's host name from server certificate/);

# Test that the CRL works
switch_server_cert($node, 'server-revoked');

$common_connstr =
  "user=ssltestuser dbname=trustdb sslcert=invalid hostaddr=$SERVERHOSTADDR host=common-name.pg-ssltest.test";

# Without the CRL, succeeds. With it, fails.
$node->connect_ok(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca",
	"connects without client-side CRL");
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrl=ssl/root+server.crl",
	"does not connect with client-side CRL file",
	expected_stderr => qr/SSL error: certificate verify failed/);
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=verify-ca sslcrldir=ssl/root+server-crldir",
	"does not connect with client-side CRL directory",
	expected_stderr => qr/SSL error: certificate verify failed/);

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
	expected_stderr => qr/invalid ssl_min_protocol_version value/);
$node->connect_fails(
	"$common_connstr sslrootcert=ssl/root+server_ca.crt sslmode=require ssl_max_protocol_version=incorrect_tls",
	"connection failure with an incorrect SSL protocol maximum bound",
	expected_stderr => qr/invalid ssl_max_protocol_version value/);

### Server-side tests.
###
### Test certificate authorization.

note "running server tests";

$common_connstr =
  "sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=certdb hostaddr=$SERVERHOSTADDR";

# no client cert
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=invalid",
	"certificate authorization fails without client cert",
	expected_stderr => qr/connection requires a valid client certificate/);

# correct client cert in unencrypted PEM
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client.key'}",
	"certificate authorization succeeds with correct client cert in PEM format"
);

# correct client cert in unencrypted DER
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client-der.key'}",
	"certificate authorization succeeds with correct client cert in DER format"
);

# correct client cert in encrypted PEM
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client-encrypted-pem.key'} sslpassword='dUmmyP^#+'",
	"certificate authorization succeeds with correct client cert in encrypted PEM format"
);

# correct client cert in encrypted DER
$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client-encrypted-der.key'} sslpassword='dUmmyP^#+'",
	"certificate authorization succeeds with correct client cert in encrypted DER format"
);

# correct client cert in encrypted PEM with wrong password
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client-encrypted-pem.key'} sslpassword='wrong'",
	"certificate authorization fails with correct client cert and wrong password in encrypted PEM format",
	expected_stderr =>
	  qr!\Qprivate key file "$key{'client-encrypted-pem.key'}": bad decrypt\E!
);


# correct client cert using whole DN
my $dn_connstr = "$common_connstr dbname=certdb_dn";

$node->connect_ok(
	"$dn_connstr user=ssltestuser sslcert=ssl/client-dn.crt sslkey=$key{'client-dn.key'}",
	"certificate authorization succeeds with DN mapping",
	log_like => [
		qr/connection authenticated: identity="CN=ssltestuser-dn,OU=Testing,OU=Engineering,O=PGDG" method=cert/
	],);

# same thing but with a regex
$dn_connstr = "$common_connstr dbname=certdb_dn_re";

$node->connect_ok(
	"$dn_connstr user=ssltestuser sslcert=ssl/client-dn.crt sslkey=$key{'client-dn.key'}",
	"certificate authorization succeeds with DN regex mapping");

# same thing but using explicit CN
$dn_connstr = "$common_connstr dbname=certdb_cn";

$node->connect_ok(
	"$dn_connstr user=ssltestuser sslcert=ssl/client-dn.crt sslkey=$key{'client-dn.key'}",
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
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client-encrypted-pem.key'} sslpassword=''",
		"certificate authorization fails with correct client cert and empty password in encrypted PEM format",
		expected_stderr =>
		  qr!\Qprivate key file "$key{'client-encrypted-pem.key'}": processing error\E!
	);

	# correct client cert in encrypted PEM with no password
	$node->connect_fails(
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client-encrypted-pem.key'}",
		"certificate authorization fails with correct client cert and no password in encrypted PEM format",
		expected_stderr =>
		  qr!\Qprivate key file "$key{'client-encrypted-pem.key'}": processing error\E!
	);

}

# pg_stat_ssl

my $serialno = `openssl x509 -serial -noout -in ssl/client.crt`;
if ($? == 0)
{
	# OpenSSL prints serial numbers in hexadecimal and converting the serial
	# from hex requires a 64-bit capable Perl as the serialnumber is based on
	# the current timestamp. On 32-bit fall back to checking for it being an
	# integer like how we do when grabbing the serial fails.
	if ($Config{ivsize} == 8)
	{
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
	warn 'couldn\'t run `openssl x509` to get client cert serialno';
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
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client.key'}",
		'-c',
		"SELECT * FROM pg_stat_ssl WHERE pid = pg_backend_pid()"
	],
	qr{^pid,ssl,version,cipher,bits,client_dn,client_serial,issuer_dn\r?\n
				^\d+,t,TLSv[\d.]+,[\w-]+,\d+,/CN=ssltestuser,$serialno,\Q/CN=Test CA for PostgreSQL SSL regression test client certs\E\r?$}mx,
	'pg_stat_ssl with client certificate');

# client key with wrong permissions
SKIP:
{
	skip "Permissions check not enforced on Windows", 2 if ($windows_os);

	$node->connect_fails(
		"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client_wrongperms.key'}",
		"certificate authorization fails because of file permissions",
		expected_stderr =>
		  qr!\Qprivate key file "$key{'client_wrongperms.key'}" has group or world access\E!
	);
}

# client cert belonging to another user
$node->connect_fails(
	"$common_connstr user=anotheruser sslcert=ssl/client.crt sslkey=$key{'client.key'}",
	"certificate authorization fails with client cert belonging to another user",
	expected_stderr =>
	  qr/certificate authentication failed for user "anotheruser"/,
	# certificate authentication should be logged even on failure
	log_like =>
	  [qr/connection authenticated: identity="CN=ssltestuser" method=cert/],);

# revoked client cert
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=ssl/client-revoked.crt sslkey=$key{'client-revoked.key'}",
	"certificate authorization fails with revoked client cert",
	expected_stderr => qr/SSL error: sslv3 alert certificate revoked/,
	# revoked certificates should not authenticate the user
	log_unlike => [qr/connection authenticated:/],);

# Check that connecting with auth-option verify-full in pg_hba:
# works, iff username matches Common Name
# fails, iff username doesn't match Common Name.
$common_connstr =
  "sslrootcert=ssl/root+server_ca.crt sslmode=require dbname=verifydb hostaddr=$SERVERHOSTADDR";

$node->connect_ok(
	"$common_connstr user=ssltestuser sslcert=ssl/client.crt sslkey=$key{'client.key'}",
	"auth_option clientcert=verify-full succeeds with matching username and Common Name",
	# verify-full does not provide authentication
	log_unlike => [qr/connection authenticated:/],);

$node->connect_fails(
	"$common_connstr user=anotheruser sslcert=ssl/client.crt sslkey=$key{'client.key'}",
	"auth_option clientcert=verify-full fails with mismatching username and Common Name",
	expected_stderr =>
	  qr/FATAL: .* "trust" authentication failed for user "anotheruser"/,
	# verify-full does not provide authentication
	log_unlike => [qr/connection authenticated:/],);

# Check that connecting with auth-optionverify-ca in pg_hba :
# works, when username doesn't match Common Name
$node->connect_ok(
	"$common_connstr user=yetanotheruser sslcert=ssl/client.crt sslkey=$key{'client.key'}",
	"auth_option clientcert=verify-ca succeeds with mismatching username and Common Name",
	# verify-full does not provide authentication
	log_unlike => [qr/connection authenticated:/],);

# intermediate client_ca.crt is provided by client, and isn't in server's ssl_ca_file
switch_server_cert($node, 'server-cn-only', 'root_ca');
$common_connstr =
  "user=ssltestuser dbname=certdb sslkey=$key{'client.key'} sslrootcert=ssl/root+server_ca.crt hostaddr=$SERVERHOSTADDR";

$node->connect_ok(
	"$common_connstr sslmode=require sslcert=ssl/client+client_ca.crt",
	"intermediate client certificate is provided by client");
$node->connect_fails(
	$common_connstr . " " . "sslmode=require sslcert=ssl/client.crt",
	"intermediate client certificate is missing",
	expected_stderr => qr/SSL error: tlsv1 alert unknown ca/);

# test server-side CRL directory
switch_server_cert($node, 'server-cn-only', undef, undef,
	'root+client-crldir');

# revoked client cert
$node->connect_fails(
	"$common_connstr user=ssltestuser sslcert=ssl/client-revoked.crt sslkey=$key{'client-revoked.key'}",
	"certificate authorization fails with revoked client cert with server-side CRL directory",
	expected_stderr => qr/SSL error: sslv3 alert certificate revoked/);
