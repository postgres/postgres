
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

# Test CREATE INDEX CONCURRENTLY with concurrent prepared-xact modifications
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

Test::More->builder->todo_start('filesystem bug')
  if PostgreSQL::Test::Utils::has_wal_read_bug;

my ($node, $result);

#
# Test set-up
#
$node = PostgreSQL::Test::Cluster->new('CIC_2PC_test');
$node->init;
$node->append_conf('postgresql.conf', 'max_prepared_transactions = 10');
$node->append_conf('postgresql.conf',
	'lock_timeout = ' . (1000 * $PostgreSQL::Test::Utils::timeout_default));
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE TABLE tbl(i int, j jsonb)));


#
# Run 3 overlapping 2PC transactions with CIC
#
# We have two concurrent background psql processes: $main_h for INSERTs and
# $cic_h for CIC.  Also, we use non-background psql for some COMMIT PREPARED
# statements.
#

my $main_h = $node->background_psql('postgres');

$main_h->query_safe(
	q(
BEGIN;
INSERT INTO tbl VALUES(0, '[[14,2,3]]');
));

my $cic_h = $node->background_psql('postgres');

$cic_h->query_until(
	qr/start/, q(
\echo start
CREATE INDEX CONCURRENTLY idx ON tbl(i);
CREATE INDEX CONCURRENTLY ginidx ON tbl USING gin(j);
));

$main_h->query_safe(
	q(
PREPARE TRANSACTION 'a';
));

$main_h->query_safe(
	q(
BEGIN;
INSERT INTO tbl VALUES(0, '[[14,2,3]]');
));

$node->safe_psql('postgres', q(COMMIT PREPARED 'a';));

$main_h->query_safe(
	q(
PREPARE TRANSACTION 'b';
BEGIN;
INSERT INTO tbl VALUES(0, '"mary had a little lamb"');
));

$node->safe_psql('postgres', q(COMMIT PREPARED 'b';));

$main_h->query_safe(
	q(
PREPARE TRANSACTION 'c';
COMMIT PREPARED 'c';
));

$main_h->quit;
$cic_h->quit;

$result = $node->psql('postgres', q(SELECT bt_index_check('idx',true)));
is($result, '0', 'bt_index_check after overlapping 2PC');

$result = $node->psql('postgres', q(SELECT gin_index_check('ginidx')));
is($result, '0', 'gin_index_check after overlapping 2PC');


#
# Server restart shall not change whether prepared xact blocks CIC
#

$node->safe_psql(
	'postgres', q(
BEGIN;
INSERT INTO tbl VALUES(0, '{"a":[["b",{"x":1}],["b",{"x":2}]],"c":3}');
PREPARE TRANSACTION 'spans_restart';
BEGIN;
CREATE TABLE unused ();
PREPARE TRANSACTION 'persists_forever';
));
$node->restart;

my $reindex_h = $node->background_psql('postgres');
$reindex_h->query_until(
	qr/start/, q(
\echo start
DROP INDEX CONCURRENTLY idx;
CREATE INDEX CONCURRENTLY idx ON tbl(i);
DROP INDEX CONCURRENTLY ginidx;
CREATE INDEX CONCURRENTLY ginidx ON tbl USING gin(j);
));

$node->safe_psql('postgres', "COMMIT PREPARED 'spans_restart'");
$reindex_h->quit;
$result = $node->psql('postgres', q(SELECT bt_index_check('idx',true)));
is($result, '0', 'bt_index_check after 2PC and restart');
$result = $node->psql('postgres', q(SELECT gin_index_check('ginidx')));
is($result, '0', 'gin_index_check after 2PC and restart');


#
# Stress CIC+2PC with pgbench
#
# pgbench might try to launch more than one instance of the CIC
# transaction concurrently.  That would deadlock, so use an advisory
# lock to ensure only one CIC runs at a time.

# Fix broken index first
$node->safe_psql('postgres', q(REINDEX TABLE tbl;));

# Run pgbench.
$node->pgbench(
	'--no-vacuum --client=5 --transactions=100',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent INSERTs w/ 2PC and CIC',
	{
		'003_pgbench_concurrent_2pc' => q(
			BEGIN;
			INSERT INTO tbl VALUES(0,'null');
			PREPARE TRANSACTION 'c:client_id';
			COMMIT PREPARED 'c:client_id';
		  ),
		'003_pgbench_concurrent_2pc_savepoint' => q(
			BEGIN;
			SAVEPOINT s1;
			INSERT INTO tbl VALUES(0,'[false, "jnvaba", -76, 7, {"_": [1]}, 9]');
			PREPARE TRANSACTION 'c:client_id';
			COMMIT PREPARED 'c:client_id';
		  ),
		'003_pgbench_concurrent_cic' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				DROP INDEX CONCURRENTLY idx;
				CREATE INDEX CONCURRENTLY idx ON tbl(i);
				SELECT bt_index_check('idx',true);
				SELECT pg_advisory_unlock(42);
			\endif
		  ),
		'004_pgbench_concurrent_ric' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				REINDEX INDEX CONCURRENTLY idx;
				SELECT bt_index_check('idx',true);
				SELECT pg_advisory_unlock(42);
			\endif
		  ),
		'005_pgbench_concurrent_cic' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotginlock \gset
			\if :gotginlock
				DROP INDEX CONCURRENTLY ginidx;
				CREATE INDEX CONCURRENTLY ginidx ON tbl USING gin(j);
				SELECT gin_index_check('ginidx');
				SELECT pg_advisory_unlock(42);
			\endif
		  ),
		'006_pgbench_concurrent_ric' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotginlock \gset
			\if :gotginlock
				REINDEX INDEX CONCURRENTLY ginidx;
				SELECT gin_index_check('ginidx');
				SELECT pg_advisory_unlock(42);
			\endif
		  )

	});

$node->stop;
done_testing();
