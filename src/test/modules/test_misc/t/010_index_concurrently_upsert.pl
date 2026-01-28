
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test INSERT ON CONFLICT DO UPDATE behavior concurrent with
# CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY.
#
# These tests verify the fix for "duplicate key value violates unique
# constraint" errors that occurred when infer_arbiter_indexes() only considered
# indisvalid indexes, causing different transactions to use different arbiter
# indexes.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

# Node initialization
my $node = PostgreSQL::Test::Cluster->new('node');
$node->init();
$node->start();

# Check if the extension injection_points is available
plan skip_all => 'Extension injection_points not installed'
  unless $node->check_extension('injection_points');

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

$node->safe_psql(
	'postgres', q[
CREATE SCHEMA test;
CREATE UNLOGGED TABLE test.tblpk (i int PRIMARY KEY, updated_at timestamp);
ALTER TABLE test.tblpk SET (parallel_workers=0);

CREATE TABLE test.tblparted(i int primary key, updated_at timestamp) PARTITION BY RANGE (i);
CREATE TABLE test.tbl_partition PARTITION OF test.tblparted
    FOR VALUES FROM (0) TO (10000)
    WITH (parallel_workers = 0);

CREATE UNLOGGED TABLE test.tblexpr(i int, updated_at timestamp);
CREATE UNIQUE INDEX tbl_pkey_special ON test.tblexpr(abs(i)) WHERE i < 1000;
ALTER TABLE test.tblexpr SET (parallel_workers=0);

]);

############################################################################
note('Test: REINDEX CONCURRENTLY + UPSERT (wakeup at set-dead phase)');

# Create sessions with on_error_stop => 0 so psql doesn't exit on SQL errors.
# This allows us to collect stderr and detect errors after the test completes.
my $s1 = $node->background_psql('postgres', on_error_stop => 0);
my $s2 = $node->background_psql('postgres', on_error_stop => 0);
my $s3 = $node->background_psql('postgres', on_error_stop => 0);

# Setup injection points for each session
$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-set-dead', 'wait');
]);

# s3 starts REINDEX (will block on reindex-relation-concurrently-before-set-dead)
$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tblpk_pkey;
]);

# Wait for s3 to hit injection point
ok_injection_point($node, 'reindex-relation-concurrently-before-set-dead');

# s1 starts UPSERT (will block on check-exclusion-or-unique-constraint-no-conflict)
$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

# Wait for s1 to hit injection point
ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

# Wakeup s3 to continue (reindex-relation-concurrently-before-set-dead)
wakeup_injection_point($node,
	'reindex-relation-concurrently-before-set-dead');

# s2 starts UPSERT (will block on exec-insert-before-insert-speculative)
$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

# Wait for s2 to hit injection point
ok_injection_point($node, 'exec-insert-before-insert-speculative');

# Wakeup s1 (check-exclusion-or-unique-constraint-no-conflict)
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');

# Wakeup s2 (exec-insert-before-insert-speculative)
wakeup_injection_point($node, 'exec-insert-before-insert-speculative');

clean_safe_quit_ok($s1, $s2, $s3);

# Cleanup test 1
$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblpk');

############################################################################
note('Test: REINDEX CONCURRENTLY + UPSERT (wakeup at swap phase)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tblpk_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-swap');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

wakeup_injection_point($node, 'reindex-relation-concurrently-before-swap');

$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

wakeup_injection_point($node, 'exec-insert-before-insert-speculative');
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblpk');

############################################################################
note('Test: REINDEX CONCURRENTLY + UPSERT (s1 wakes before reindex)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-set-dead', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tblpk_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-set-dead');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

# Start s2 BEFORE waking reindex (key difference from permutation 1)
$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

# Wake s1 first, then reindex, then s2
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');
wakeup_injection_point($node,
	'reindex-relation-concurrently-before-set-dead');
wakeup_injection_point($node, 'exec-insert-before-insert-speculative');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblpk');

############################################################################
note('Test: REINDEX + UPSERT ON CONSTRAINT (set-dead phase)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-set-dead', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tblpk_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-set-dead');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblpk VALUES (13, now()) ON CONFLICT ON CONSTRAINT tblpk_pkey DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

wakeup_injection_point($node,
	'reindex-relation-concurrently-before-set-dead');

$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblpk VALUES (13, now()) ON CONFLICT ON CONSTRAINT tblpk_pkey DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');
wakeup_injection_point($node, 'exec-insert-before-insert-speculative');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblpk');

############################################################################
note('Test: REINDEX + UPSERT ON CONSTRAINT (swap phase)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tblpk_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-swap');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblpk VALUES (13, now()) ON CONFLICT ON CONSTRAINT tblpk_pkey DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

wakeup_injection_point($node, 'reindex-relation-concurrently-before-swap');

$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblpk VALUES (13, now()) ON CONFLICT ON CONSTRAINT tblpk_pkey DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

wakeup_injection_point($node, 'exec-insert-before-insert-speculative');
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblpk');

############################################################################
note('Test: REINDEX + UPSERT ON CONSTRAINT (s1 wakes before reindex)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-set-dead', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tblpk_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-set-dead');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblpk VALUES (13, now()) ON CONFLICT ON CONSTRAINT tblpk_pkey DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

# Start s2 BEFORE waking reindex
$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblpk VALUES (13, now()) ON CONFLICT ON CONSTRAINT tblpk_pkey DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

# Wake s1 first, then reindex, then s2
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');
wakeup_injection_point($node,
	'reindex-relation-concurrently-before-set-dead');
wakeup_injection_point($node, 'exec-insert-before-insert-speculative');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblpk');

############################################################################
note('Test: REINDEX on partitioned table (set-dead phase)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-set-dead', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tbl_partition_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-set-dead');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblparted VALUES (13, now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

wakeup_injection_point($node,
	'reindex-relation-concurrently-before-set-dead');

$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblparted VALUES (13, now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');
wakeup_injection_point($node, 'exec-insert-before-insert-speculative');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblparted');

############################################################################
note('Test: REINDEX on partitioned table (swap phase)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tbl_partition_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-swap');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblparted VALUES (13, now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

wakeup_injection_point($node, 'reindex-relation-concurrently-before-swap');

$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblparted VALUES (13, now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

wakeup_injection_point($node, 'exec-insert-before-insert-speculative');
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblparted');

############################################################################
note('Test: REINDEX on partitioned table (s1 wakes before reindex)');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-set-dead', 'wait');
]);

$s3->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tbl_partition_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-set-dead');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblparted VALUES (13, now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

# Start s2 BEFORE waking reindex
$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblparted VALUES (13, now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

# Wake s1 first, then reindex, then s2
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');
wakeup_injection_point($node,
	'reindex-relation-concurrently-before-set-dead');
wakeup_injection_point($node, 'exec-insert-before-insert-speculative');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblparted');

############################################################################
note('Test: REINDEX on partitioned table, cache inval between two get_partition_ancestors');

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-init-partition-after-get-partition-ancestors', 'wait');
]);

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
]);

$s2->query_until(
	qr/starting_reindex/, q[
\echo starting_reindex
REINDEX INDEX CONCURRENTLY test.tbl_partition_pkey;
]);

ok_injection_point($node, 'reindex-relation-concurrently-before-swap');

$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblparted VALUES (13, now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node,
	'exec-init-partition-after-get-partition-ancestors');

wakeup_injection_point($node, 'reindex-relation-concurrently-before-swap');

wakeup_injection_point($node,
	'exec-init-partition-after-get-partition-ancestors');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblparted');

############################################################################
note('Test: CREATE INDEX CONCURRENTLY + UPSERT');
# Uses invalidate-catalog-snapshot-end to test catalog invalidation
# during UPSERT

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

my $s1_pid = $s1->query_safe('SELECT pg_backend_pid()');

# s1 attaches BOTH injection points - the unique constraint check AND catalog snapshot
$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s1->query_until(
	qr/attaching_injection_point/, q[
\echo attaching_injection_point
SELECT injection_points_attach('invalidate-catalog-snapshot-end', 'wait');
]);

# In cases of cache clobbering, s1 may hit the injection point during attach.
# Wait for that session to become idle (attach completed), or wake it up if
# it becomes stuck on injection point.
if (!wait_for_idle($node, $s1_pid))
{
	ok_injection_point(
		$node,
		'invalidate-catalog-snapshot-end',
		's1 hit injection point during attach (cache clobbering mode)');
	$node->safe_psql(
		'postgres', q[
		SELECT injection_points_wakeup('invalidate-catalog-snapshot-end');
	]);
}

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('define-index-before-set-valid', 'wait');
]);

# s3: Start CREATE INDEX CONCURRENTLY (blocks on define-index-before-set-valid)
$s3->query_until(
	qr/starting_create_index/, q[
\echo starting_create_index
CREATE UNIQUE INDEX CONCURRENTLY tbl_pkey_duplicate ON test.tblpk(i);
]);

ok_injection_point($node, 'define-index-before-set-valid');

# s1: Start UPSERT (blocks on invalidate-catalog-snapshot-end)
$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'invalidate-catalog-snapshot-end');

# Wakeup s3 (CREATE INDEX continues, triggers catalog invalidation)
wakeup_injection_point($node, 'define-index-before-set-valid');

# s2: Start UPSERT (blocks on exec-insert-before-insert-speculative)
$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblpk VALUES (13,now()) ON CONFLICT (i) DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');

wakeup_injection_point($node, 'invalidate-catalog-snapshot-end');

ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');

wakeup_injection_point($node, 'exec-insert-before-insert-speculative');

wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblparted');

############################################################################
note('Test: CREATE INDEX CONCURRENTLY on partial index + UPSERT');
# Uses invalidate-catalog-snapshot-end to test catalog invalidation during UPSERT

$s1 = $node->background_psql('postgres', on_error_stop => 0);
$s2 = $node->background_psql('postgres', on_error_stop => 0);
$s3 = $node->background_psql('postgres', on_error_stop => 0);

$s1_pid = $s1->query_safe('SELECT pg_backend_pid()');

# s1 attaches BOTH injection points - the unique constraint check AND catalog snapshot
$s1->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('check-exclusion-or-unique-constraint-no-conflict', 'wait');
]);

$s1->query_until(
	qr/attaching_injection_point/, q[
\echo attaching_injection_point
SELECT injection_points_attach('invalidate-catalog-snapshot-end', 'wait');
]);

# In cases of cache clobbering, s1 may hit the injection point during attach.
# Wait for that session to become idle (attach completed), or wake it up if
# it becomes stuck on injection point.
if (!wait_for_idle($node, $s1_pid))
{
	ok_injection_point(
		$node,
		'invalidate-catalog-snapshot-end',
		's1 hit injection point during attach (cache clobbering mode)');
	$node->safe_psql(
		'postgres', q[
		SELECT injection_points_wakeup('invalidate-catalog-snapshot-end');
	]);
}

$s2->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('exec-insert-before-insert-speculative', 'wait');
]);

$s3->query_safe(
	q[
SELECT injection_points_set_local();
SELECT injection_points_attach('define-index-before-set-valid', 'wait');
]);

# s3: Start CREATE INDEX CONCURRENTLY (blocks on define-index-before-set-valid)
$s3->query_until(
	qr/starting_create_index/, q[
\echo starting_create_index
CREATE UNIQUE INDEX CONCURRENTLY tbl_pkey_special_duplicate ON test.tblexpr(abs(i)) WHERE i < 10000;
]);

ok_injection_point($node, 'define-index-before-set-valid');

# s1: Start UPSERT (blocks on invalidate-catalog-snapshot-end)
$s1->query_until(
	qr/starting_upsert_s1/, q[
\echo starting_upsert_s1
INSERT INTO test.tblexpr VALUES(13,now()) ON CONFLICT (abs(i)) WHERE i < 100 DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'invalidate-catalog-snapshot-end');

# Wakeup s3 (CREATE INDEX continues, triggers catalog invalidation)
wakeup_injection_point($node, 'define-index-before-set-valid');

# s2: Start UPSERT (blocks on exec-insert-before-insert-speculative)
$s2->query_until(
	qr/starting_upsert_s2/, q[
\echo starting_upsert_s2
INSERT INTO test.tblexpr VALUES(13,now()) ON CONFLICT (abs(i)) WHERE i < 100 DO UPDATE SET updated_at = now();
]);

ok_injection_point($node, 'exec-insert-before-insert-speculative');
wakeup_injection_point($node, 'invalidate-catalog-snapshot-end');
ok_injection_point($node, 'check-exclusion-or-unique-constraint-no-conflict');
wakeup_injection_point($node, 'exec-insert-before-insert-speculative');
wakeup_injection_point($node,
	'check-exclusion-or-unique-constraint-no-conflict');

clean_safe_quit_ok($s1, $s2, $s3);

$node->safe_psql('postgres', 'TRUNCATE TABLE test.tblexpr');

done_testing();

############################################################################
# Helper functions
#
############################################################################

# Helper: Wait for a session to hit an injection point.
# Optional second argument is timeout in seconds.
# Returns true if found, false if timeout.
# On timeout, logs diagnostic information about all active queries.
sub wait_for_injection_point
{
	my ($node, $point_name, $timeout) = @_;
	$timeout //= $PostgreSQL::Test::Utils::timeout_default / 2;

	for (my $elapsed = 0; $elapsed < $timeout * 10; $elapsed++)
	{
		my $pid = $node->safe_psql(
			'postgres', qq[
			SELECT pid FROM pg_stat_activity
			WHERE wait_event_type = 'InjectionPoint'
			  AND wait_event = '$point_name'
			LIMIT 1;
		]);
		return 1 if $pid ne '';
		usleep(100_000);
	}

	# Timeout - report diagnostic information
	my $activity = $node->safe_psql(
		'postgres', q[
		SELECT format('pid=%s, state=%s, wait_event_type=%s, wait_event=%s, backend_xmin=%s, backend_xid=%s, query=%s',
			pid, state, wait_event_type, wait_event, backend_xmin, backend_xid, left(query, 100))
		FROM pg_stat_activity
		ORDER BY pid;
	]);
	diag(   "wait_for_injection_point timeout waiting for: $point_name\n"
		  . "Current queries in pg_stat_activity:\n$activity");

	return 0;
}

# Test helper: ok() a wait for the given injection point
# Third argument is an optional test name.
sub ok_injection_point
{
	my ($node, $injection_point, $testname) = @_;
	$testname //= "hit injection point $injection_point";

	ok(wait_for_injection_point($node, $injection_point), $testname);
}

# Helper: Wait for a specific backend to become idle.
# Returns true if idle, false if waiting for injection point or timeout.
sub wait_for_idle
{
	my ($node, $pid, $timeout) = @_;
	$timeout //= $PostgreSQL::Test::Utils::timeout_default / 2;

	for (my $elapsed = 0; $elapsed < $timeout * 10; $elapsed++)
	{
		my $result = $node->safe_psql(
			'postgres', qq[
			SELECT state, wait_event_type FROM pg_stat_activity WHERE pid = $pid;
		]);
		my ($state, $wait_event_type) = split(/\|/, $result, 2);
		$state           //= '';
		$wait_event_type //= '';
		return 1 if $state eq 'idle';
		return 0 if $wait_event_type eq 'InjectionPoint';

		usleep(100_000);
	}
	return 0;
}

# Helper: Detach and wakeup an injection point
sub wakeup_injection_point
{
	my ($node, $point_name) = @_;
	$node->safe_psql(
		'postgres', qq[
SELECT injection_points_detach('$point_name');
SELECT injection_points_wakeup('$point_name');
]);
}

# Wait for any pending query to complete, capture stderr, and close the session.
# Returns the stderr output (excluding internal markers).
sub safe_quit
{
	my ($session) = @_;

	# Send a marker and wait for it to ensure any pending query completes
	my $banner = "safe_quit_marker";
	my $banner_match = qr/(^|\n)$banner\r?\n/;

	$session->{stdin} .= "\\echo $banner\n\\warn $banner\n";

	pump_until(
		$session->{run}, $session->{timeout},
		\$session->{stdout}, $banner_match);
	pump_until(
		$session->{run}, $session->{timeout},
		\$session->{stderr}, $banner_match);

	# Capture stderr (excluding the banner)
	my $stderr = $session->{stderr};
	$stderr =~ s/$banner_match//;

	# Close the session
	$session->quit;

	return $stderr;
}

# Helper function: verify that the given sessions exit cleanly.
sub clean_safe_quit_ok
{
	my $i = 1;
	foreach my $session (@_)
	{
		is(safe_quit($session), '', "session " . $i++ . " quit cleanly");
	}
}
