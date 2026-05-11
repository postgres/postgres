# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the negotiation of combined SSL and GSS requests.  This test
# relies on both SSL and GSS requests to be rejected first, followed
# by more requests.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', "log_min_messages = debug2");
$node->append_conf('postgresql.conf',
	"log_connections = 'receipt,authentication,authorization'");
$node->append_conf('postgresql.conf', 'trace_connection_negotiation=on');
$node->start;

if (!$node->raw_connect_works())
{
	plan skip_all => "this test requires working raw_connect()";
}

my $sock = $node->raw_connect();

# SSLRequest: packet length followed by NEGOTIATE_SSL_CODE.
my $ssl_request = pack("Nnn", 8, 1234, 5679);

# GSSENCRequest: packet length followed by NEGOTIATE_GSS_CODE.
my $gss_request = pack("Nnn", 8, 1234, 5680);

# Send SSLRequest, reject or bypass.
$sock->send($ssl_request);
my $reply = "";
$sock->recv($reply, 1);
if ($reply ne 'N')
{
	$sock->close();
	plan skip_all =>
	  "server accepted SSL; test requires SSL to be rejected";
}

# Send GSSENCRequest, reject or bypass test.
$sock->send($gss_request);
$reply = "";
$sock->recv($reply, 1);
if ($reply ne 'N')
{
	$sock->close();
	plan skip_all =>
	  "server accepted GSS; test requires GSS to be rejected";
}

my $log_offset = -s $node->logfile;

# Send a second SSLRequest, now that we know that both SSL and GSS have
# been rejected for this connection.  We are done with both requests, so
# extra requests will be rejected and fail with an invalid protocol
# version, and the connection should be closed by the server.
$sock->send($ssl_request);

# Try to read a response, there should be nothing, and certainly not an
# extra 'N' message indicating a rejection.
$reply = "";
my $bytes = $sock->recv($reply, 1024);
isnt($reply, 'N',
	"server does not re-enter SSL negotiation after SSL+GSS were both tried");

$sock->close();
$node->wait_for_log(qr/FATAL: .* unsupported frontend protocol 1234.5679/,
					$log_offset);

# Check extra connection with a simple query.
my $result = $node->safe_psql('postgres', 'select 1;');
is($result, '1', 'server able to accept connection');

$node->stop;

done_testing();
