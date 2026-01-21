
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test CREATE INDEX CONCURRENTLY with concurrent modifications
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node;

#
# Test set-up
#
$node = PostgreSQL::Test::Cluster->new('CIC_test');
$node->init;
$node->append_conf('postgresql.conf',
	'lock_timeout = ' . (1000 * $PostgreSQL::Test::Utils::timeout_default));
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE TABLE tbl(i int, j jsonb)));
$node->safe_psql('postgres', q(CREATE INDEX idx ON tbl(i)));
$node->safe_psql('postgres', q(CREATE INDEX ginidx ON tbl USING gin(j)));

#
# Stress CIC with pgbench.
#
# pgbench might try to launch more than one instance of the CIC
# transaction concurrently.  That would deadlock, so use an advisory
# lock to ensure only one CIC runs at a time.
#
$node->pgbench(
	'--no-vacuum --client=5 --transactions=100',
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent INSERTs and CIC',
	{
		'002_pgbench_concurrent_transaction' => q(
			BEGIN;
			INSERT INTO tbl VALUES(0, '{"a":[["b",{"x":1}],["b",{"x":2}]],"c":3}');
			COMMIT;
		  ),
		'002_pgbench_concurrent_transaction_savepoints' => q(
			BEGIN;
			SAVEPOINT s1;
			INSERT INTO tbl VALUES(0, '[[14,2,3]]');
			COMMIT;
		  ),
		'002_pgbench_concurrent_cic' => q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				DROP INDEX CONCURRENTLY idx;
				CREATE INDEX CONCURRENTLY idx ON tbl(i);
				DROP INDEX CONCURRENTLY ginidx;
				CREATE INDEX CONCURRENTLY ginidx ON tbl USING gin(j);
				SELECT bt_index_check('idx',true);
				SELECT gin_index_check('ginidx');
				SELECT pg_advisory_unlock(42);
			\endif
		  )
	});

# Test bt_index_parent_check() with indexes created with
# CREATE INDEX CONCURRENTLY.
$node->safe_psql('postgres', q(CREATE TABLE quebec(i int primary key)));
# Insert two rows into index
$node->safe_psql('postgres',
	q(INSERT INTO quebec SELECT i FROM generate_series(1, 2) s(i);));

# start background transaction
my $in_progress_h = $node->background_psql('postgres');
$in_progress_h->query_safe(q(BEGIN; SELECT pg_current_xact_id();));

# delete one row from table, while background transaction is in progress
$node->safe_psql('postgres', q(DELETE FROM quebec WHERE i = 1;));
# create index concurrently, which will skip the deleted row
$node->safe_psql('postgres',
	q(CREATE INDEX CONCURRENTLY oscar ON quebec(i);));

# check index using bt_index_parent_check
my $result = $node->psql('postgres',
	q(SELECT bt_index_parent_check('oscar', heapallindexed => true)));
is($result, '0', 'bt_index_parent_check for CIC after removed row');

$in_progress_h->quit;

$node->stop;
done_testing();
