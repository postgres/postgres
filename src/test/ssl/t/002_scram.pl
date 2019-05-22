# Test SCRAM authentication and TLS channel binding types

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;

use File::Copy;

use FindBin;
use lib $FindBin::RealBin;

use SSLServer;

if ($ENV{with_openssl} ne 'yes')
{
	plan skip_all => 'SSL not supported by this build';
}

my $number_of_tests = 1;

# This is the hostname used to connect to the server.
my $SERVERHOSTADDR = '127.0.0.1';

# Allocation of base connection string shared among multiple tests.
my $common_connstr;

# Set up the server.

note "setting up data directory";
my $node = get_new_node('master');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

# Configure server for SSL connections, with password handling.
configure_test_server_for_ssl($node, $SERVERHOSTADDR, "scram-sha-256",
	"pass", "scram-sha-256");
switch_server_cert($node, 'server-cn-only');
$ENV{PGPASSWORD} = "pass";
$common_connstr =
  "user=ssltestuser dbname=trustdb sslmode=require sslcert=invalid sslrootcert=invalid hostaddr=$SERVERHOSTADDR";

# Default settings
test_connect_ok($common_connstr, '', "Basic SCRAM authentication with SSL");

done_testing($number_of_tests);
