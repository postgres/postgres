#
# Exercises the API for custom OAuth client flows, using the oauth_hook_client
# test driver.
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

if (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\boauth\b/)
{
	plan skip_all =>
	  'Potentially unsafe test oauth not enabled in PG_TEST_EXTRA';
}

#
# Cluster Setup
#

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->append_conf('postgresql.conf',
	"oauth_validator_libraries = 'validator'\n");
$node->start;

$node->safe_psql('postgres', 'CREATE USER test;');

# These tests don't use the builtin flow, and we don't have an authorization
# server running, so the address used here shouldn't matter. Use an invalid IP
# address, so if there's some cascade of errors that causes the client to
# attempt a connection, we'll fail noisily.
my $issuer = "https://256.256.256.256";
my $scope = "openid postgres";

unlink($node->data_dir . '/pg_hba.conf');
$node->append_conf(
	'pg_hba.conf', qq{
local all test oauth issuer="$issuer" scope="$scope"
});
$node->reload;

my ($log_start, $log_end);
$log_start = $node->wait_for_log(qr/reloading configuration files/);

$ENV{PGOAUTHDEBUG} = "UNSAFE";

#
# Tests
#

my $user = "test";
my $base_connstr = $node->connstr() . " user=$user";
my $common_connstr =
  "$base_connstr oauth_issuer=$issuer oauth_client_id=myID";

sub test
{
	my ($test_name, %params) = @_;

	my $flags = [];
	if (defined($params{flags}))
	{
		$flags = $params{flags};
	}

	my @cmd = ("oauth_hook_client", @{$flags}, $common_connstr);
	note "running '" . join("' '", @cmd) . "'";

	my ($stdout, $stderr) = run_command(\@cmd);

	if (defined($params{expected_stdout}))
	{
		like($stdout, $params{expected_stdout}, "$test_name: stdout matches");
	}

	if (defined($params{expected_stderr}))
	{
		like($stderr, $params{expected_stderr}, "$test_name: stderr matches");
	}
	else
	{
		is($stderr, "", "$test_name: no stderr");
	}
}

test(
	"basic synchronous hook can provide a token",
	flags => [
		"--token", "my-token",
		"--expected-uri", "$issuer/.well-known/openid-configuration",
		"--expected-scope", $scope,
	],
	expected_stdout => qr/connection succeeded/);

$node->log_check("validator receives correct token",
	$log_start,
	log_like => [ qr/oauth_validator: token="my-token", role="$user"/, ]);

if ($ENV{with_libcurl} ne 'yes')
{
	# libpq should help users out if no OAuth support is built in.
	test(
		"fails without custom hook installed",
		flags => ["--no-hook"],
		expected_stderr =>
		  qr/no custom OAuth flows are available, and libpq was not built with libcurl support/
	);
}

# connect_timeout should work if the flow doesn't respond.
$common_connstr = "$common_connstr connect_timeout=1";
test(
	"connect_timeout interrupts hung client flow",
	flags => ["--hang-forever"],
	expected_stderr => qr/failed: timeout expired/);

# Test various misbehaviors of the client hook.
my @cases = (
	{
		flag => "--misbehave=no-hook",
		expected_error =>
		  qr/user-defined OAuth flow provided neither a token nor an async callback/,
	},
	{
		flag => "--misbehave=fail-async",
		expected_error => qr/user-defined OAuth flow failed/,
	},
	{
		flag => "--misbehave=no-token",
		expected_error => qr/user-defined OAuth flow did not provide a token/,
	},
	{
		flag => "--misbehave=no-socket",
		expected_error =>
		  qr/user-defined OAuth flow did not provide a socket for polling/,
	});

foreach my $c (@cases)
{
	test(
		"hook misbehavior: $c->{'flag'}",
		flags => [ $c->{'flag'} ],
		expected_stderr => $c->{'expected_error'});
}

done_testing();
