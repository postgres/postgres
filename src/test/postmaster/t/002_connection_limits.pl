
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test connection limits, i.e. max_connections, reserved_connections
# and superuser_reserved_connections.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize the server with specific low connection limits
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(
	auth_extra => [
		'--create-role' =>
		  'regress_regular,regress_reserved,regress_superuser',
	]);
$node->append_conf('postgresql.conf', "max_connections = 6");
$node->append_conf('postgresql.conf', "reserved_connections = 2");
$node->append_conf('postgresql.conf', "superuser_reserved_connections = 1");
$node->append_conf('postgresql.conf',
	"log_connections = 'receipt,authentication,authorization'");
$node->append_conf('postgresql.conf', "log_min_messages=debug2");
$node->start;

$node->safe_psql(
	'postgres', qq{
CREATE USER regress_regular LOGIN;
CREATE USER regress_reserved LOGIN;
GRANT pg_use_reserved_connections TO regress_reserved;
CREATE USER regress_superuser LOGIN SUPERUSER;
});

# With the limits we set in postgresql.conf, we can establish:
# - 3 connections for any user with no special privileges
# - 2 more connections for users belonging to "pg_use_reserved_connections"
# - 1 more connection for superuser

sub background_psql_as_user
{
	my $user = shift;

	return $node->background_psql(
		'postgres',
		on_error_die => 1,
		extra_params => [ '--username' => $user ]);
}

# Like connect_fails(), except that we also wait for the failed backend to
# have exited.
#
# This tests needs to wait for client processes to exit because the error
# message for a failed connection is reported before the backend has detached
# from shared memory. If we didn't wait, subsequent tests might hit connection
# limits spuriously.
#
# This can't easily be generalized, as detecting process exit requires
# log_min_messages to be at least DEBUG2 and is not concurrency safe, as we
# can't easily be sure the right process exited. In this test that's not a
# problem though, we only have one new connection at a time.
sub connect_fails_wait
{
	local $Test::Builder::Level = $Test::Builder::Level + 1;
	my ($node, $connstr, $test_name, %params) = @_;

	my $log_location = -s $node->logfile;

	$node->connect_fails($connstr, $test_name, %params);
	$node->wait_for_log(
		qr/DEBUG:  (00000: )?client backend.*exited with exit code 1/,
		$log_location);
	ok(1, "$test_name: client backend process exited");
}

my @sessions = ();
my @raw_connections = ();

push(@sessions, background_psql_as_user('regress_regular'));
push(@sessions, background_psql_as_user('regress_regular'));
push(@sessions, background_psql_as_user('regress_regular'));
connect_fails_wait(
	$node,
	"dbname=postgres user=regress_regular",
	"regular connections limit",
	expected_stderr =>
	  qr/FATAL:  remaining connection slots are reserved for roles with privileges of the "pg_use_reserved_connections" role/
);

push(@sessions, background_psql_as_user('regress_reserved'));
push(@sessions, background_psql_as_user('regress_reserved'));
connect_fails_wait(
	$node,
	"dbname=postgres user=regress_reserved",
	"reserved_connections limit",
	expected_stderr =>
	  qr/FATAL:  remaining connection slots are reserved for roles with the SUPERUSER attribute/
);

push(@sessions, background_psql_as_user('regress_superuser'));
connect_fails_wait(
	$node,
	"dbname=postgres user=regress_superuser",
	"superuser_reserved_connections limit",
	expected_stderr => qr/FATAL:  sorry, too many clients already/);

# We can still open TCP (or Unix domain socket) connections, but
# beyond a certain number (roughly 2x max_connections), they will be
# "dead-end backends".
SKIP:
{
	skip "this test requires working raw_connect()"
	  unless $node->raw_connect_works();

	for (my $i = 0; $i <= 20; $i++)
	{
		my $sock = $node->raw_connect();

		# On a busy system, the server might reject connections if
		# postmaster cannot accept() them fast enough. The exact limit
		# and behavior depends on the platform. To make this reliable,
		# we attempt SSL negotiation on each connection before opening
		# next one. The server will reject the SSL negotiations, but
		# when it does so, we know that the backend has been launched
		# and we should be able to open another connection.

		# SSLRequest packet consists of packet length followed by
		# NEGOTIATE_SSL_CODE.
		my $negotiate_ssl_code = pack("Nnn", 8, 1234, 5679);
		my $sent = $sock->send($negotiate_ssl_code);

		# Read reply. We expect the server to reject it with 'N'
		my $reply = "";
		$sock->recv($reply, 1);
		is($reply, "N", "dead-end connection $i");

		push(@raw_connections, $sock);
	}
}

# TODO: test that query cancellation is still possible. A dead-end
# backend can process a query cancellation packet.

# Clean up
foreach my $session (@sessions)
{
	$session->quit;
}
foreach my $socket (@raw_connections)
{
	$socket->close();
}

done_testing();
