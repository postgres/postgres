use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 3;

my $tempdir       = TestLib::tempdir;
my $tempdir_short = TestLib::tempdir_short;

my $node = get_new_node('main');
my $port = $node->port;

$node->init;
$node->start;

#########################################
# Verify that dumping foreign data includes only foreign tables of
# matching servers

$node->safe_psql('postgres', "CREATE FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s0 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s1 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s2 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t0 (a int) SERVER s0");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t1 (a int) SERVER s1");
my ($cmd, $stdout, $stderr, $result);

command_fails_like(
	[ "pg_dump", '-p', $port, '--include-foreign-data=s0', 'postgres' ],
	qr/foreign-data wrapper \"dummy\" has no handler\r?\npg_dump: error: query was:.*t0/,
	"correctly fails to dump a foreign table from a dummy FDW");

command_ok(
	[ "pg_dump", '-p', $port, '-a', '--include-foreign-data=s2', 'postgres' ],
	"dump foreign server with no tables");
