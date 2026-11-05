# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test that incremental backups preserve visibility map changes made by
# VACUUM truncation when both data checksums and wal_log_hints are disabled.
# The restored map must not retain set bits for truncated heap pages.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $mode = $ENV{PG_TEST_PG_COMBINEBACKUP_MODE} || '--copy';

my $primary = PostgreSQL::Test::Cluster->new('primary');
# Enable checksums explicitly, then disable them before starting the server.
# This also works when PG_TEST_INITDB_EXTRA_OPTS enables checksums.
$primary->init(allows_streaming => 1, extra => ['--data-checksums']);
$primary->command_ok(
	[ 'pg_checksums', '--disable', '--no-sync', '-D', $primary->data_dir ],
	'disable data checksums');
$primary->append_conf('postgresql.conf', <<EOF);
summarize_wal = on
wal_log_hints = off
autovacuum = off
EOF
$primary->start;

# Use uncompressed inline rows large enough to fit only one per heap page.
$primary->safe_psql(
	'postgres', q{
	CREATE EXTENSION pg_visibility;
	CREATE TABLE t (id int, pad text STORAGE PLAIN);
	INSERT INTO t
		SELECT g, repeat('x', current_setting('block_size')::int / 2)
		FROM generate_series(1, 8) g;
	DELETE FROM t WHERE id > 2;
});

# Set both VM bits for all pages, keeping the empty tail pages.
$primary->safe_psql('postgres', 'VACUUM (FREEZE, TRUNCATE false) t');

my $heap_blocks_sql =
  q{SELECT pg_relation_size('t') / current_setting('block_size')::int};

# Check all eight original heap blocks, including those removed by truncation.
my $vm_summary_sql = q{
  SELECT count(*) FILTER (WHERE all_visible), count(*) FILTER (WHERE all_frozen)
  FROM generate_series(0, 7) AS b, pg_visibility_map('t', b)};

is($primary->safe_psql('postgres', $heap_blocks_sql),
	'8', 'empty tail pages remain before full backup');
is($primary->safe_psql('postgres', $vm_summary_sql),
	'8|8', 'all pages have both VM bits set before full backup');

$primary->backup('full');

$primary->safe_psql('postgres', 'VACUUM (TRUNCATE) t');
is($primary->safe_psql('postgres', $heap_blocks_sql),
	'2', 'VACUUM truncates the empty tail pages');
is($primary->safe_psql('postgres', $vm_summary_sql),
	'2|2', 'VACUUM clears VM bits for the removed heap pages');
is( $primary->safe_psql(
		'postgres',
		q{SELECT pg_relation_size('t', 'vm') / current_setting('block_size')::int}
	),
	'1',
	'VM fork still contains one page after truncation');

my $full_backup = $primary->backup_dir . '/full';
$primary->backup('incr',
	backup_options => [ '--incremental', "$full_backup/backup_manifest" ]);
$primary->stop;

my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup(
	$primary, 'incr',
	combine_with_prior => ['full'],
	combine_mode => $mode);
$restored->start;

# Count all VM bits, including those beyond the end of the heap, to
# detect stale bits inherited from the full backup.
is($restored->safe_psql('postgres', $heap_blocks_sql),
	'2', 'restored heap has the truncated length');
is($restored->safe_psql('postgres', $vm_summary_sql),
	'2|2', 'restored VM has no stale bits for removed heap pages');

$restored->stop;
done_testing();
