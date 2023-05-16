
# Copyright (c) 2021-2023, PostgreSQL Global Development Group

# Tests targeting SSPI on Windows.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (!$windows_os || $use_unix_sockets)
{
	plan skip_all =>
	  "SSPI tests require Windows (without PG_TEST_USE_UNIX_SOCKETS)";
}

# Initialize primary node
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf('postgresql.conf', "log_connections = on\n");
$node->start;

# SSPI is set up by default.  Make sure it interacts correctly with
# require_auth.
$node->connect_ok("require_auth=sspi",
	"SSPI authentication required, works with SSPI auth");
$node->connect_fails(
	"require_auth=!sspi",
	"SSPI authentication forbidden, fails with SSPI auth",
	expected_stderr =>
	  qr/authentication method requirement "!sspi" failed: server requested SSPI authentication/
);
$node->connect_fails(
	"require_auth=scram-sha-256",
	"SCRAM authentication required, fails with SSPI auth",
	expected_stderr =>
	  qr/authentication method requirement "scram-sha-256" failed: server requested SSPI authentication/
);

done_testing();
