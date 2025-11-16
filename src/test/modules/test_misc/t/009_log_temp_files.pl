# Copyright (c) 2025, PostgreSQL Global Development Group

# Check how temporary file removals and statement queries are associated
# in the server logs for various query sequences with the simple and
# extended query protocols.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize a new PostgreSQL test cluster
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init();
$node->append_conf(
	'postgresql.conf', qq(
work_mem = 64kB
log_temp_files = 0
debug_parallel_query = off
log_error_verbosity = default
));
$node->start;

# Setup table and populate with data
$node->safe_psql(
	"postgres", qq{
CREATE UNLOGGED TABLE foo(a int);
INSERT INTO foo(a) SELECT * FROM generate_series(1, 5000);
});

note "unnamed portal: temporary file dropped under second SELECT query";
my $log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
BEGIN;
SELECT a FROM foo ORDER BY a OFFSET \$1 \\bind 4990 \\g
SELECT 'unnamed portal';
END;
});
ok( $node->log_contains(
		qr/LOG:\s+temporary file: path.*\n.*\ STATEMENT:\s+SELECT 'unnamed portal'/s,
		$log_offset),
	"unnamed portal");

note "bind and implicit transaction: temporary file dropped without query";
$log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
SELECT a FROM foo ORDER BY a OFFSET \$1 \\bind 4991 \\g
});
ok( $node->log_contains(qr/LOG:\s+temporary file:/s, $log_offset),
	"bind and implicit transaction, temporary file removed");
ok( !$node->log_contains(qr/STATEMENT:/s, $log_offset),
	"bind and implicit transaction, no statement logged");

note "named portal: temporary file dropped under second SELECT query";
$node->safe_psql(
	"postgres", qq{
BEGIN;
SELECT a FROM foo ORDER BY a OFFSET \$1 \\parse stmt
\\bind_named stmt 4999 \\g
SELECT 'named portal';
END;
});
ok( $node->log_contains(
		qr/LOG:\s+temporary file: path.*\n.*\ STATEMENT:\s+SELECT 'named portal'/s,
		$log_offset),
	"named portal");

note "pipelined query: temporary file dropped under second SELECT query";
$log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
\\startpipeline
SELECT a FROM foo ORDER BY a OFFSET \$1 \\bind 4992 \\sendpipeline
SELECT 'pipelined query';
\\endpipeline
});
ok( $node->log_contains(
		qr/LOG:\s+temporary file: path.*\n.*\ STATEMENT:\s+SELECT 'pipelined query'/s,
		$log_offset),
	"pipelined query");

note "parse and bind: temporary file dropped without query";
$log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
SELECT a, a, a FROM foo ORDER BY a OFFSET \$1 \\parse p1
\\bind_named p1 4993 \\g
});
ok($node->log_contains(qr/LOG:\s+temporary file:/s, $log_offset),
	"parse and bind, temporary file removed");
ok(!$node->log_contains(qr/STATEMENT:/s, $log_offset),
	"bind and bind, no statement logged");

note "simple query: temporary file dropped under SELECT query";
$log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
BEGIN;
SELECT a FROM foo ORDER BY a OFFSET 4994;
END;
});
ok( $node->log_contains(
		qr/LOG:\s+temporary file: path.*\n.*\ STATEMENT:\s+SELECT a FROM foo ORDER BY a OFFSET 4994;/s,
		$log_offset),
	"simple query");

note "cursor: temporary file dropped under CLOSE";
$log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
BEGIN;
DECLARE mycur CURSOR FOR SELECT a FROM foo ORDER BY a OFFSET 4995;
FETCH 10 FROM mycur;
SELECT 1;
CLOSE mycur;
END;
});
ok( $node->log_contains(
		qr/LOG:\s+temporary file: path.*\n.*\ STATEMENT:\s+CLOSE mycur;/s,
		$log_offset),
	"cursor");

note "cursor WITH HOLD: temporary file dropped under COMMIT";
$log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
BEGIN;
DECLARE holdcur CURSOR WITH HOLD FOR SELECT a FROM foo ORDER BY a OFFSET 4996;
FETCH 10 FROM holdcur;
COMMIT;
CLOSE holdcur;
});
ok( $node->log_contains(
		qr/LOG:\s+temporary file: path.*\n.*\ STATEMENT:\s+COMMIT;/s,
		$log_offset),
	"cursor WITH HOLD");

note "prepare/execute: temporary file dropped under EXECUTE";
$log_offset = -s $node->logfile;
$node->safe_psql(
	"postgres", qq{
BEGIN;
PREPARE p1 AS SELECT a FROM foo ORDER BY a OFFSET 4997;
EXECUTE p1;
DEALLOCATE p1;
END;
});
ok( $node->log_contains(
		qr/LOG:\s+temporary file: path.*\n.*\ STATEMENT:\s+EXECUTE p1;/s,
		$log_offset),
	"prepare/execute");

$node->stop('fast');
done_testing();
