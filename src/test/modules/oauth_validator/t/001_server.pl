
#
# Tests the libpq builtin OAuth flow, as well as server-side HBA and validator
# setup.
#
# Copyright (c) 2021-2025, PostgreSQL Global Development Group
#

use strict;
use warnings FATAL => 'all';

use JSON::PP     qw(encode_json);
use MIME::Base64 qw(encode_base64);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use OAuth::Server;

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\boauth\b/)
{
	plan skip_all =>
	  'Potentially unsafe test oauth not enabled in PG_TEST_EXTRA';
}

if ($windows_os)
{
	plan skip_all => 'OAuth server-side tests are not supported on Windows';
}

if ($ENV{with_libcurl} ne 'yes')
{
	plan skip_all => 'client-side OAuth not supported by this build';
}

if ($ENV{with_python} ne 'yes')
{
	plan skip_all => 'OAuth tests require --with-python to run';
}

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->append_conf('postgresql.conf',
	"oauth_validator_libraries = 'validator'\n");
$node->start;

$node->safe_psql('postgres', 'CREATE USER test;');
$node->safe_psql('postgres', 'CREATE USER testalt;');
$node->safe_psql('postgres', 'CREATE USER testparam;');

# Save a background connection for later configuration changes.
my $bgconn = $node->background_psql('postgres');

my $webserver = OAuth::Server->new();
$webserver->run();

END
{
	my $exit_code = $?;

	$webserver->stop() if defined $webserver;    # might have been SKIP'd

	$? = $exit_code;
}

my $port = $webserver->port();
my $issuer = "http://127.0.0.1:$port";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf', qq{
local all test      oauth issuer="$issuer"       scope="openid postgres"
local all testalt   oauth issuer="$issuer/.well-known/oauth-authorization-server/alternate" scope="openid postgres alt"
local all testparam oauth issuer="$issuer/param" scope="openid postgres"
});
$node->reload;

my $log_start = $node->wait_for_log(qr/reloading configuration files/);

# Check pg_hba_file_rules() support.
my $contents = $bgconn->query_safe(
	qq(SELECT rule_number, auth_method, options
		 FROM pg_hba_file_rules
		 ORDER BY rule_number;));
is( $contents,
	qq{1|oauth|\{issuer=$issuer,"scope=openid postgres",validator=validator\}
2|oauth|\{issuer=$issuer/.well-known/oauth-authorization-server/alternate,"scope=openid postgres alt",validator=validator\}
3|oauth|\{issuer=$issuer/param,"scope=openid postgres",validator=validator\}},
	"pg_hba_file_rules recreates OAuth HBA settings");

# To test against HTTP rather than HTTPS, we need to enable PGOAUTHDEBUG. But
# first, check to make sure the client refuses such connections by default.
$node->connect_fails(
	"user=test dbname=postgres oauth_issuer=$issuer oauth_client_id=f02c6361-0635",
	"HTTPS is required without debug mode",
	expected_stderr =>
	  qr@OAuth discovery URI "\Q$issuer\E/.well-known/openid-configuration" must use HTTPS@
);

$ENV{PGOAUTHDEBUG} = "UNSAFE";

my $user = "test";
$node->connect_ok(
	"user=$user dbname=postgres oauth_issuer=$issuer oauth_client_id=f02c6361-0635",
	"connect as test",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@,
	log_like => [
		qr/oauth_validator: token="9243959234", role="$user"/,
		qr/oauth_validator: issuer="\Q$issuer\E", scope="openid postgres"/,
		qr/connection authenticated: identity="test" method=oauth/,
		qr/connection authorized/,
	]);

# The /alternate issuer uses slightly different parameters, along with an
# OAuth-style discovery document.
$user = "testalt";
$node->connect_ok(
	"user=$user dbname=postgres oauth_issuer=$issuer/alternate oauth_client_id=f02c6361-0636",
	"connect as testalt",
	expected_stderr =>
	  qr@Visit https://example\.org/ and enter the code: postgresuser@,
	log_like => [
		qr/oauth_validator: token="9243959234-alt", role="$user"/,
		qr|oauth_validator: issuer="\Q$issuer/.well-known/oauth-authorization-server/alternate\E", scope="openid postgres alt"|,
		qr/connection authenticated: identity="testalt" method=oauth/,
		qr/connection authorized/,
	]);

# The issuer linked by the server must match the client's oauth_issuer setting.
$node->connect_fails(
	"user=$user dbname=postgres oauth_issuer=$issuer oauth_client_id=f02c6361-0636",
	"oauth_issuer must match discovery",
	expected_stderr =>
	  qr@server's discovery document at \Q$issuer/.well-known/oauth-authorization-server/alternate\E \(issuer "\Q$issuer/alternate\E"\) is incompatible with oauth_issuer \(\Q$issuer\E\)@
);

# Test require_auth settings against OAUTHBEARER.
my @cases = (
	{ require_auth => "oauth" },
	{ require_auth => "oauth,scram-sha-256" },
	{ require_auth => "password,oauth" },
	{ require_auth => "none,oauth" },
	{ require_auth => "!scram-sha-256" },
	{ require_auth => "!none" },

	{
		require_auth => "!oauth",
		failure => qr/server requested OAUTHBEARER authentication/
	},
	{
		require_auth => "scram-sha-256",
		failure => qr/server requested OAUTHBEARER authentication/
	},
	{
		require_auth => "!password,!oauth",
		failure => qr/server requested OAUTHBEARER authentication/
	},
	{
		require_auth => "none",
		failure => qr/server requested SASL authentication/
	},
	{
		require_auth => "!oauth,!scram-sha-256",
		failure => qr/server requested SASL authentication/
	});

$user = "test";
foreach my $c (@cases)
{
	my $connstr =
	  "user=$user dbname=postgres oauth_issuer=$issuer oauth_client_id=f02c6361-0635 require_auth=$c->{'require_auth'}";

	if (defined $c->{'failure'})
	{
		$node->connect_fails(
			$connstr,
			"require_auth=$c->{'require_auth'} fails",
			expected_stderr => $c->{'failure'});
	}
	else
	{
		$node->connect_ok(
			$connstr,
			"require_auth=$c->{'require_auth'} succeeds",
			expected_stderr =>
			  qr@Visit https://example\.com/ and enter the code: postgresuser@
		);
	}
}

# Make sure the client_id and secret are correctly encoded. $vschars contains
# every allowed character for a client_id/_secret (the "VSCHAR" class).
# $vschars_esc is additionally backslash-escaped for inclusion in a
# single-quoted connection string.
my $vschars =
  " !\"#\$%&'()*+,-./0123456789:;<=>?\@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
my $vschars_esc =
  " !\"#\$%&\\'()*+,-./0123456789:;<=>?\@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";

$node->connect_ok(
	"user=$user dbname=postgres oauth_issuer=$issuer oauth_client_id='$vschars_esc'",
	"escapable characters: client_id",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);
$node->connect_ok(
	"user=$user dbname=postgres oauth_issuer=$issuer oauth_client_id='$vschars_esc' oauth_client_secret='$vschars_esc'",
	"escapable characters: client_id and secret",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);

#
# Further tests rely on support for specific behaviors in oauth_server.py. To
# trigger these behaviors, we ask for the special issuer .../param (which is set
# up in HBA for the testparam user) and encode magic instructions into the
# oauth_client_id.
#

my $common_connstr =
  "user=testparam dbname=postgres oauth_issuer=$issuer/param ";
my $base_connstr = $common_connstr;

sub connstr
{
	my (%params) = @_;

	my $json = encode_json(\%params);
	my $encoded = encode_base64($json, "");

	return "$base_connstr oauth_client_id=$encoded";
}

# Make sure the param system works end-to-end first.
$node->connect_ok(
	connstr(),
	"connect to /param",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);

$node->connect_ok(
	connstr(stage => 'token', retries => 1),
	"token retry",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);
$node->connect_ok(
	connstr(stage => 'token', retries => 2),
	"token retry (twice)",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);
$node->connect_ok(
	connstr(stage => 'all', retries => 1, interval => 2),
	"token retry (two second interval)",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);
$node->connect_ok(
	connstr(stage => 'all', retries => 1, interval => JSON::PP::null),
	"token retry (default interval)",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);

$node->connect_ok(
	connstr(stage => 'all', content_type => 'application/json;charset=utf-8'),
	"content type with charset",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);
$node->connect_ok(
	connstr(
		stage => 'all',
		content_type => "application/json \t;\t charset=utf-8"),
	"content type with charset (whitespace)",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);
$node->connect_ok(
	connstr(stage => 'device', uri_spelling => "verification_url"),
	"alternative spelling of verification_uri",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);

$node->connect_fails(
	connstr(stage => 'device', huge_response => JSON::PP::true),
	"bad device authz response: overlarge JSON",
	expected_stderr =>
	  qr/failed to obtain device authorization: response is too large/);
$node->connect_fails(
	connstr(stage => 'token', huge_response => JSON::PP::true),
	"bad token response: overlarge JSON",
	expected_stderr =>
	  qr/failed to obtain access token: response is too large/);

$node->connect_fails(
	connstr(stage => 'device', content_type => 'text/plain'),
	"bad device authz response: wrong content type",
	expected_stderr =>
	  qr/failed to parse device authorization: unexpected content type/);
$node->connect_fails(
	connstr(stage => 'token', content_type => 'text/plain'),
	"bad token response: wrong content type",
	expected_stderr =>
	  qr/failed to parse access token response: unexpected content type/);
$node->connect_fails(
	connstr(stage => 'token', content_type => 'application/jsonx'),
	"bad token response: wrong content type (correct prefix)",
	expected_stderr =>
	  qr/failed to parse access token response: unexpected content type/);

$node->connect_fails(
	connstr(
		stage => 'all',
		interval => ~0,
		retries => 1,
		retry_code => "slow_down"),
	"bad token response: server overflows the device authz interval",
	expected_stderr =>
	  qr/failed to obtain access token: slow_down interval overflow/);

$node->connect_fails(
	connstr(stage => 'token', error_code => "invalid_grant"),
	"bad token response: invalid_grant, no description",
	expected_stderr => qr/failed to obtain access token: \(invalid_grant\)/);
$node->connect_fails(
	connstr(
		stage => 'token',
		error_code => "invalid_grant",
		error_desc => "grant expired"),
	"bad token response: expired grant",
	expected_stderr =>
	  qr/failed to obtain access token: grant expired \(invalid_grant\)/);
$node->connect_fails(
	connstr(
		stage => 'token',
		error_code => "invalid_client",
		error_status => 401),
	"bad token response: client authentication failure, default description",
	expected_stderr =>
	  qr/failed to obtain access token: provider requires client authentication, and no oauth_client_secret is set \(invalid_client\)/
);
$node->connect_fails(
	connstr(
		stage => 'token',
		error_code => "invalid_client",
		error_status => 401,
		error_desc => "authn failure"),
	"bad token response: client authentication failure, provided description",
	expected_stderr =>
	  qr/failed to obtain access token: authn failure \(invalid_client\)/);

$node->connect_fails(
	connstr(stage => 'token', token => ""),
	"server rejects access: empty token",
	expected_stderr => qr/bearer authentication failed/);
$node->connect_fails(
	connstr(stage => 'token', token => "****"),
	"server rejects access: invalid token contents",
	expected_stderr => qr/bearer authentication failed/);

# Test behavior of the oauth_client_secret.
$base_connstr = "$common_connstr oauth_client_secret=''";

$node->connect_ok(
	connstr(stage => 'all', expected_secret => ''),
	"empty oauth_client_secret",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);

$base_connstr = "$common_connstr oauth_client_secret='$vschars_esc'";

$node->connect_ok(
	connstr(stage => 'all', expected_secret => $vschars),
	"nonempty oauth_client_secret",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);

$node->connect_fails(
	connstr(
		stage => 'token',
		error_code => "invalid_client",
		error_status => 401),
	"bad token response: client authentication failure, default description with oauth_client_secret",
	expected_stderr =>
	  qr/failed to obtain access token: provider rejected the oauth_client_secret \(invalid_client\)/
);
$node->connect_fails(
	connstr(
		stage => 'token',
		error_code => "invalid_client",
		error_status => 401,
		error_desc => "mutual TLS required for client"),
	"bad token response: client authentication failure, provided description with oauth_client_secret",
	expected_stderr =>
	  qr/failed to obtain access token: mutual TLS required for client \(invalid_client\)/
);

# Stress test: make sure our builtin flow operates correctly even if the client
# application isn't respecting PGRES_POLLING_READING/WRITING signals returned
# from PQconnectPoll().
$base_connstr =
  "$common_connstr port=" . $node->port . " host=" . $node->host;
my @cmd = (
	"oauth_hook_client", "--no-hook", "--stress-async",
	connstr(stage => 'all', retries => 1, interval => 1));

note "running '" . join("' '", @cmd) . "'";
my ($stdout, $stderr) = run_command(\@cmd);

like($stdout, qr/connection succeeded/, "stress-async: stdout matches");
unlike(
	$stderr,
	qr/connection to database failed/,
	"stress-async: stderr matches");

#
# This section of tests reconfigures the validator module itself, rather than
# the OAuth server.
#

# Searching the logs is easier if OAuth parameter discovery isn't cluttering
# things up; hardcode the discovery URI. (Scope is hardcoded to empty to cover
# that case as well.)
$common_connstr =
  "dbname=postgres oauth_issuer=$issuer/.well-known/openid-configuration oauth_scope='' oauth_client_id=f02c6361-0635";

# Misbehaving validators must fail shut.
$bgconn->query_safe("ALTER SYSTEM SET oauth_validator.authn_id TO ''");
$node->reload;
$log_start =
  $node->wait_for_log(qr/reloading configuration files/, $log_start);

$node->connect_fails(
	"$common_connstr user=test",
	"validator must set authn_id",
	expected_stderr => qr/OAuth bearer authentication failed/,
	log_like => [
		qr/connection authenticated: identity=""/,
		qr/DETAIL:\s+Validator provided no identity/,
		qr/FATAL:\s+OAuth bearer authentication failed/,
	]);

# Even if a validator authenticates the user, if the token isn't considered
# valid, the connection fails.
$bgconn->query_safe(
	"ALTER SYSTEM SET oauth_validator.authn_id TO 'test\@example.org'");
$bgconn->query_safe(
	"ALTER SYSTEM SET oauth_validator.authorize_tokens TO false");
$node->reload;
$log_start =
  $node->wait_for_log(qr/reloading configuration files/, $log_start);

$node->connect_fails(
	"$common_connstr user=test",
	"validator must authorize token explicitly",
	expected_stderr => qr/OAuth bearer authentication failed/,
	log_like => [
		qr/connection authenticated: identity="test\@example\.org"/,
		qr/DETAIL:\s+Validator failed to authorize the provided token/,
		qr/FATAL:\s+OAuth bearer authentication failed/,
	]);

#
# Test user mapping.
#

# Allow "user@example.com" to log in under the test role.
unlink($node->data_dir . '/pg_ident.conf');
$node->append_conf(
	'pg_ident.conf', qq{
oauthmap	user\@example.com	test
});

# test and testalt use the map; testparam uses ident delegation.
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf', qq{
local all test      oauth issuer="$issuer" scope="" map=oauthmap
local all testalt   oauth issuer="$issuer" scope="" map=oauthmap
local all testparam oauth issuer="$issuer" scope="" delegate_ident_mapping=1
});

# To start, have the validator use the role names as authn IDs.
$bgconn->query_safe("ALTER SYSTEM RESET oauth_validator.authn_id");
$bgconn->query_safe("ALTER SYSTEM RESET oauth_validator.authorize_tokens");

$node->reload;
$log_start =
  $node->wait_for_log(qr/reloading configuration files/, $log_start);

# The test and testalt roles should no longer map correctly.
$node->connect_fails(
	"$common_connstr user=test",
	"mismatched username map (test)",
	expected_stderr => qr/OAuth bearer authentication failed/);
$node->connect_fails(
	"$common_connstr user=testalt",
	"mismatched username map (testalt)",
	expected_stderr => qr/OAuth bearer authentication failed/);

# Have the validator identify the end user as user@example.com.
$bgconn->query_safe(
	"ALTER SYSTEM SET oauth_validator.authn_id TO 'user\@example.com'");
$node->reload;
$log_start =
  $node->wait_for_log(qr/reloading configuration files/, $log_start);

# Now the test role can be logged into. (testalt still can't be mapped.)
$node->connect_ok(
	"$common_connstr user=test",
	"matched username map (test)",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);
$node->connect_fails(
	"$common_connstr user=testalt",
	"mismatched username map (testalt)",
	expected_stderr => qr/OAuth bearer authentication failed/);

# testparam ignores the map entirely.
$node->connect_ok(
	"$common_connstr user=testparam",
	"delegated ident (testparam)",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@);

$bgconn->query_safe("ALTER SYSTEM RESET oauth_validator.authn_id");
$node->reload;
$log_start =
  $node->wait_for_log(qr/reloading configuration files/, $log_start);

#
# Test multiple validators.
#

$node->append_conf('postgresql.conf',
	"oauth_validator_libraries = 'validator, fail_validator'\n");

# With multiple validators, every HBA line must explicitly declare one.
my $result = $node->restart(fail_ok => 1);
is($result, 0,
	'restart fails without explicit validators in oauth HBA entries');

$log_start = $node->wait_for_log(
	qr/authentication method "oauth" requires argument "validator" to be set/,
	$log_start);

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf', qq{
local all test    oauth validator=validator      issuer="$issuer"           scope="openid postgres"
local all testalt oauth validator=fail_validator issuer="$issuer/.well-known/oauth-authorization-server/alternate" scope="openid postgres alt"
});
$node->restart;

$log_start = $node->wait_for_log(qr/ready to accept connections/, $log_start);

# The test user should work as before.
$user = "test";
$node->connect_ok(
	"user=$user dbname=postgres oauth_issuer=$issuer oauth_client_id=f02c6361-0635",
	"validator is used for $user",
	expected_stderr =>
	  qr@Visit https://example\.com/ and enter the code: postgresuser@,
	log_like => [qr/connection authorized/]);

# testalt should be routed through the fail_validator.
$user = "testalt";
$node->connect_fails(
	"user=$user dbname=postgres oauth_issuer=$issuer/.well-known/oauth-authorization-server/alternate oauth_client_id=f02c6361-0636",
	"fail_validator is used for $user",
	expected_stderr => qr/FATAL:\s+fail_validator: sentinel error/);

#
# Test ABI compatibility magic marker
#
$node->append_conf('postgresql.conf',
	"oauth_validator_libraries = 'magic_validator'\n");
unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf', qq{
local all test    oauth validator=magic_validator      issuer="$issuer"           scope="openid postgres"
});
$node->restart;

$log_start = $node->wait_for_log(qr/ready to accept connections/, $log_start);

$node->connect_fails(
	"user=test dbname=postgres oauth_issuer=$issuer/.well-known/oauth-authorization-server/alternate oauth_client_id=f02c6361-0636",
	"magic_validator is used for $user",
	expected_stderr =>
	  qr/FATAL:\s+OAuth validator module "magic_validator": magic number mismatch/
);
$node->stop;

done_testing();
