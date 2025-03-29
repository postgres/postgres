
# Copyright (c) 2021-2025, PostgreSQL Global Development Group

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

$node->stop;
done_testing();
