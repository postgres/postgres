
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test postmaster start and stop state machine.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

#
# Test that dead-end backends don't prevent the server from shutting
# down.
#
# Dead-end backends can linger until they reach
# authentication_timeout. We use a long authentication_timeout and a
# much shorter timeout for the "pg_ctl stop" operation, to test that
# if dead-end backends are killed at fast shut down. If they're not,
# "pg_ctl stop" will error out before the authentication timeout kicks
# in and cleans up the dead-end backends.
my $authentication_timeout = $PostgreSQL::Test::Utils::timeout_default;
my $stop_timeout = $authentication_timeout / 2;

# Initialize the server with low connection limits, to test dead-end backends
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "max_connections = 5");
$node->append_conf('postgresql.conf', "max_wal_senders = 0");
$node->append_conf('postgresql.conf', "autovacuum_max_workers = 1");
$node->append_conf('postgresql.conf', "max_worker_processes = 1");
$node->append_conf('postgresql.conf', "log_connections = on");
$node->append_conf('postgresql.conf', "log_min_messages = debug2");
$node->append_conf('postgresql.conf',
	"authentication_timeout = '$authentication_timeout s'");
$node->append_conf('postgresql.conf', 'trace_connection_negotiation=on');
$node->start;

if (!$node->raw_connect_works())
{
	plan skip_all => "this test requires working raw_connect()";
}

my @raw_connections = ();

# Open a lot of TCP (or Unix domain socket) connections to use up all
# the connection slots. Beyond a certain number (roughly 2x
# max_connections), they will be "dead-end backends".
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

# When all the connection slots are in use, new connections will fail
# before even looking up the user. Hence you now get "sorry, too many
# clients already" instead of "role does not exist" error. Test that
# to ensure that we have used up all the slots.
$node->connect_fails("dbname=postgres user=invalid_user",
	"connect ",
	expected_stderr => qr/FATAL:  sorry, too many clients already/);

# Open one more connection, to really ensure that we have at least one
# dead-end backend.
my $sock = $node->raw_connect();

# Test that the dead-end backends don't prevent the server from stopping.
$node->stop('fast', timeout => $stop_timeout);

$node->start();
$node->connect_ok("dbname=postgres", "works after restart");

# Clean up
foreach my $socket (@raw_connections)
{
	$socket->close();
}

done_testing();
