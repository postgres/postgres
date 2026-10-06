
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use SSL::Server;

# This is the hostaddr used to connect to the server. This cannot be a
# hostname, because the server certificate is always for the domain
# postgresql-ssl-regression.test.
my $SERVERHOSTADDR = '127.0.0.1';
# This is the pattern to use in pg_hba.conf to match incoming connections.
my $SERVERHOSTCIDR = '127.0.0.1/32';

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

if ($ssl_server->is_libressl)
{
	plan skip_all => 'SNI not supported when building with LibreSSL';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

my $exec_backend = $node->safe_psql('postgres', 'SHOW debug_exec_backend');
chomp($exec_backend);

$ssl_server->configure_test_server_for_ssl($node, $SERVERHOSTADDR,
	$SERVERHOSTCIDR, 'trust');

$ssl_server->switch_server_cert($node, certfile => 'server-cn-only');

my $connstr =
  "user=ssltestuser dbname=trustdb hostaddr=$SERVERHOSTADDR sslsni=1";

# Enable SNI with an encrypted host key and a per-host passphrase command.
{
	$ssl_server->switch_server_cert(
		$node,
		certfile => 'server-ip-cn-only',
		passphrase_cmd => '');
	my $verify_connstr =
	  "$connstr sslmode=verify-full sslrootcert=ssl/root+server_ca.crt sslcert=invalid sslkey=invalid sslcrl=invalid sslcrldir=invalid";
	$node->connect_ok(
		"$verify_connstr host=192.0.2.1 sslsni=0",
		'global certificate before enabling SNI');
	$node->append_conf('pg_hosts.conf',
		'common-name.pg-ssltest.test server-cn-only.crt server-password.key root+client_ca.crt "echo secret1" on'
	);
	$node->append_conf('postgresql.conf', 'ssl_sni = on');
	my $log_offset = -s $node->logfile;
	$node->reload;
	$node->wait_for_log(qr/reloading configuration files/, $log_offset);
	$node->connect_ok(
		"$verify_connstr host=common-name.pg-ssltest.test",
		'reload enables SNI with the per-host passphrase command');

	ok(unlink($node->data_dir . '/pg_hosts.conf'));
	$node->append_conf('pg_hosts.conf', '');
	$node->append_conf('postgresql.conf', 'ssl_sni = off');
	$ssl_server->switch_server_cert($node, certfile => 'server-cn-only');
}

# Check certificate selection after a failed reload and a successful retry.
SKIP:
{
	skip 'SSL context retention requires forked backends', 11
	  if $exec_backend =~ /on/;

	my $verify_connstr =
	  "$connstr sslmode=verify-full sslrootcert=ssl/root+server_ca.crt sslcert=invalid sslkey=invalid sslcrl=invalid sslcrldir=invalid";
	$node->append_conf(
		'pg_hosts.conf', qq{
* server-ip-cn-only.crt server-ip-cn-only.key
common-name.pg-ssltest.test server-cn-only.crt server-cn-only.key
});
	$node->append_conf('postgresql.conf', 'ssl_sni = on');
	my $log_offset = -s $node->logfile;
	$node->reload;
	$node->wait_for_log(qr/reloading configuration files/, $log_offset);
	$node->connect_ok(
		"$verify_connstr host=common-name.pg-ssltest.test",
		'loaded SNI mode selects the named host certificate');

	$node->append_conf('postgresql.conf', 'ssl_sni = off');
	$node->append_conf('sslconfig.conf',
		"ssl_cert_file = 'missing-server.crt'");
	$log_offset = -s $node->logfile;
	$node->reload;
	$node->wait_for_log(qr/SSL configuration was not reloaded/, $log_offset);
	$node->connect_ok(
		"$verify_connstr host=common-name.pg-ssltest.test",
		'failed reload retains the named host certificate with a default host configured'
	);

	$ssl_server->switch_server_cert(
		$node,
		certfile => 'server-ip-cn-only',
		restart => 'no');
	$log_offset = -s $node->logfile;
	$node->reload;
	$node->wait_for_log(qr/reloading configuration files/, $log_offset);
	$node->connect_fails(
		"$verify_connstr host=common-name.pg-ssltest.test",
		'successful retry disables SNI host selection',
		expected_stderr =>
		  qr/server certificate for "192\.0\.2\.1" does not match host name/);
	$node->connect_ok("$verify_connstr host=192.0.2.1 sslsni=0",
		'successful retry serves the configured certificate');

	ok(unlink($node->data_dir . '/pg_hosts.conf'));
	$node->append_conf('pg_hosts.conf', '');
	$ssl_server->switch_server_cert($node, certfile => 'server-cn-only');
}

##############################################################################
# postgresql.conf
##############################################################################

# Connect without any hosts configured in pg_hosts.conf, thus using the cert
# and key in postgresql.conf. pg_hosts.conf exists at this point but is empty
# apart from the comments stemming from the sample.
$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require",
	"pg.conf: connect with correct server CA cert file sslmode=require");

$node->connect_fails(
	"$connstr sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg.conf: connect fails without intermediate for sslmode=verify-ca",
	expected_stderr => qr/certificate verify failed/);

# Add an entry in pg_hosts.conf with no default, and reload. Since ssl_sni is
# still 'off' we should still be able to connect using the certificates in
# postgresql.conf
$node->append_conf('pg_hosts.conf',
	"example.org server-cn-only.crt server-cn-only.key");
$node->reload;
$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require",
	"pg.conf: connect with correct server CA cert file sslmode=require");

# Turn on SNI support and remove pg_hosts.conf and reload to make sure a
# missing file is treated like an empty file.
$node->append_conf('postgresql.conf', 'ssl_sni = on');
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->reload;

$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require",
	"pg.conf: connect after deleting pg_hosts.conf");

##############################################################################
# pg_hosts.conf
##############################################################################

# Replicate the postgresql.conf configuration into pg_hosts.conf and retry the
# same tests as above.
$node->append_conf('pg_hosts.conf',
	"* server-cn-only.crt server-cn-only.key");
$node->reload;

$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require",
	"pg_hosts.conf: connect to default, with correct server CA cert file sslmode=require"
);

$node->connect_fails(
	"$connstr sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg_hosts.conf: connect to default, fail without intermediate for sslmode=verify-ca",
	expected_stderr => qr/certificate verify failed/);

# Add host entry for example.org which serves the server cert and its
# intermediate CA.  The previously existing default host still exists without
# a CA.
$node->append_conf('pg_hosts.conf',
	"example.org server-cn-only+server_ca.crt server-cn-only.key root_ca.crt"
);
$node->reload;

$node->connect_ok(
	"$connstr host=example.org sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg_hosts.conf: connect to example.org and verify server CA");

$node->connect_ok(
	"$connstr host=Example.ORG sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg_hosts.conf: connect to Example.ORG and verify server CA");

$node->connect_fails(
	"$connstr host=example.org sslrootcert=invalid sslmode=verify-ca",
	"pg_hosts.conf: connect to example.org but without server root cert, sslmode=verify-ca",
	expected_stderr => qr/root certificate file "invalid" does not exist/);

$node->connect_fails(
	"$connstr sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg_hosts.conf: connect to default and fail to verify CA",
	expected_stderr => qr/certificate verify failed/);

$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require",
	"pg_hosts.conf: connect to default with sslmode=require");

# Use multiple hostnames for a single configuration
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	"example.org,example.com,example.net server-cn-only+server_ca.crt server-cn-only.key root_ca.crt"
);
$node->reload;

$node->connect_ok(
	"$connstr host=example.org sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg_hosts.conf: connect to example.org and verify server CA");
$node->connect_ok(
	"$connstr host=example.com sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg_hosts.conf: connect to example.com and verify server CA");
$node->connect_ok(
	"$connstr host=example.net sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	"pg_hosts.conf: connect to example.net and verify server CA");
$node->connect_fails(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=example.se",
	"pg_hosts.conf: connect to default with sslmode=require",
	expected_stderr => qr/unrecognized name/);

# Test @-inclusion of hostnames.
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	'example.org,@hostnames.txt server-cn-only+server_ca.crt server-cn-only.key root_ca.crt'
);
$node->append_conf(
	'hostnames.txt', qq{
example.com
example.net
});
$node->reload;

$node->connect_ok(
	"$connstr host=example.org sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	'@hostnames.txt: connect to example.org and verify server CA');
$node->connect_ok(
	"$connstr host=example.com sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	'@hostnames.txt: connect to example.com and verify server CA');
$node->connect_ok(
	"$connstr host=example.net sslrootcert=ssl/root_ca.crt sslmode=verify-ca",
	'@hostnames.txt: connect to example.net and verify server CA');
$node->connect_fails(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=example.se",
	'@hostnames.txt: connect to default with sslmode=require',
	expected_stderr => qr/unrecognized name/);

# Add an incorrect entry specifying a default entry combined with hostnames
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	"example.org,*,example.net server-cn-only+server_ca.crt server-cn-only.key root_ca.crt"
);
my $result = $node->restart(fail_ok => 1);
is($result, 0,
	'pg_hosts.conf: restart fails with default entry combined with hostnames'
);

# Add incorrect duplicate entries.
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf(
	'pg_hosts.conf', qq{
* server-cn-only.crt server-cn-only.key
* server-cn-only.crt server-cn-only.key
});
$result = $node->restart(fail_ok => 1);
is($result, 0, 'pg_hosts.conf: restart fails with two default entries');

ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf(
	'pg_hosts.conf', qq{
/no_sni/ server-cn-only.crt server-cn-only.key
/no_sni/ server-cn-only.crt server-cn-only.key
});
$result = $node->restart(fail_ok => 1);
is($result, 0, 'pg_hosts.conf: restart fails with two no_sni entries');

ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf(
	'pg_hosts.conf', qq{
example.org server-cn-only.crt server-cn-only.key
example.net server-cn-only.crt server-cn-only.key
example.org server-cn-only.crt server-cn-only.key
});
$result = $node->restart(fail_ok => 1);
is($result, 0,
	'pg_hosts.conf: restart fails with two identical hostname entries');
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf(
	'pg_hosts.conf', qq{
example.org server-cn-only.crt server-cn-only.key
example.net,example.com,Example.org server-cn-only.crt server-cn-only.key
});
$result = $node->restart(fail_ok => 1);
is($result, 0,
	'pg_hosts.conf: restart fails with two identical hostname entries in lists'
);

# Modify pg_hosts.conf to no longer have the default host entry.
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	"example.org server-cn-only+server_ca.crt server-cn-only.key root_ca.crt"
);
$node->restart;

# Connecting without a hostname as well as with a hostname which isn't in the
# pg_hosts configuration should fail.
$node->connect_fails(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require sslsni=0",
	"pg_hosts.conf: connect to default with sslmode=require",
	expected_stderr => qr/handshake failure/);
$node->connect_fails(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=example.com",
	"pg_hosts.conf: connect to default with sslmode=require",
	expected_stderr => qr/unrecognized name/);
$node->connect_fails(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=example",
	"pg_hosts.conf: connect to 'example' with sslmode=require",
	expected_stderr => qr/unrecognized name/);

# Turn off SNI while the postgresql.conf configuration cannot be loaded, such
# that the reload fails to replace the SSL configuration.  The pg_hosts.conf
# configuration without a default host must remain in effect, ssl_sni will be
# set to 'off' but the previous config - including ssl_sni setting - is what
# will be used.
my $bsession = $node->background_psql('trustdb',
	connstr =>
	  "$connstr host=example.org sslrootcert=ssl/root_ca.crt sslmode=verify-ca"
);
$result = $bsession->query_safe('SHOW ssl_sni');
is($result, 'on', 'SNI is enabled in active config for background session');

$node->append_conf(
	'postgresql.conf', qq{
ssl_sni = off
ssl_cert_file = 'nonexistent.crt'
});
my $node_loglocation = -s $node->logfile;
$node->reload;

$node->wait_for_log(qr/SSL configuration was not reloaded/,
	$node_loglocation);
$node->log_check('SSL reload triggered WARNING on ssl_sni state',
	$node_loglocation, log_like => [qr/SNI remains on/]);

$result = $bsession->query_safe('SHOW ssl_sni');
is($result, 'off', 'SNI is reported as off in background session');
$bsession->quit;
$node_loglocation = -s $node->logfile;

# EXEC_BACKEND backends load the SSL configuration on each connection instead
# of reusing the contexts in the postmaster
SKIP:
{
	skip 'SSL config reload is per connection in EXEC_BACKEND', 4
	  if ($windows_os || $exec_backend =~ /on/);

	my ($rc, $stdout, $stderr) = $node->psql(
		'trustdb',
		qq[SHOW ssl_sni;],
		connstr =>
		  "$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=example.org"
	);
	is($rc, 0,
		"pg_hosts.conf: connect to example.org after failed reload with ssl_sni off"
	);
	is($stdout, 'off', 'ssl_sni is set to off in a new connection');

	$node->connect_fails(
		"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require sslsni=0",
		"pg_hosts.conf: connect to default after failed reload with ssl_sni off",
		expected_stderr => qr/handshake failure/);
}

# Specifically test EXEC_BACKEND where a failed reload by definition cannot
# retain the previous configuration.  Here the server should not accept any
# connections.
SKIP:
{
	skip 'Only testing EXEC_BACKEND behavior', 2 if ($exec_backend !~ /on/);
	$node->connect_fails(
		"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require sslsni=0",
		"pg_hosts.conf: connect to default after failed reload with ssl_sni off",
		expected_stderr => qr/server does not support SSL/);
}

# Reset configuration for the next test
$node->append_conf(
	'postgresql.conf', qq{
ssl_sni = on
ssl_cert_file = ''
});
$node_loglocation = -s $node->logfile;
$node->reload;

# Reconfigure with broken configuration for the key passphrase, the server
# should not start up
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	'localhost server-cn-only.crt server-password.key root+client_ca.crt "echo wrongpassword" on'
);
$result = $node->restart(fail_ok => 1);
is($result, 0,
	'pg_hosts.conf: restart fails with password-protected key when using the wrong passphrase command'
);

# Reconfigure again but with the correct passphrase set
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	'localhost server-cn-only.crt server-password.key root+client_ca.crt "echo secret1" on'
);
$result = $node->restart(fail_ok => 1);
is($result, 1,
	'pg_hosts.conf: restart succeeds with password-protected key when using the correct passphrase command'
);

# Make sure connecting works, and try to stress the reload logic by issuing
# subsequent reloads
$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=localhost",
	"pg_hosts.conf: connect with correct server CA cert file sslmode=require"
);
$node->reload;
$node->reload;
$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=localhost",
	"pg_hosts.conf: connect with correct server CA cert file after reloads");
$node->reload;
$node->reload;
$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=localhost",
	"pg_hosts.conf: connect with correct server CA cert file after more reloads"
);

# Test reloading a passphrase protected key without reloading support in the
# passphrase hook. Restarting should not give any errors in the log, but the
# subsequent reload should fail with an error regarding reloading.
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	'localhost server-cn-only.crt server-password.key root+client_ca.crt "echo secret1" off'
);
$node_loglocation = -s $node->logfile;
$result = $node->restart(fail_ok => 1);
is($result, 1,
	'pg_hosts.conf: restart succeeds with password-protected key when using the correct passphrase command'
);
my $log =
  PostgreSQL::Test::Utils::slurp_file($node->logfile, $node_loglocation);
unlike(
	$log,
	qr/cannot be reloaded because it requires a passphrase/,
	'log reload failure due to passphrase command reloading');

SKIP:
{
	# Passphrase reloads must be enabled on Windows (and EXEC_BACKEND) to
	# succeed even without a restart
	skip "Passphrase command reload required on Windows and EXEC_BACKEND", 1
	  if ($windows_os || $exec_backend =~ /on/);

	$node->connect_ok(
		"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=localhost",
		"pg_hosts.conf: connect with correct server CA cert file sslmode=require"
	);
	# Reloading should fail since the passphrase cannot be reloaded, with an
	# error recorded in the log.  Since we keep existing contexts around it
	# should still work.
	$node_loglocation = -s $node->logfile;
	$node->reload;
	$node->connect_ok(
		"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=localhost",
		"pg_hosts.conf: connect with correct server CA cert file sslmode=require"
	);
	$log =
	  PostgreSQL::Test::Utils::slurp_file($node->logfile, $node_loglocation);
	like(
		$log,
		qr/cannot be reloaded because it requires a passphrase/,
		'log reload failure due to passphrase command reloading');
}

# Configure with only non-SNI connections allowed
ok(unlink($node->data_dir . '/pg_hosts.conf'));
$node->append_conf('pg_hosts.conf',
	"/no_sni/ server-cn-only.crt server-cn-only.key");
$node->restart;

$node->connect_ok(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require sslsni=0",
	"pg_hosts.conf: only non-SNI connections allowed");

$node->connect_fails(
	"$connstr sslrootcert=ssl/root+server_ca.crt sslmode=require host=example.org",
	"pg_hosts.conf: only non-SNI connections allowed, connecting with SNI",
	expected_stderr => qr/unrecognized name/);

# Test client CAs

# pg_hosts configuration
ok(unlink($node->data_dir . '/pg_hosts.conf'));

# Neither ssl_ca_file nor the default host should have any effect whatsoever on
# the following tests.
$node->append_conf('postgresql.conf', "ssl_ca_file = 'root+client_ca.crt'");
$node->append_conf('pg_hosts.conf',
	'* server-cn-only.crt server-cn-only.key root+client_ca.crt');

# example.org has an unconfigured CA.
$node->append_conf('pg_hosts.conf',
	'example.org server-cn-only.crt server-cn-only.key');
# example.com uses the client CA.
$node->append_conf('pg_hosts.conf',
	'example.com server-cn-only.crt server-cn-only.key root+client_ca.crt');
# example.net uses the server CA (which is wrong).
$node->append_conf('pg_hosts.conf',
	'example.net server-cn-only.crt server-cn-only.key root+server_ca.crt');

$node->restart;

$connstr =
  "user=ssltestuser dbname=certdb hostaddr=$SERVERHOSTADDR sslmode=require sslsni=1";

# example.org is unconfigured and should fail.
$node->connect_fails(
	"$connstr host=example.org sslcertmode=require sslcert=ssl/client.crt"
	  . $ssl_server->sslkey('client.key'),
	"host: 'example.org', ca: '': connect with sslcert, no client CA configured",
	expected_stderr =>
	  qr/client certificates can only be checked if a root certificate store is available/
);

# example.com is configured and should require a valid client cert.
$node->connect_fails(
	"$connstr host=example.com sslcertmode=disable",
	"host: 'example.com', ca: 'root+client_ca.crt': connect fails if no client certificate sent",
	expected_stderr => qr/connection requires a valid client certificate/);

$node->connect_ok(
	"$connstr host=example.com sslcertmode=require sslcert=ssl/client.crt "
	  . $ssl_server->sslkey('client.key'),
	"host: 'example.com', ca: 'root+client_ca.crt': connect with sslcert, client certificate sent"
);

# example.net is configured and should require a client cert, but will
# always fail verification.
$node->connect_fails(
	"$connstr host=example.net sslcertmode=disable",
	"host: 'example.net', ca: 'root+server_ca.crt': connect fails if no client certificate sent",
	expected_stderr => qr/connection requires a valid client certificate/);

$node->connect_fails(
	"$connstr host=example.net sslcertmode=require sslcert=ssl/client.crt "
	  . $ssl_server->sslkey('client.key'),
	"host: 'example.net', ca: 'root+server_ca.crt': connect with sslcert, client certificate sent",
	expected_stderr => qr/unknown ca/);

# Make sure the global CRL dir interacts properly with per-host trust.
$ssl_server->switch_server_cert(
	$node,
	certfile => 'server-cn-only',
	crldir => 'client-crldir');

$node->connect_fails(
	"$connstr host=example.com sslcertmode=require sslcert=ssl/client-revoked.crt "
	  . $ssl_server->sslkey('client-revoked.key'),
	"host: 'example.com', ca: 'root+client_ca.crt': connect fails with revoked client cert",
	expected_stderr => qr/certificate revoked/);

# pg_hosts configuration with useless data at EOL
ok(unlink($node->data_dir . '/pg_hosts.conf'));
# example.org has an unconfigured CA.
$node->append_conf('pg_hosts.conf',
	'example.org server-cn-only.crt server-cn-only.key root+client_ca.crt "cmd" on TRAILING_TEXT MORE_TEXT'
);
$result = $node->restart(fail_ok => 1);
is($result, 0, 'pg_hosts.conf: restart fails with extra data at EOL');
# pg_hosts configuration with useless data at EOL
ok(unlink($node->data_dir . '/pg_hosts.conf'));
# example.org has an unconfigured CA.
$node->append_conf('pg_hosts.conf',
	'example.org server-cn-only.crt server-cn-only.key root+client_ca.crt "cmd" notabooleanvalue'
);
$result = $node->restart(fail_ok => 1);
is($result, 0,
	'pg_hosts.conf: restart fails with non-boolean value in boolean field');

done_testing();
