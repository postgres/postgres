use strict;
use warnings;

use PostgresNode;
use TestLib;
use Test::More tests => 6;

my $tempdir       = TestLib::tempdir;
my $tempdir_short = TestLib::tempdir_short;

my $node = get_new_node('main');
my $port = $node->port;

$node->init;
$node->start;

#########################################
# pg_dumpall: newline in database name

$node->safe_psql('postgres', qq{CREATE DATABASE "regress_\nattack"});

my (@cmd, $stdout, $stderr);
@cmd = ("pg_dumpall", '--port' => $port, '--exclude-database=postgres');
print("# Running: " . join(" ", @cmd) . "\n");
my $result = IPC::Run::run \@cmd, '>' => \$stdout, '2>' => \$stderr;
ok(!$result, "newline in dbname: exit code not 0");
like(
	$stderr,
	qr/shell command argument contains a newline/,
	"newline in dbname: stderr matches");
unlike($stdout, qr/^attack/m, "newline in dbname: no comment escape");

#########################################
# Verify that dumping foreign data includes only foreign tables of
# matching servers

$node->safe_psql('postgres', "CREATE FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s0 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s1 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE SERVER s2 FOREIGN DATA WRAPPER dummy");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t0 (a int) SERVER s0");
$node->safe_psql('postgres', "CREATE FOREIGN TABLE t1 (a int) SERVER s1");

command_fails_like(
	[ "pg_dump", '-p', $port, '--include-foreign-data=s0', 'postgres' ],
	qr/foreign-data wrapper \"dummy\" has no handler\r?\npg_dump: error: query was:.*t0/,
	"correctly fails to dump a foreign table from a dummy FDW");

command_ok(
	[ "pg_dump", '-p', $port, '-a', '--include-foreign-data=s2', 'postgres' ],
	"dump foreign server with no tables");
